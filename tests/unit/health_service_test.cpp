// Ranvier Core - Health Service Unit Tests
//
// Tests pure storage and lookup logic from HealthService.
// The store/get functions are replicated here to avoid Seastar dependencies,
// following the same pattern as local_discovery_test.cpp.
//
// What IS tested here:
//   - store_vllm_metrics() bounded container enforcement (Rule #4)
//   - Overflow drop counter
//   - get_vllm_metrics() lookup (existing, missing, after overflow)
//   - get_backend_load() delegation to VLLMMetrics::load_score()
//
// What is NOT tested here (requires Seastar reactor):
//   - scrape_vllm_metrics() HTTP socket I/O
//   - run_loop() / scrape_one_backend() coroutine orchestration
//   - Prometheus metrics registration/deregistration lifecycle

#include "vllm_metrics.hpp"
#include "types.hpp"

#include <gtest/gtest.h>

#include <absl/container/flat_hash_map.h>

using namespace ranvier;

// =============================================================================
// Replicated pure logic from HealthService (avoids linking Seastar)
// =============================================================================

// Mirrors HealthService storage with same bounds
static constexpr size_t MAX_TRACKED_BACKENDS = 256;

// Mirrors HealthService adaptive suppression threshold
static constexpr uint32_t SCRAPE_FAILURE_SUPPRESSION_THRESHOLD = 3;

struct TestMetricsStore {
    absl::flat_hash_map<BackendId, VLLMMetrics> backend_vllm_metrics;
    uint64_t overflow_drops = 0;

    // Adaptive scrape suppression state (mirrors HealthService)
    absl::flat_hash_map<BackendId, uint32_t> scrape_failures;
    uint64_t scrapes_suppressed = 0;

    void store(BackendId id, VLLMMetrics metrics) {
        if (backend_vllm_metrics.size() >= MAX_TRACKED_BACKENDS
            && !backend_vllm_metrics.contains(id)) {
            ++overflow_drops;
            return;
        }
        backend_vllm_metrics[id] = std::move(metrics);
    }

    VLLMMetrics get(BackendId id) const {
        auto it = backend_vllm_metrics.find(id);
        if (it != backend_vllm_metrics.end()) {
            return it->second;
        }
        return VLLMMetrics{};
    }

    double get_load(BackendId id) const {
        auto it = backend_vllm_metrics.find(id);
        if (it != backend_vllm_metrics.end()) {
            return it->second.load_score();
        }
        return 0.0;
    }

    // --- Adaptive suppression (mirrors HealthService logic) ---

    bool is_scrape_suppressed(BackendId id) const {
        auto it = scrape_failures.find(id);
        return it != scrape_failures.end()
            && it->second >= SCRAPE_FAILURE_SUPPRESSION_THRESHOLD;
    }

    uint32_t record_scrape_failure(BackendId id) {
        if (scrape_failures.size() >= MAX_TRACKED_BACKENDS
            && !scrape_failures.contains(id)) {
            return 0;
        }
        return ++scrape_failures[id];
    }

    void record_scrape_success(BackendId id) {
        scrape_failures.erase(id);
    }
};

// Helper: create a valid VLLMMetrics with some load
static VLLMMetrics make_metrics(uint32_t running, uint32_t waiting, double cache) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = running;
    m.num_requests_waiting = waiting;
    m.gpu_cache_usage_percent = cache;
    m.scraped_at = std::chrono::steady_clock::now();
    return m;
}

// =============================================================================
// Fixture
// =============================================================================

class HealthServiceStoreTest : public ::testing::Test {
protected:
    TestMetricsStore store;
};

// =============================================================================
// get_vllm_metrics — Lookup Behavior
// =============================================================================

TEST_F(HealthServiceStoreTest, MissingBackendReturnsInvalidMetrics) {
    auto m = store.get(42);
    EXPECT_FALSE(m.valid);
}

TEST_F(HealthServiceStoreTest, StoredMetricsAreRetrievable) {
    auto metrics = make_metrics(5, 3, 0.65);
    store.store(1, metrics);

    auto retrieved = store.get(1);
    EXPECT_TRUE(retrieved.valid);
    EXPECT_EQ(retrieved.num_requests_running, 5u);
    EXPECT_EQ(retrieved.num_requests_waiting, 3u);
    EXPECT_DOUBLE_EQ(retrieved.gpu_cache_usage_percent, 0.65);
}

