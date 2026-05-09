#include "tokenizer_thread_pool.hpp"

#include <chrono>

#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>

namespace ranvier {

static seastar::logger log_thread_pool("tokenizer_pool");

// ============================================================================
// TokenizerWorker Implementation
// ============================================================================

TokenizerWorker::TokenizerWorker(uint32_t shard_id, size_t queue_size)
    : _shard_id(shard_id)
    , _job_queue(queue_size) {
}

TokenizerWorker::~TokenizerWorker() {
    // Ensure worker thread is stopped before destruction
    if (_thread && _thread->joinable()) {
        log_thread_pool.warn("TokenizerWorker destructor called with running thread "
                            "on shard {} - forcing stop", _shard_id);
        stop();
    }
}

void TokenizerWorker::load_tokenizer(const std::string& json_content) {
    // Load tokenizer on the main thread before worker starts
    // This ensures the tokenizer is ready when the worker thread begins
    try {
        _tokenizer = tokenizers::Tokenizer::FromBlobJSON(json_content);
        if (!_tokenizer) {
            log_thread_pool.error("TokenizerWorker::load_tokenizer() returned null "
                                 "tokenizer on shard {}", _shard_id);
        }
    } catch (const std::exception& e) {
        // Rule #9: Log exceptions at warn level with context
        log_thread_pool.warn("Failed to load tokenizer on shard {}: {}",
                            _shard_id, e.what());
        _tokenizer.reset();
    }
}

void TokenizerWorker::start(seastar::alien::instance& alien_instance) {
    if (_running.load(std::memory_order_acquire)) {
        log_thread_pool.warn("TokenizerWorker::start() called on already running "
                            "worker for shard {}", _shard_id);
        return;
    }

    if (!_tokenizer) {
        log_thread_pool.error("TokenizerWorker::start() called without loading "
                             "tokenizer on shard {}", _shard_id);
        return;
    }

    _shutdown.store(false, std::memory_order_release);
    _running.store(true, std::memory_order_release);

    // Spawn worker thread.
    //
    // SHUTDOWN-CONTRACT: alien_instance is captured by reference because
    // seastar::alien::instance is a non-movable, non-reference-counted
    // singleton owned by app_template. The worker thread calls
    // alien::run_on(alien_instance, ...) on every completion (see
    // process_job below), so a dangling reference here is undefined behavior.
    //
    // The caller MUST guarantee that stop() is called (which joins this
    // thread) before app_template / the alien instance is torn down. See
    // the class-level comment on TokenizerThreadPool, and the assertion in
    // ~TokenizerThreadPool() that the worker has been stopped.
    _thread = std::make_unique<std::thread>([this, &alien_instance]() {
        worker_loop(alien_instance);
    });

    log_thread_pool.info("TokenizerWorker started for shard {}", _shard_id);
}

void TokenizerWorker::stop() {
    if (!_running.load(std::memory_order_acquire)) {
        return;
    }

    log_thread_pool.info("TokenizerWorker stopping for shard {}...", _shard_id);

    // Signal shutdown
    _shutdown.store(true, std::memory_order_release);

    // Wait for thread to exit
    if (_thread && _thread->joinable()) {
        _thread->join();
    }

    _running.store(false, std::memory_order_release);
    _thread.reset();

    log_thread_pool.info("TokenizerWorker stopped for shard {} "
                        "(processed={}, dropped={})",
                        _shard_id, _jobs_processed.load(), _jobs_dropped.load());
}

bool TokenizerWorker::submit(TokenizationJob job) {
    if (!_running.load(std::memory_order_acquire)) {
        return false;
    }

    // SPSC queue push is lock-free
    if (!_job_queue.push(std::move(job))) {
        // Queue full - backpressure (Rule #4)
        _queue_full_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

void TokenizerWorker::worker_loop(seastar::alien::instance& alien_instance) {
    log_thread_pool.debug("Worker thread started for shard {}", _shard_id);

    constexpr auto SPIN_ITERATIONS = 1000;
    constexpr auto SLEEP_DURATION = std::chrono::microseconds(100);

    while (!_shutdown.load(std::memory_order_acquire)) {
        TokenizationJob job;
        bool got_job = false;

        // Spin briefly before sleeping (reduces latency for bursty workloads)
        for (int i = 0; i < SPIN_ITERATIONS && !_shutdown.load(std::memory_order_acquire); ++i) {
            if (_job_queue.pop(job)) {
                got_job = true;
                break;
            }
        }

        if (!got_job) {
            std::this_thread::sleep_for(SLEEP_DURATION);
            continue;
        }

        process_job(job, alien_instance);
    }

    log_thread_pool.debug("Worker thread exiting for shard {}", _shard_id);
}

void TokenizerWorker::process_job(TokenizationJob& job, seastar::alien::instance& alien_instance) {
    std::vector<int32_t> tokens;
    bool success = false;

    // Reallocate the string on the worker thread so the reactor shard's
    // per-shard memory isn't held across the FFI call. Rust itself uses
    // jemalloc (patched at build time) and is allocator-safe, but keeping
    // the copy avoids pinning shard-local memory on a foreign thread.
    std::string local_text(job.text.data(), job.text.size());

    try {
        tokens = _tokenizer->Encode(local_text);
        success = true;
    } catch (const std::exception& e) {
        log_thread_pool.warn("Worker tokenization failed on shard {}: {}",
                            _shard_id, e.what());
    }

    _jobs_processed.fetch_add(1, std::memory_order_relaxed);

    uint32_t target_shard = job.source_shard;
    uint64_t job_id = job.job_id;

    try {
        seastar::alien::run_on(alien_instance, target_shard,
            [job_id, tokens = std::move(tokens), success]() noexcept {
                if (auto* callback = get_thread_pool_completion_callback()) {
                    // Copy tokens into shard-local memory. The captured vector was
                    // allocated on the worker thread (foreign_malloc). Copying here
                    // keeps the hot path on the shard's own allocator and avoids
                    // do_foreign_free overhead on the reactor.
                    std::vector<int32_t> local_tokens(tokens.begin(), tokens.end());
                    (*callback)(job_id, std::move(local_tokens), success);
                }
            });
    } catch (const std::exception& e) {
        log_thread_pool.warn("Failed to signal completion for job {} on shard {}: {}",
                            job_id, target_shard, e.what());
        _jobs_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// Thread-local callback for worker → reactor completion signaling.
// Set by TokenizerThreadPool::start_worker(), cleared on stop.
namespace {
    thread_local std::function<void(uint64_t, std::vector<int32_t>, bool)>*
        tl_completion_callback = nullptr;
}

std::function<void(uint64_t, std::vector<int32_t>, bool)>*
get_thread_pool_completion_callback() {
    return tl_completion_callback;
}

void set_thread_pool_completion_callback(
    std::function<void(uint64_t, std::vector<int32_t>, bool)>* callback) {
    tl_completion_callback = callback;
}

// ============================================================================
// TokenizerThreadPool Implementation
// ============================================================================

TokenizerThreadPool::TokenizerThreadPool(ThreadPoolTokenizationConfig config)
    : _config(config)
    , _shard_id(seastar::this_shard_id()) {
}

TokenizerThreadPool::~TokenizerThreadPool() {
    // SHUTDOWN-CONTRACT enforcement: the worker thread captures the alien
    // instance by reference; if it is still alive at destruction time, the
    // caller skipped stop_worker() and the alien may be torn down before
    // the thread exits. Force a stop with a loud warning so we don't UAF
    // the alien from a foreign thread.
    if (_worker) {
        log_thread_pool.error("~TokenizerThreadPool on shard {}: stop_worker() "
                              "was not called before destruction; forcing stop "
                              "(see SHUTDOWN-CONTRACT in tokenizer_thread_pool.hpp)",
                              _shard_id);
        stop_worker();
    }
}

seastar::future<> TokenizerThreadPool::stop() {
    // Rule #6: Deregister metrics first
    _metrics.clear();

    // Clear pending jobs (they will never complete)
    for (auto& [job_id, promise] : _pending_jobs) {
        // Set empty result to unblock waiters
        ThreadPoolTokenizationResult empty_result;
        promise.set_value(std::move(empty_result));
    }
    _pending_jobs.clear();

    // Clear completion callback
    set_thread_pool_completion_callback(nullptr);

    return seastar::make_ready_future<>();
}

void TokenizerThreadPool::load_tokenizer(const std::string& json_content) {
    _tokenizer_json = json_content;
}

void TokenizerThreadPool::start_worker(seastar::alien::instance& alien_instance) {
    if (!_config.enabled) {
        log_thread_pool.debug("Thread pool disabled on shard {}", _shard_id);
        return;
    }

    if (_tokenizer_json.empty()) {
        log_thread_pool.error("Cannot start worker on shard {}: tokenizer JSON not loaded",
                             _shard_id);
        return;
    }

    _worker = std::make_unique<TokenizerWorker>(_shard_id, _config.max_queue_size);
    _worker->load_tokenizer(_tokenizer_json);

    // Static thread_local ensures callback lifetime (Rule #13)
    static thread_local std::function<void(uint64_t, std::vector<int32_t>, bool)> callback;
    callback = [this](uint64_t job_id, std::vector<int32_t> tokens, bool success) {
        complete_job(job_id, std::move(tokens), success);
    };
    set_thread_pool_completion_callback(&callback);
    _worker->start(alien_instance);

    log_thread_pool.info("Thread pool worker started on shard {}", _shard_id);
}

void TokenizerThreadPool::stop_worker() {
    if (_worker) {
        _worker->stop();
        _worker.reset();
    }
    set_thread_pool_completion_callback(nullptr);
    log_thread_pool.debug("Thread pool worker stopped on shard {}", _shard_id);
}

bool TokenizerThreadPool::should_use_thread_pool(size_t text_length) const {
    if (!_config.enabled) {
        return false;
    }

    if (!_worker || !_worker->is_running()) {
        return false;
    }

    if (text_length < _config.min_text_length ||
        text_length > _config.max_text_length) {
        return false;
    }

    return true;
}

std::optional<seastar::future<ThreadPoolTokenizationResult>>
TokenizerThreadPool::submit_async(std::string_view text) {
    if (!should_use_thread_pool(text.size())) {
        return std::nullopt;
    }

    // Double-check worker is still valid (defensive against race during shutdown)
    if (!_worker) {
        return std::nullopt;
    }

    // Create job with owned copy of text (Rule #14)
    uint64_t job_id = _next_job_id++;
    TokenizationJob job{
        .job_id = job_id,
        .text = std::string(text),
        .source_shard = _shard_id
    };

    // Submit to worker queue
    if (!_worker->submit(std::move(job))) {
        // Queue full - fallback to local tokenization
        ++_jobs_fallback;
        return std::nullopt;
    }

    ++_jobs_submitted;

    // Create promise/future pair
    seastar::promise<ThreadPoolTokenizationResult> promise;
    auto future = promise.get_future();

    // Store promise for completion callback (shard-local map, no locks)
    _pending_jobs.emplace(job_id, std::move(promise));

    return future;
}

void TokenizerThreadPool::complete_job(
    uint64_t job_id, std::vector<int32_t> tokens, bool success) {
    // Find and complete the pending promise
    auto it = _pending_jobs.find(job_id);
    if (it == _pending_jobs.end()) {
        // Job was cancelled or timed out
        log_thread_pool.debug("Completion for unknown job {} on shard {}",
                             job_id, _shard_id);
        return;
    }

    // Log failed tokenizations (Rule #9)
    if (!success) {
        log_thread_pool.warn("Tokenization job {} failed on shard {} "
                            "(returning empty tokens)", job_id, _shard_id);
    }

    ThreadPoolTokenizationResult result;
    result.tokens = std::move(tokens);
    result.cache_hit = false;
    result.thread_pool = true;
    result.source_shard = _shard_id;

    // Set the promise value (unblocks the waiting future)
    it->second.set_value(std::move(result));

    // Remove from pending map
    _pending_jobs.erase(it);
    ++_jobs_completed;
}

void TokenizerThreadPool::register_metrics() {
    namespace sm = seastar::metrics;
    _metrics.add_group("ranvier_tokenizer_thread_pool", {
        sm::make_counter("jobs_submitted", _jobs_submitted,
            sm::description("Total tokenization jobs submitted to thread pool")),
        sm::make_counter("jobs_completed", _jobs_completed,
            sm::description("Total tokenization jobs completed by thread pool")),
        sm::make_counter("jobs_fallback", _jobs_fallback,
            sm::description("Jobs that fell back to local tokenization (queue full)")),
        sm::make_gauge("pending_jobs", [this] { return _pending_jobs.size(); },
            sm::description("Currently pending jobs awaiting completion")),
        sm::make_gauge("worker_running", [this] {
            return (_worker && _worker->is_running()) ? 1.0 : 0.0;
        }, sm::description("Whether the worker thread is running (1=yes, 0=no)")),
        sm::make_gauge("enabled", [this] { return _config.enabled ? 1.0 : 0.0; },
            sm::description("Whether thread pool tokenization is enabled")),
    });
}

}  // namespace ranvier
