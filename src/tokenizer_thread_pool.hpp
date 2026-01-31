// Ranvier Core - Tokenizer Thread Pool
//
// Dedicated thread pool for offloading tokenization FFI calls outside Seastar's
// event loop. Each shard gets exactly one worker thread with its own tokenizer
// instance (tokenizer is NOT thread-safe).
//
// Architecture:
//   Reactor Thread (shard N)           Worker Thread N
//   ─────────────────────────          ─────────────────
//   1. Create promise<Result>
//   2. Enqueue (job_id, text) ──────► 3. Dequeue job
//   3. Return future to caller        4. Tokenize (BLOCKING FFI)
//      ↓                              5. alien::run_on(shard_N, complete)
//   [suspended]                          │
//      ↓                                 │
//   6. complete(job_id, tokens) ◄────────┘
//   7. promise.set_value(tokens)
//   8. Future resolves
//
// This approach frees ALL reactor threads during tokenization, unlike cross-shard
// dispatch which still blocks the target shard's reactor.
//
// References:
// - Seastar alien API: https://docs.seastar.io/master/namespaceseastar_1_1alien.html
// - Ceph crimson thread pool: https://github.com/ceph/ceph/pull/22565

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/lockfree/spsc_queue.hpp>
#include <seastar/core/future.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_registration.hh>
#include <tokenizers_cpp.h>

namespace ranvier {

/**
 * Configuration for the tokenizer thread pool.
 */
struct ThreadPoolTokenizationConfig {
    bool enabled = false;  // Disabled by default; benchmark first (P3 priority)

    // Maximum pending jobs in the SPSC queue per shard (Rule #4: bounded container)
    // When queue is full, fallback to local tokenization (backpressure)
    size_t max_queue_size = 256;

    // Minimum text length (bytes) to consider thread pool dispatch.
    // Short texts tokenize quickly; thread pool overhead may exceed benefit.
    size_t min_text_length = 256;

    // Maximum text length (bytes) for thread pool dispatch.
    // Very long texts require large string copies; may be better processed locally.
    size_t max_text_length = 65536;
};

/**
 * Result from thread pool tokenization with metadata.
 */
struct ThreadPoolTokenizationResult {
    std::vector<int32_t> tokens;
    bool cache_hit = false;       // True if served from local cache
    bool thread_pool = false;     // True if tokenized via thread pool
    uint32_t source_shard = 0;    // Shard that owns the worker thread
};

/**
 * Job submitted to the worker thread queue.
 * Contains owned copies of all data (Rule #14: safe cross-thread data).
 */
struct TokenizationJob {
    uint64_t job_id;        // Unique job identifier for promise lookup
    std::string text;       // Owned copy of text to tokenize
    uint32_t source_shard;  // Shard that submitted the job (for alien::run_on)
};

/**
 * Result returned from worker thread to reactor.
 */
struct TokenizationJobResult {
    uint64_t job_id;
    std::vector<int32_t> tokens;
    bool success;
};

/**
 * Per-shard worker that runs tokenization in a dedicated OS thread.
 *
 * Design rationale:
 * - One worker per shard ensures no contention (tokenizer is NOT thread-safe)
 * - SPSC queue is lock-free for single producer (reactor) single consumer (worker)
 * - Worker uses alien::run_on() to post completion back to reactor
 *
 * Memory safety:
 * - Job text is copied into queue (owned string)
 * - Results are copied back via alien::run_on()
 * - Worker thread uses default allocator (not Seastar's per-shard allocator)
 *
 * Lifecycle:
 * - start() spawns the worker thread
 * - stop() sets shutdown flag and joins thread
 * - stop() must complete before destroying the TokenizerThreadPool
 */
class TokenizerWorker {
public:
    // Rule #4: bounded queue size
    static constexpr size_t DEFAULT_QUEUE_SIZE = 256;

    explicit TokenizerWorker(uint32_t shard_id, size_t queue_size = DEFAULT_QUEUE_SIZE);
    ~TokenizerWorker();

    // Non-copyable, non-movable (owns thread and tokenizer)
    TokenizerWorker(const TokenizerWorker&) = delete;
    TokenizerWorker& operator=(const TokenizerWorker&) = delete;
    TokenizerWorker(TokenizerWorker&&) = delete;
    TokenizerWorker& operator=(TokenizerWorker&&) = delete;

    // Load tokenizer from JSON (must be called before start())
    void load_tokenizer(const std::string& json_content);

    // Start the worker thread
    void start(seastar::alien::instance& alien_instance);

    // Stop the worker thread (blocks until thread exits)
    void stop();

    // Submit a job to the worker queue
    // Returns false if queue is full (backpressure)
    bool submit(TokenizationJob job);

    // Check if worker is running
    bool is_running() const { return _running.load(std::memory_order_acquire); }

    // Statistics (lock-free via atomics, Rule #1)
    uint64_t jobs_processed() const { return _jobs_processed.load(std::memory_order_relaxed); }
    uint64_t jobs_dropped() const { return _jobs_dropped.load(std::memory_order_relaxed); }
    uint64_t queue_full_count() const { return _queue_full_count.load(std::memory_order_relaxed); }

private:
    // Worker thread main loop
    void worker_loop(seastar::alien::instance& alien_instance);