TEST_F(HealthServiceStoreTest, UpdateExistingBackend) {
    store.store(1, make_metrics(5, 0, 0.3));
    store.store(1, make_metrics(10, 5, 0.8));

    auto m = store.get(1);
    EXPECT_EQ(m.num_requests_running, 10u);
    EXPECT_EQ(m.num_requests_waiting, 5u);
    EXPECT_DOUBLE_EQ(m.gpu_cache_usage_percent, 0.8);
}

TEST_F(HealthServiceStoreTest, MultipleBackendsIndependent) {
    store.store(1, make_metrics(5, 0, 0.3));
    store.store(2, make_metrics(10, 5, 0.8));

    EXPECT_EQ(store.get(1).num_requests_running, 5u);
    EXPECT_EQ(store.get(2).num_requests_running, 10u);
}

// =============================================================================
// get_backend_load — Score Delegation
// =============================================================================

TEST_F(HealthServiceStoreTest, LoadScoreForMissingBackendIsZero) {
    EXPECT_DOUBLE_EQ(store.get_load(999), 0.0);
}

TEST_F(HealthServiceStoreTest, LoadScoreDelegatesToVLLMMetrics) {
    auto metrics = make_metrics(8, 3, 0.65);
    store.store(1, metrics);

    // Should match VLLMMetrics::load_score() exactly
    EXPECT_DOUBLE_EQ(store.get_load(1), metrics.load_score());
}

TEST_F(HealthServiceStoreTest, LoadScoreUpdatesWithNewScrape) {
    store.store(1, make_metrics(1, 0, 0.1));
    double low_score = store.get_load(1);

    store.store(1, make_metrics(10, 50, 0.9));
    double high_score = store.get_load(1);

    EXPECT_LT(low_score, high_score);
}

// =============================================================================
// Bounded Container — Rule #4
// =============================================================================

TEST_F(HealthServiceStoreTest, BoundedAtMaxTrackedBackends) {
    // Fill to capacity
    for (size_t i = 0; i < MAX_TRACKED_BACKENDS; ++i) {
        store.store(static_cast<BackendId>(i), make_metrics(1, 0, 0.1));
    }

    EXPECT_EQ(store.backend_vllm_metrics.size(), MAX_TRACKED_BACKENDS);
    EXPECT_EQ(store.overflow_drops, 0u);

    // Next new backend should be dropped
    store.store(static_cast<BackendId>(MAX_TRACKED_BACKENDS), make_metrics(99, 99, 1.0));
    EXPECT_EQ(store.backend_vllm_metrics.size(), MAX_TRACKED_BACKENDS);
    EXPECT_EQ(store.overflow_drops, 1u);

    // Verify the dropped backend is not present
    EXPECT_FALSE(store.get(static_cast<BackendId>(MAX_TRACKED_BACKENDS)).valid);
}

TEST_F(HealthServiceStoreTest, OverflowCounterAccumulates) {
    // Fill to capacity
    for (size_t i = 0; i < MAX_TRACKED_BACKENDS; ++i) {
        store.store(static_cast<BackendId>(i), make_metrics(1, 0, 0.1));
    }

    // Try adding 10 more new backends
    for (size_t i = 0; i < 10; ++i) {
        store.store(static_cast<BackendId>(MAX_TRACKED_BACKENDS + i),
                    make_metrics(1, 0, 0.1));
    }

    EXPECT_EQ(store.overflow_drops, 10u);
    EXPECT_EQ(store.backend_vllm_metrics.size(), MAX_TRACKED_BACKENDS);
}

TEST_F(HealthServiceStoreTest, ExistingBackendUpdatesWhenFull) {
    // Fill to capacity
    for (size_t i = 0; i < MAX_TRACKED_BACKENDS; ++i) {
        store.store(static_cast<BackendId>(i), make_metrics(1, 0, 0.1));
    }

    // Updating an existing backend should succeed even at capacity
    store.store(0, make_metrics(50, 50, 0.99));

    EXPECT_EQ(store.overflow_drops, 0u);
    EXPECT_EQ(store.get(0).num_requests_running, 50u);
}

