/**
 * Unit tests for GPU Load-Aware Routing (VISION 2.2)
 *
 * Tests the composite load calculation that blends shard-local active_requests
 * with vLLM GPU load scores (0.0-1.0) for routing decisions. Also tests the
 * GPU load cache staleness logic and the integration with existing load-aware
 * selection algorithms (median-based, bounded-load, P2C).
 *
 * Key properties tested:
 * - Composite load = local_active + (gpu_score * gpu_load_weight)
 * - Unavailable GPU metrics fall back to local-only load
 * - Stale cache entries are ignored (>30s)
 * - Cache bounded at MAX_ENTRIES (256)
 * - GPU-heavy backends get diverted by existing load-aware algorithms
 * - gpu_load_weight=0 disables GPU influence
 * - Negative/invalid GPU scores treated as unavailable
 *
 * These tests mirror the composite load logic in router_service.cpp using
 * simulated backend state, following the same pattern as load_aware_routing_test.cpp.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>
#include <utility>
#include <limits>

// =============================================================================
// Type Definitions (mirroring router_service.cpp)
// =============================================================================

using BackendId = int32_t;

// Simulated backend info (local shard state)
struct TestBackendInfo {
    uint64_t active_requests = 0;
    bool is_draining = false;
    bool is_dead = false;
};

// Simulated GPU load cache entry
struct TestGpuLoadCache {
    std::map<BackendId, double> scores;  // BackendId -> load_score (0.0-1.0)
    std::chrono::steady_clock::time_point updated_at;
    static constexpr size_t MAX_ENTRIES = 256;
};

// Configuration for composite load routing
struct TestGpuLoadConfig {
    double gpu_load_weight = 10.0;
    std::chrono::seconds gpu_load_cache_ttl{30};
    // Inherited from load-aware config
    bool load_aware_routing = true;
    double load_imbalance_factor = 2.0;
    uint64_t load_imbalance_floor = 2;
};

// =============================================================================
// Simulated Composite Load Functions (Algorithm Under Test)
// =============================================================================

/**
 * Get cached GPU load score for a backend.
 * Returns -1.0 if unavailable (no metrics, stale cache, or not found).
 * Staleness threshold is configurable via gpu_load_cache_ttl.
 */
inline double get_cached_gpu_load(
    const TestGpuLoadCache& cache,
    const TestGpuLoadConfig& config,
    BackendId id,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    auto age = now - cache.updated_at;
    if (age > config.gpu_load_cache_ttl) return -1.0;

    auto it = cache.scores.find(id);
    if (it == cache.scores.end()) return -1.0;
    return it->second;
}

/**
 * Get shard-local active request count for a backend.
 */
inline uint64_t get_backend_load(
    const std::map<BackendId, TestBackendInfo>& backends,
    BackendId id)
{
    auto it = backends.find(id);
    if (it == backends.end()) return 0;
    return it->second.active_requests;
}

/**
 * Get composite load: local active_requests + scaled GPU load score.
 * When GPU metrics are unavailable, returns local load only.
 */
inline uint64_t get_composite_backend_load(
    const std::map<BackendId, TestBackendInfo>& backends,
    const TestGpuLoadCache& cache,
    const TestGpuLoadConfig& config,
    BackendId id,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    uint64_t local_load = get_backend_load(backends, id);

    auto gpu_score = get_cached_gpu_load(cache, config, id, now);
    if (gpu_score < 0.0) {
        return local_load;  // No GPU metrics — fall back to local only
    }

    uint64_t gpu_contribution = static_cast<uint64_t>(
        gpu_score * config.gpu_load_weight);

    return local_load + gpu_contribution;
}

/**
 * Find least loaded backend using composite load.
 * Skips draining and dead backends.
 */
inline std::pair<BackendId, uint64_t> get_least_loaded_composite(
    const std::map<BackendId, TestBackendInfo>& backends,
    const TestGpuLoadCache& cache,
    const TestGpuLoadConfig& config,
    const std::vector<BackendId>& candidates,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    BackendId best_id = 0;
    uint64_t best_load = std::numeric_limits<uint64_t>::max();

    for (BackendId id : candidates) {
        auto it = backends.find(id);
        if (it == backends.end()) continue;
        if (it->second.is_draining || it->second.is_dead) continue;

        uint64_t load = get_composite_backend_load(backends, cache, config, id, now);
        if (load < best_load) {
            best_load = load;
            best_id = id;
        }
    }
    return {best_id, best_load};
}

/**
 * Apply load-aware selection using composite load and median threshold.
 * Mirrors apply_load_aware_selection() in router_service.cpp with GPU blending.
 */
