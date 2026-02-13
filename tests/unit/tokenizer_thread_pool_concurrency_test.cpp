// Ranvier Core - Tokenizer Thread Pool Concurrency Stress Tests
//
// Exercises concurrent access to TokenizerWorker's atomic statistics
// and TokenizerThreadPool's config accessors. The worker requires
// Seastar's alien API to start (sets _running=true), so without it
// submit() rejects immediately. These tests focus on:
//   - Atomic stat reads (jobs_processed, jobs_dropped, queue_full_count)
//     concurrent with submit() rejection path
//   - should_use_thread_pool() concurrent access (returns false without
//     a running worker, but must not crash under contention)
//   - Config accessor thread safety (const, no shared mutable state)

#include "tokenizer_thread_pool.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

using namespace ranvier;

// =============================================================================
// TokenizerWorker Atomic Statistics Concurrency Tests
// =============================================================================

class TokenizerWorkerConcurrencyTest : public ::testing::Test {
protected:
    static constexpr int kNumReaders = 4;
    static constexpr int kSubmitOps = 10'000;
};

TEST_F(TokenizerWorkerConcurrencyTest, ConcurrentSubmitAndStatReads) {
    // Single producer submits (all rejected since worker not started).
    // Multiple readers concurrently check atomic statistics.
    // Validates no data race between submit rejection path and stat reads.
    constexpr size_t kSmallQueue = 64;
    TokenizerWorker worker(0, kSmallQueue);
    // Worker is not started (requires Seastar alien instance), so submit()
    // returns false immediately (guarded by _running check).

    std::atomic<bool> running{true};
    std::latch start_latch(kNumReaders + 1);
    std::vector<std::thread> threads;

    // Stat reader threads
    std::atomic<uint64_t> reads_performed{0};
    for (int r = 0; r < kNumReaders; ++r) {
        threads.emplace_back([&worker, &start_latch, &running, &reads_performed]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                // These are lock-free atomic reads
                [[maybe_unused]] auto processed = worker.jobs_processed();
                [[maybe_unused]] auto dropped = worker.jobs_dropped();
                [[maybe_unused]] auto full = worker.queue_full_count();
                [[maybe_unused]] auto is_running = worker.is_running();
                reads_performed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Single producer thread (all submits rejected: worker not running)
    uint64_t rejected = 0;
    threads.emplace_back([&worker, &start_latch, &running, &rejected]() {
        start_latch.arrive_and_wait();
        for (int i = 0; i < kSubmitOps; ++i) {
            TokenizationJob job;
            job.job_id = static_cast<uint64_t>(i);
            job.text = "test text " + std::to_string(i);
            job.source_shard = 0;

            if (!worker.submit(std::move(job))) {
                rejected++;
            }
        }
        running.store(false, std::memory_order_relaxed);
    });

    for (auto& thread : threads) {
        thread.join();
    }

    // All submits rejected (worker not running)
    EXPECT_EQ(rejected, static_cast<uint64_t>(kSubmitOps));
    EXPECT_GT(reads_performed.load(), 0u);
    // Stats remain at zero (no jobs were processed)
    EXPECT_EQ(worker.jobs_processed(), 0u);
}

TEST_F(TokenizerWorkerConcurrencyTest, StatisticsAreConsistentUnderContention) {
    // Verify that atomic stats don't produce torn reads.
    // Worker not started, so jobs_processed stays 0.
    TokenizerWorker worker(0, 128);

    std::atomic<bool> running{true};
    std::latch start_latch(kNumReaders);
    std::vector<std::thread> threads;

    for (int r = 0; r < kNumReaders; ++r) {
        threads.emplace_back([&worker, &start_latch, &running]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                uint64_t processed = worker.jobs_processed();
                uint64_t dropped = worker.jobs_dropped();
                uint64_t full = worker.queue_full_count();

                // Worker never started, so processed should stay 0
                EXPECT_EQ(processed, 0u);
                // With no submissions yet, dropped and full should be 0
                EXPECT_EQ(dropped, 0u);
                EXPECT_EQ(full, 0u);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_relaxed);

    for (auto& thread : threads) {
        thread.join();
    }
}

// =============================================================================
// TokenizerThreadPool Configuration Concurrency Tests
// =============================================================================

class TokenizerThreadPoolConfigConcurrencyTest : public ::testing::Test {
protected:
    static constexpr int kNumThreads = 8;
    static constexpr int kOpsPerThread = 10'000;
};

TEST_F(TokenizerThreadPoolConfigConcurrencyTest, ShouldUseThreadPoolIsThreadSafe) {
    // should_use_thread_pool() checks config AND worker readiness.
    // Without a running worker, it always returns false. We verify it
    // doesn't crash under concurrent access and returns consistent results.
    ThreadPoolTokenizationConfig config;
    config.enabled = true;
    config.min_text_length = 256;
    config.max_text_length = 65536;
    TokenizerThreadPool pool(config);

    std::latch start_latch(kNumThreads);
    std::atomic<int> false_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&pool, &start_latch, &false_count, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                size_t len = (t * kOpsPerThread + i) % 100000;
                if (!pool.should_use_thread_pool(len)) {
                    false_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Worker is not running, so all calls must return false
    EXPECT_EQ(false_count.load(), kNumThreads * kOpsPerThread);
}

TEST_F(TokenizerThreadPoolConfigConcurrencyTest, ConfigAccessorsAreThreadSafe) {
    // All const accessors should be safe to call concurrently.
    // Accumulate failures per thread instead of asserting in the hot loop
    // to avoid flooding gtest output on failure (8 threads x 10k iterations).
    ThreadPoolTokenizationConfig config;
    config.enabled = true;
    config.min_text_length = 100;
    config.max_text_length = 50000;
    config.max_queue_size = 512;
    TokenizerThreadPool pool(config);

    std::latch start_latch(kNumThreads);
    std::atomic<int> correct_threads{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&pool, &start_latch, &correct_threads]() {
            start_latch.arrive_and_wait();
            bool all_ok = true;
            for (int i = 0; i < kOpsPerThread; ++i) {
                if (!pool.enabled() ||
                    pool.min_text_length() != 100u ||
                    pool.max_text_length() != 50000u ||
                    pool.jobs_submitted() != 0u ||
                    pool.jobs_completed() != 0u ||
                    pool.jobs_fallback() != 0u) {
                    all_ok = false;
                    break;
                }
            }
            if (all_ok) {
                correct_threads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(correct_threads.load(), kNumThreads);
}

// =============================================================================
// TokenizerWorker Queue Boundary Tests Under Contention
// =============================================================================

TEST_F(TokenizerWorkerConcurrencyTest, SubmitRejectsWhenNotRunning) {
    // submit() guards on _running. When worker is not started, all submits
    // are rejected. This is the expected safety behavior.
    constexpr size_t kQueueSize = 128;
    TokenizerWorker worker(0, kQueueSize);

    // Worker not started - submit must return false
    EXPECT_FALSE(worker.is_running());

    uint64_t rejected = 0;
    for (size_t i = 0; i < kQueueSize; ++i) {
        TokenizationJob job;
        job.job_id = i;
        job.text = "text";
        job.source_shard = 0;
        if (!worker.submit(std::move(job))) {
            rejected++;
        }
    }

    EXPECT_EQ(rejected, kQueueSize);
}

TEST_F(TokenizerWorkerConcurrencyTest, ConcurrentMultiProducerRejection) {
    // Multiple producer threads all submit simultaneously.
    // Worker is not started, so all submits are rejected.
    // Validates no race in the _running check path with multiple producers.
    constexpr int kProducers = 4;
    constexpr int kJobsPerProducer = 5'000;
    TokenizerWorker worker(0, 256);

    std::latch start_latch(kProducers);
    std::atomic<uint64_t> total_rejected{0};
    std::vector<std::thread> threads;

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&worker, &start_latch, &total_rejected, p]() {
            start_latch.arrive_and_wait();
            uint64_t local_rejected = 0;
            for (int i = 0; i < kJobsPerProducer; ++i) {
                TokenizationJob job;
                job.job_id = static_cast<uint64_t>(p * kJobsPerProducer + i);
                job.text = "multi-producer text " + std::to_string(i);
                job.source_shard = static_cast<uint32_t>(p);

                if (!worker.submit(std::move(job))) {
                    local_rejected++;
                }
            }
            total_rejected.fetch_add(local_rejected, std::memory_order_relaxed);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All submits rejected: worker not running
    EXPECT_EQ(total_rejected.load(),
              static_cast<uint64_t>(kProducers) * kJobsPerProducer);
    EXPECT_EQ(worker.jobs_processed(), 0u);
    EXPECT_FALSE(worker.is_running());
}

TEST_F(TokenizerWorkerConcurrencyTest, StopOnNonStartedWorkerConcurrentWithReads) {
    // Call stop() on a worker that was never started while stat readers run.
    // Validates stop() is safe when worker thread doesn't exist.
    TokenizerWorker worker(0, 128);
    ASSERT_FALSE(worker.is_running());

    std::atomic<bool> done{false};
    std::latch start_latch(kNumReaders + 1);

    std::vector<std::thread> readers;
    for (int r = 0; r < kNumReaders; ++r) {
        readers.emplace_back([&worker, &start_latch, &done]() {
            start_latch.arrive_and_wait();
            while (!done.load(std::memory_order_acquire)) {
                [[maybe_unused]] auto p = worker.jobs_processed();
                [[maybe_unused]] auto d = worker.jobs_dropped();
                [[maybe_unused]] auto r = worker.is_running();
            }
        });
    }

    // Stop thread
    std::thread stopper([&worker, &start_latch, &done]() {
        start_latch.arrive_and_wait();
        worker.stop();  // Should be safe on non-started worker
        done.store(true, std::memory_order_release);
    });

    for (auto& t : readers) {
        t.join();
    }
    stopper.join();

    EXPECT_FALSE(worker.is_running());
}

TEST_F(TokenizerWorkerConcurrencyTest, SubmitAndReadStatsNoCrash) {
    // Rapidly submit from one thread, read stats from another.
    // Validates no data race between submit path and stat reads.
    constexpr size_t kQueueSize = 256;
    TokenizerWorker worker(0, kQueueSize);

    std::atomic<bool> done{false};
    std::latch start_latch(2);

    std::thread writer([&worker, &start_latch, &done]() {
        start_latch.arrive_and_wait();
        for (int i = 0; i < 50'000; ++i) {
            TokenizationJob job;
            job.job_id = static_cast<uint64_t>(i);
            job.text = "t";
            job.source_shard = 0;
            worker.submit(std::move(job));
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&worker, &start_latch, &done]() {
        start_latch.arrive_and_wait();
        while (!done.load(std::memory_order_acquire)) {
            [[maybe_unused]] auto p = worker.jobs_processed();
            [[maybe_unused]] auto d = worker.jobs_dropped();
            [[maybe_unused]] auto q = worker.queue_full_count();
            [[maybe_unused]] auto r = worker.is_running();
        }
    });

    writer.join();
    reader.join();
}