    // Process a single job and signal completion
    void process_job(TokenizationJob& job, seastar::alien::instance& alien_instance);

    uint32_t _shard_id;

    // SPSC queue for job submission (reactor → worker)
    // Rule #4: bounded size prevents OOM
    boost::lockfree::spsc_queue<TokenizationJob> _job_queue;

    // Worker thread state
    std::unique_ptr<std::thread> _thread;  // Rule #0: unique_ptr for ownership
    std::atomic<bool> _running{false};     // Rule #11: atomic for cross-thread visibility
    std::atomic<bool> _shutdown{false};

    // Worker's own tokenizer instance (tokenizer is NOT thread-safe)
    std::unique_ptr<tokenizers::Tokenizer> _tokenizer;

    // Statistics (Rule #1: lock-free metrics)
    std::atomic<uint64_t> _jobs_processed{0};
    std::atomic<uint64_t> _jobs_dropped{0};
    std::atomic<uint64_t> _queue_full_count{0};
};

/**
 * Thread pool managing per-shard tokenizer workers.
 *
 * This is a shard-local service (one instance per Seastar shard). Each instance
 * manages a single worker thread for its shard.
 *
 * Usage pattern:
 *   seastar::sharded<TokenizerThreadPool> _thread_pool;
 *   _thread_pool.start(config);
 *   _thread_pool.invoke_on_all([&json](TokenizerThreadPool& p) {
 *       p.load_tokenizer(json);
 *       p.start_worker();
 *   });
 *
 * Shutdown pattern:
 *   _thread_pool.invoke_on_all([](TokenizerThreadPool& p) {
 *       p.stop_worker();  // Blocks until worker thread exits
 *   }).then([this] {
 *       return _thread_pool.stop();
 *   });
 */
class TokenizerThreadPool {
public:
    explicit TokenizerThreadPool(ThreadPoolTokenizationConfig config = {});

    // Seastar sharded service requirement
    seastar::future<> stop();

    // Load tokenizer JSON for the worker (must be called before start_worker())
    void load_tokenizer(const std::string& json_content);

    // Start the worker thread for this shard
    // alien_instance: reference to Seastar's alien instance for cross-thread signaling
    void start_worker(seastar::alien::instance& alien_instance);

    // Stop the worker thread (blocks until thread exits)
    // Must be called before stop() to ensure clean shutdown
    void stop_worker();

    // Submit tokenization job and get future for result
    // Returns nullopt if thread pool is disabled, not ready, or queue full
    // Caller should fallback to local tokenization in that case
    std::optional<seastar::future<ThreadPoolTokenizationResult>> submit_async(
        std::string_view text);

    // Configuration accessors
    bool enabled() const { return _config.enabled; }
    size_t min_text_length() const { return _config.min_text_length; }
    size_t max_text_length() const { return _config.max_text_length; }

    // Check if text qualifies for thread pool dispatch
    bool should_use_thread_pool(size_t text_length) const;

    // Statistics accessors (for metrics)
    uint64_t jobs_submitted() const { return _jobs_submitted; }
    uint64_t jobs_completed() const { return _jobs_completed; }
    uint64_t jobs_fallback() const { return _jobs_fallback; }
    uint64_t worker_jobs_processed() const {
        return _worker ? _worker->jobs_processed() : 0;
    }
    uint64_t worker_queue_full() const {
        return _worker ? _worker->queue_full_count() : 0;
    }

    // Register Prometheus metrics
    void register_metrics();

private:
    // Complete a job when worker signals back
    void complete_job(uint64_t job_id, std::vector<int32_t> tokens, bool success);

    ThreadPoolTokenizationConfig _config;
    uint32_t _shard_id;

    // Per-shard worker (Rule #0: unique_ptr for ownership)
    std::unique_ptr<TokenizerWorker> _worker;

    // Pending jobs awaiting completion (job_id → promise)
    // Shard-local, no locks needed
    std::unordered_map<uint64_t, seastar::promise<ThreadPoolTokenizationResult>> _pending_jobs;

    // Job ID counter (shard-local, no atomics needed)
    uint64_t _next_job_id = 0;

    // Statistics (shard-local, lock-free)
    uint64_t _jobs_submitted = 0;
    uint64_t _jobs_completed = 0;
    uint64_t _jobs_fallback = 0;  // Jobs that fell back to local tokenization

    // Prometheus metrics registration
    seastar::metrics::metric_groups _metrics;

    // Tokenizer JSON cached for worker initialization
    std::string _tokenizer_json;
};

// ============================================================================
// Thread-local callback for worker → reactor completion signaling
// ============================================================================

// Get the completion callback for the current shard
// Returns nullptr if no thread pool is active on this shard
std::function<void(uint64_t, std::vector<int32_t>, bool)>*
get_thread_pool_completion_callback();

// Set the completion callback for the current shard
// Called by TokenizerThreadPool::start_worker()
void set_thread_pool_completion_callback(
    std::function<void(uint64_t, std::vector<int32_t>, bool)>* callback);

}  // namespace ranvier