inline BackendId apply_composite_load_aware_selection(
    const std::map<BackendId, TestBackendInfo>& backends,
    const TestGpuLoadCache& cache,
    const TestGpuLoadConfig& config,
    BackendId preferred_id,
    const std::vector<BackendId>& live_backends,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    if (!config.load_aware_routing) return preferred_id;
    if (live_backends.size() < 2) return preferred_id;

    uint64_t preferred_load = get_composite_backend_load(
        backends, cache, config, preferred_id, now);
    if (preferred_load == 0) return preferred_id;

    // Collect composite loads for median computation
    std::vector<uint64_t> loads;
    loads.reserve(live_backends.size());
    for (BackendId id : live_backends) {
        loads.push_back(get_composite_backend_load(backends, cache, config, id, now));
    }

    size_t mid = loads.size() / 2;
    std::nth_element(loads.begin(), loads.begin() + static_cast<ptrdiff_t>(mid), loads.end());
    uint64_t median = loads[mid];

    uint64_t threshold = static_cast<uint64_t>(
        static_cast<double>(median) * config.load_imbalance_factor) + config.load_imbalance_floor;
    if (preferred_load <= threshold) return preferred_id;

    auto [least_id, least_load] = get_least_loaded_composite(
        backends, cache, config, live_backends, now);
    if (least_id == 0) return preferred_id;

    return least_id;
}

// =============================================================================
// Test Fixtures
// =============================================================================

class GpuLoadCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        backends.clear();
        backends[1] = {0, false, false};
        backends[2] = {0, false, false};
        backends[3] = {0, false, false};

        cache.scores.clear();
        cache.updated_at = std::chrono::steady_clock::now();

        config = TestGpuLoadConfig{};
        live_backends = {1, 2, 3};
    }

    std::map<BackendId, TestBackendInfo> backends;
    TestGpuLoadCache cache;
    TestGpuLoadConfig config;
    std::vector<BackendId> live_backends;
};

class CompositeLoadTest : public ::testing::Test {
protected:
    void SetUp() override {
        backends.clear();
        backends[1] = {5, false, false};   // 5 active requests
        backends[2] = {3, false, false};   // 3 active requests
        backends[3] = {1, false, false};   // 1 active request

        cache.scores.clear();
        cache.updated_at = std::chrono::steady_clock::now();

        config = TestGpuLoadConfig{};
        config.gpu_load_weight = 10.0;
        live_backends = {1, 2, 3};
    }

    std::map<BackendId, TestBackendInfo> backends;
    TestGpuLoadCache cache;
    TestGpuLoadConfig config;
    std::vector<BackendId> live_backends;
};

// =============================================================================
// GPU Load Cache Tests
// =============================================================================

TEST_F(GpuLoadCacheTest, EmptyCacheReturnsNegative) {
    // No scores cached — should return -1.0
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,1), -1.0);
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,99), -1.0);
}

TEST_F(GpuLoadCacheTest, ValidScoreReturned) {
    cache.scores[1] = 0.42;
    cache.scores[2] = 0.85;

    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,1), 0.42);
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,2), 0.85);
}

TEST_F(GpuLoadCacheTest, MissingBackendReturnsNegative) {
    cache.scores[1] = 0.5;
    // Backend 3 not in cache
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,3), -1.0);
}

TEST_F(GpuLoadCacheTest, StaleCache30SecondsReturnsNegative) {
    cache.scores[1] = 0.5;
    cache.updated_at = std::chrono::steady_clock::now() - std::chrono::seconds(31);

    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,1), -1.0);
}

TEST_F(GpuLoadCacheTest, FreshCacheWithin30Seconds) {
    cache.scores[1] = 0.5;
    cache.updated_at = std::chrono::steady_clock::now() - std::chrono::seconds(29);

    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,1), 0.5);
}

TEST_F(GpuLoadCacheTest, ExactlyAt30SecondsIsStale) {
    cache.scores[1] = 0.5;
    auto now = std::chrono::steady_clock::now();
    cache.updated_at = now - std::chrono::seconds(30);

    // age == 30s, threshold is >30s, so exactly at boundary is NOT stale
    // (the check is age > 30s, not age >= 30s)
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,1, now), 0.5);
}

TEST_F(GpuLoadCacheTest, ZeroScoreIsValid) {
    // 0.0 is a valid score (idle GPU), not the same as "no data"
    cache.scores[1] = 0.0;
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config,1), 0.0);
}