TEST_F(HealthServiceStoreTest, OverflowDoesNotCorruptExistingData) {
    // Fill to capacity with known data
    for (size_t i = 0; i < MAX_TRACKED_BACKENDS; ++i) {
        store.store(static_cast<BackendId>(i),
                    make_metrics(static_cast<uint32_t>(i), 0, 0.5));
    }

    // Overflow attempts
    for (size_t i = 0; i < 5; ++i) {
        store.store(static_cast<BackendId>(MAX_TRACKED_BACKENDS + i),
                    make_metrics(999, 999, 1.0));
    }

    // Verify existing data is intact
    for (size_t i = 0; i < MAX_TRACKED_BACKENDS; ++i) {
        auto m = store.get(static_cast<BackendId>(i));
        EXPECT_TRUE(m.valid);
        EXPECT_EQ(m.num_requests_running, static_cast<uint32_t>(i));
    }
}

// =============================================================================
// Adaptive Scrape Suppression
// =============================================================================

TEST_F(HealthServiceStoreTest, NewBackendIsNotSuppressed) {
    EXPECT_FALSE(store.is_scrape_suppressed(42));
}

TEST_F(HealthServiceStoreTest, BelowThresholdNotSuppressed) {
    for (uint32_t i = 0; i < SCRAPE_FAILURE_SUPPRESSION_THRESHOLD - 1; ++i) {
        store.record_scrape_failure(1);
    }
    EXPECT_FALSE(store.is_scrape_suppressed(1));
}

TEST_F(HealthServiceStoreTest, AtThresholdIsSuppressed) {
    for (uint32_t i = 0; i < SCRAPE_FAILURE_SUPPRESSION_THRESHOLD; ++i) {
        store.record_scrape_failure(1);
    }
    EXPECT_TRUE(store.is_scrape_suppressed(1));
}

TEST_F(HealthServiceStoreTest, AboveThresholdStaysSuppressed) {
    for (uint32_t i = 0; i < SCRAPE_FAILURE_SUPPRESSION_THRESHOLD + 5; ++i) {
        store.record_scrape_failure(1);
    }
    EXPECT_TRUE(store.is_scrape_suppressed(1));
}

TEST_F(HealthServiceStoreTest, SuccessResetsSuppressionCounter) {
    // Reach suppression threshold
    for (uint32_t i = 0; i < SCRAPE_FAILURE_SUPPRESSION_THRESHOLD; ++i) {
        store.record_scrape_failure(1);
    }
    EXPECT_TRUE(store.is_scrape_suppressed(1));

    // A single success resets (backend upgraded to vLLM)
    store.record_scrape_success(1);
    EXPECT_FALSE(store.is_scrape_suppressed(1));
}

TEST_F(HealthServiceStoreTest, SuccessOnNeverFailedBackendIsNoOp) {
    store.record_scrape_success(42);
    EXPECT_FALSE(store.is_scrape_suppressed(42));
    EXPECT_TRUE(store.scrape_failures.empty());
}

TEST_F(HealthServiceStoreTest, SuppressionIsPerBackend) {
    // Suppress backend 1
    for (uint32_t i = 0; i < SCRAPE_FAILURE_SUPPRESSION_THRESHOLD; ++i) {
        store.record_scrape_failure(1);
    }
    // Backend 2 has one failure
    store.record_scrape_failure(2);

    EXPECT_TRUE(store.is_scrape_suppressed(1));
    EXPECT_FALSE(store.is_scrape_suppressed(2));
}

TEST_F(HealthServiceStoreTest, FailureCounterReturnValue) {
    EXPECT_EQ(store.record_scrape_failure(1), 1u);
    EXPECT_EQ(store.record_scrape_failure(1), 2u);
    EXPECT_EQ(store.record_scrape_failure(1), 3u);
    EXPECT_EQ(store.record_scrape_failure(1), 4u);
}

TEST_F(HealthServiceStoreTest, SuccessAfterPartialFailureResetsCount) {
    store.record_scrape_failure(1);
    store.record_scrape_failure(1);
    store.record_scrape_success(1);

    // Counter should restart from 1
    EXPECT_EQ(store.record_scrape_failure(1), 1u);
    EXPECT_FALSE(store.is_scrape_suppressed(1));
}

TEST_F(HealthServiceStoreTest, ScrapeFailuresBoundedByMaxTrackedBackends) {
    // Fill failure tracking to capacity
    for (size_t i = 0; i < MAX_TRACKED_BACKENDS; ++i) {
        store.record_scrape_failure(static_cast<BackendId>(i));
    }

    // New backend beyond capacity returns 0 (not tracked)
    EXPECT_EQ(store.record_scrape_failure(static_cast<BackendId>(MAX_TRACKED_BACKENDS)), 0u);
    EXPECT_EQ(store.scrape_failures.size(), MAX_TRACKED_BACKENDS);
}