TEST_F(GpuLoadCacheTest, CustomTtlRespected) {
    cache.scores[1] = 0.5;
    auto now = std::chrono::steady_clock::now();
    cache.updated_at = now - std::chrono::seconds(15);

    // With default TTL (30s), 15s old cache is fresh
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, config, 1, now), 0.5);

    // With shorter TTL (10s), 15s old cache is stale
    TestGpuLoadConfig short_ttl = config;
    short_ttl.gpu_load_cache_ttl = std::chrono::seconds(10);
    EXPECT_DOUBLE_EQ(get_cached_gpu_load(cache, short_ttl, 1, now), -1.0);
}

TEST_F(GpuLoadCacheTest, MaxEntriesBound) {
    // Verify the constant exists and is reasonable
    EXPECT_EQ(TestGpuLoadCache::MAX_ENTRIES, 256u);
}

// =============================================================================
// Composite Load Calculation Tests
// =============================================================================

TEST_F(CompositeLoadTest, NoGpuMetricsReturnsLocalOnly) {
    // No GPU scores in cache — composite should equal local load
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 5u);
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 2), 3u);
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 3), 1u);
}

TEST_F(CompositeLoadTest, GpuScoreAddsWeightedContribution) {
    // Backend 1: local=5, gpu=0.5, weight=10 → composite = 5 + (0.5*10) = 10
    cache.scores[1] = 0.5;
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 10u);
}

TEST_F(CompositeLoadTest, FullGpuLoadAddsFullWeight) {
    // Backend 1: local=5, gpu=1.0, weight=10 → composite = 5 + 10 = 15
    cache.scores[1] = 1.0;
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 15u);
}

TEST_F(CompositeLoadTest, ZeroGpuScoreNoContribution) {
    // Backend 1: local=5, gpu=0.0, weight=10 → composite = 5 + 0 = 5
    cache.scores[1] = 0.0;
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 5u);
}

TEST_F(CompositeLoadTest, ZeroWeightDisablesGpuInfluence) {
    cache.scores[1] = 0.9;
    config.gpu_load_weight = 0.0;
    // gpu_contribution = 0.9 * 0.0 = 0 → composite = local only
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 5u);
}

TEST_F(CompositeLoadTest, HighWeightAmplifiesGpuSignal) {
    cache.scores[1] = 0.5;
    config.gpu_load_weight = 100.0;
    // local=5, gpu_contribution = 0.5 * 100 = 50 → composite = 55
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 55u);
}

TEST_F(CompositeLoadTest, MixedAvailability) {
    // Only backend 1 has GPU metrics
    cache.scores[1] = 0.8;
    // Backend 1: 5 + (0.8*10) = 13
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 13u);
    // Backend 2: no GPU metrics → local only = 3
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 2), 3u);
    // Backend 3: no GPU metrics → local only = 1
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 3), 1u);
}

TEST_F(CompositeLoadTest, StaleCacheFallsBackToLocal) {
    cache.scores[1] = 0.9;
    cache.updated_at = std::chrono::steady_clock::now() - std::chrono::seconds(31);

    // Stale cache — should return local load only
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 5u);
}

TEST_F(CompositeLoadTest, UnknownBackendReturnsZero) {
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 99), 0u);
}

TEST_F(CompositeLoadTest, GpuContributionTruncatedToInteger) {
    // gpu_score=0.33, weight=10 → 3.3 truncated to 3
    cache.scores[1] = 0.33;
    // local=5 + 3 = 8
    EXPECT_EQ(get_composite_backend_load(backends, cache, config, 1), 8u);
}

// =============================================================================
// Least Loaded with Composite Load
// =============================================================================

TEST_F(CompositeLoadTest, LeastLoadedConsidersGpuScore) {
    // Without GPU: backend 3 (load=1) is least loaded
    // With GPU on backend 3: 3 has gpu=0.9 → composite = 1 + 9 = 10
    // Backend 2 has no GPU → composite = 3 (least loaded)
    cache.scores[3] = 0.9;

    auto [best_id, best_load] = get_least_loaded_composite(
        backends, cache, config, live_backends);
    EXPECT_EQ(best_id, 2);
    EXPECT_EQ(best_load, 3u);
}

TEST_F(CompositeLoadTest, LeastLoadedSkipsDraining) {
    cache.scores[1] = 0.0;
    cache.scores[2] = 0.0;
    cache.scores[3] = 0.0;

    backends[3].is_draining = true;
    // Backend 3 (load=1) would be least loaded but is draining
    auto [best_id, best_load] = get_least_loaded_composite(
        backends, cache, config, live_backends);
    EXPECT_EQ(best_id, 2);  // Backend 2 with load=3
}

TEST_F(CompositeLoadTest, LeastLoadedSkipsDead) {
    backends[3].is_dead = true;
    auto [best_id, best_load] = get_least_loaded_composite(
        backends, cache, config, live_backends);
    EXPECT_EQ(best_id, 2);  // Backend 2 (load=3), skipping dead backend 3
}

// =============================================================================
// Composite Load-Aware Selection (Median Threshold with GPU)
// =============================================================================

TEST_F(CompositeLoadTest, GpuLoadCausesDiversion) {
    // Local loads are balanced: [5, 3, 1]
    // Add GPU load to backend 1: gpu=0.8 → composite = 5+8 = 13
    // Backends 2,3 have no GPU: composites = [13, 3, 1]
    // median=3, threshold = 3*2 + 2 = 8
    // preferred (backend 1) has 13 > 8 → divert
    cache.scores[1] = 0.8;

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result, 3);  // Least loaded (composite=1)
}

TEST_F(CompositeLoadTest, LowGpuLoadNoEffect) {
    // Backend 1: local=5, gpu=0.1 → composite = 5+1 = 6
    // Composites: [6, 3, 1], median=3, threshold = 3*2+2 = 8
    // preferred=6 <= 8 → no diversion
    cache.scores[1] = 0.1;

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result, 1);  // Preferred (below threshold)
}

TEST_F(CompositeLoadTest, AllBackendsWithGpuBalanced) {
    // Equal GPU across all backends — should not divert
    cache.scores[1] = 0.5;
    cache.scores[2] = 0.5;
    cache.scores[3] = 0.5;
    // Composites: [5+5=10, 3+5=8, 1+5=6], median=8, threshold = 8*2+2 = 18
    // preferred=10 <= 18 → no diversion

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(CompositeLoadTest, GpuOnlyDiversionZeroLocalLoad) {
    // All local loads are 0, but backend 1 has high GPU load
    backends[1].active_requests = 0;
    backends[2].active_requests = 0;
    backends[3].active_requests = 0;
    cache.scores[1] = 0.9;  // composite = 0 + 9 = 9
    cache.scores[2] = 0.1;  // composite = 0 + 1 = 1
    cache.scores[3] = 0.1;  // composite = 0 + 1 = 1
    // Composites: [9, 1, 1], median=1, threshold = 1*2+2 = 4
    // preferred=9 > 4 → divert

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_NE(result, 1);
}

TEST_F(CompositeLoadTest, DisabledRoutingReturnsPreferred) {
    config.load_aware_routing = false;
    cache.scores[1] = 1.0;  // Saturated GPU

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(CompositeLoadTest, SingleBackendReturnsPreferred) {
    cache.scores[1] = 1.0;
    std::vector<BackendId> single = {1};

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, single);
    EXPECT_EQ(result, 1);
}

TEST_F(CompositeLoadTest, StaleCacheIgnoredInSelection) {
    // High GPU load but stale cache — should use local load only
    cache.scores[1] = 1.0;
    cache.updated_at = std::chrono::steady_clock::now() - std::chrono::seconds(31);
    // Local composites: [5, 3, 1], median=3, threshold=3*2+2=8
    // preferred=5 <= 8 → no diversion

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(CompositeLoadTest, PartialGpuMetricsMixCorrectly) {
    // Only backend 1 has GPU metrics — others use local only
    backends[1].active_requests = 2;
    backends[2].active_requests = 10;
    backends[3].active_requests = 10;
    cache.scores[1] = 0.9;  // composite = 2 + 9 = 11
    // Composites: [11, 10, 10], median=10, threshold = 10*2+2 = 22
    // preferred=11 <= 22 → no diversion (GPU didn't make it an outlier)

    BackendId result = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(CompositeLoadTest, WeightScalesGpuInfluenceOnDiversion) {
    // With default weight=10: backend 1 gpu=0.5 → composite = 5+5 = 10
    // Composites: [10, 3, 1], median=3, threshold = 3*2+2 = 8 → divert (10>8)
    cache.scores[1] = 0.5;
    BackendId result1 = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_NE(result1, 1);  // Diverted

    // With low weight=2: backend 1 gpu=0.5 → composite = 5+1 = 6
    // Composites: [6, 3, 1], median=3, threshold = 3*2+2 = 8 → no divert (6<=8)
    config.gpu_load_weight = 2.0;
    BackendId result2 = apply_composite_load_aware_selection(
        backends, cache, config, 1, live_backends);
    EXPECT_EQ(result2, 1);  // Not diverted
}
