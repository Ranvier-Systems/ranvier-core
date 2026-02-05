/**
 * Unit tests for Load-Aware Routing
 *
 * Tests the load-aware backend selection logic that routes requests to less-loaded
 * backends when the prefix-preferred backend is overloaded. This prevents hot spots
 * and reduces tail latency under heavy load.
 *
 * Key properties tested:
 * - Load below threshold returns preferred backend
 * - Load above threshold with no alternative returns preferred
 * - Load above threshold with available alternative returns least-loaded
 * - Load difference below diff_threshold returns preferred (marginal improvement)
 * - Multiple backends at same load returns deterministic selection
 * - Config disabled always returns preferred
 * - BackendRequestGuard correctly increments/decrements counters
 *
 * These tests verify the load-aware routing algorithm in isolation, using
 * simulated backend state to avoid Seastar dependencies.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <vector>
#include <utility>
#include <limits>

// =============================================================================
// Type Definitions (mirroring router_service.cpp)
// =============================================================================

using BackendId = int32_t;

// Simulated backend info for testing
struct TestBackendInfo {
    std::atomic<uint64_t> active_requests{0};
    bool is_draining = false;
    bool is_dead = false;

    TestBackendInfo() = default;
    TestBackendInfo(uint64_t load, bool draining = false, bool dead = false)
        : active_requests(load), is_draining(draining), is_dead(dead) {}

    // Copy constructor: atomics aren't copyable, so load the value explicitly
    TestBackendInfo(const TestBackendInfo& other)
        : active_requests(other.active_requests.load(std::memory_order_relaxed))
        , is_draining(other.is_draining)
        , is_dead(other.is_dead) {}

    // Move constructor: atomics aren't movable, so load the value explicitly
    TestBackendInfo(TestBackendInfo&& other) noexcept
        : active_requests(other.active_requests.load(std::memory_order_relaxed))
        , is_draining(other.is_draining)
        , is_dead(other.is_dead) {}

    // Copy assignment
    TestBackendInfo& operator=(const TestBackendInfo& other) {
        if (this != &other) {
            active_requests.store(other.active_requests.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
            is_draining = other.is_draining;
            is_dead = other.is_dead;
        }
        return *this;
    }

    // Move assignment
    TestBackendInfo& operator=(TestBackendInfo&& other) noexcept {
        if (this != &other) {
            active_requests.store(other.active_requests.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
            is_draining = other.is_draining;
            is_dead = other.is_dead;
        }
        return *this;
    }
};

// Simulated config for testing
struct TestLoadAwareConfig {
    bool load_aware_routing = true;
    uint64_t queue_depth_threshold = 4;
    uint64_t queue_diff_threshold = 2;
};

// =============================================================================
// Simulated Load-Aware Routing Functions (Algorithm Under Test)
// =============================================================================
// These mirror the logic in router_service.cpp but operate on test data structures

/**
 * Get current in-flight request count for a backend.
 * Returns 0 if backend not found.
 */
inline uint64_t get_backend_load(
    const std::map<BackendId, TestBackendInfo>& backends,
    BackendId id)
{
    auto it = backends.find(id);
    if (it == backends.end()) {
        return 0;
    }
    return it->second.active_requests.load(std::memory_order_relaxed);
}

/**
 * Find the least loaded backend from a list of candidates.
 * Skips draining and dead backends.
 * Returns pair of (backend_id, load). Returns (0, UINT64_MAX) if no candidates found.
 */
inline std::pair<BackendId, uint64_t> get_least_loaded_backend(
    const std::map<BackendId, TestBackendInfo>& backends,
    const std::vector<BackendId>& candidates)
{
    if (candidates.empty()) {
        return {0, std::numeric_limits<uint64_t>::max()};
    }

    BackendId best_id = 0;
    uint64_t best_load = std::numeric_limits<uint64_t>::max();

    for (BackendId id : candidates) {
        auto it = backends.find(id);
        if (it == backends.end()) {
            continue;  // Skip unknown backends
        }

        // Skip draining or dead backends
        if (it->second.is_draining || it->second.is_dead) {
            continue;
        }

        uint64_t load = it->second.active_requests.load(std::memory_order_relaxed);
        if (load < best_load) {
            best_load = load;
            best_id = id;
            // Early exit: 0 is the minimum possible load
            if (load == 0) {
                break;
            }
        }
    }

    return {best_id, best_load};
}

/**
 * Apply load-aware selection to choose between preferred backend and alternatives.
 * This is the core algorithm being tested.
 *
 * Returns the backend to use: either preferred_id or a less-loaded alternative.
 */
inline BackendId apply_load_aware_selection(
    const std::map<BackendId, TestBackendInfo>& backends,
    const TestLoadAwareConfig& config,
    BackendId preferred_id,
    const std::vector<BackendId>& live_backends)
{
    // Fast path: check if load-aware routing is enabled
    if (!config.load_aware_routing) {
        return preferred_id;
    }

    // Check preferred backend's load
    uint64_t preferred_load = get_backend_load(backends, preferred_id);
    if (preferred_load <= config.queue_depth_threshold) {
        return preferred_id;  // Not overloaded - fast path
    }

    // Preferred backend overloaded - find alternative
    auto [least_loaded_id, least_load] = get_least_loaded_backend(backends, live_backends);

    // Validate we found a viable alternative with significant load difference
    if (least_loaded_id == 0 || preferred_load - least_load <= config.queue_diff_threshold) {
        return preferred_id;
    }

    // Significant difference - divert to less-loaded backend
    return least_loaded_id;
}

// =============================================================================
// BackendRequestGuard Simulation
// =============================================================================

/**
 * Simulated RAII guard for tracking in-flight requests.
 * Mirrors BackendRequestGuard in router_service.cpp.
 */
class TestBackendRequestGuard {
public:
    TestBackendRequestGuard(std::map<BackendId, TestBackendInfo>& backends, BackendId id)
        : _backends(&backends), _backend_id(id), _active(false)
    {
        auto it = _backends->find(id);
        if (it == _backends->end()) {
            return;  // Backend not found
        }
        it->second.active_requests.fetch_add(1, std::memory_order_relaxed);
        _active = true;
    }

    ~TestBackendRequestGuard() {
        if (!_active || !_backends) {
            return;
        }
        auto it = _backends->find(_backend_id);
        if (it == _backends->end()) {
            return;
        }
        // Underflow guard
        uint64_t current = it->second.active_requests.load(std::memory_order_relaxed);
        if (current > 0) {
            it->second.active_requests.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // Move constructor
    TestBackendRequestGuard(TestBackendRequestGuard&& other) noexcept
        : _backends(other._backends)
        , _backend_id(other._backend_id)
        , _active(other._active)
    {
        other._active = false;
    }

    // Move assignment
    TestBackendRequestGuard& operator=(TestBackendRequestGuard&& other) noexcept {
        if (this != &other) {
            // Decrement current count if active
            if (_active && _backends) {
                auto it = _backends->find(_backend_id);
                if (it != _backends->end()) {
                    uint64_t current = it->second.active_requests.load(std::memory_order_relaxed);
                    if (current > 0) {
                        it->second.active_requests.fetch_sub(1, std::memory_order_relaxed);
                    }
                }
            }
            _backends = other._backends;
            _backend_id = other._backend_id;
            _active = other._active;
            other._active = false;
        }
        return *this;
    }

    // Non-copyable
    TestBackendRequestGuard(const TestBackendRequestGuard&) = delete;
    TestBackendRequestGuard& operator=(const TestBackendRequestGuard&) = delete;

    BackendId backend_id() const { return _backend_id; }
    bool is_active() const { return _active; }

private:
    std::map<BackendId, TestBackendInfo>* _backends;
    BackendId _backend_id{0};
    bool _active{false};
};

// =============================================================================
// Test Fixtures
// =============================================================================

class LoadAwareRoutingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Default configuration
        config.load_aware_routing = true;
        config.queue_depth_threshold = 4;
        config.queue_diff_threshold = 2;

        // Set up default backends
        backends.clear();
        backends[1] = TestBackendInfo(0);  // Backend 1 with 0 load
        backends[2] = TestBackendInfo(0);  // Backend 2 with 0 load
        backends[3] = TestBackendInfo(0);  // Backend 3 with 0 load

        live_backends = {1, 2, 3};
    }

    TestLoadAwareConfig config;
    std::map<BackendId, TestBackendInfo> backends;
    std::vector<BackendId> live_backends;
};

class BackendRequestGuardTest : public ::testing::Test {
protected:
    void SetUp() override {
        backends.clear();
        backends[1] = TestBackendInfo(0);
        backends[2] = TestBackendInfo(0);
    }

    std::map<BackendId, TestBackendInfo> backends;
};

// =============================================================================
// Load-Aware Routing Tests
// =============================================================================

TEST_F(LoadAwareRoutingTest, BelowThresholdReturnsPreferred) {
    // Backend 1 has load below threshold (4)
    backends[1].active_requests.store(3, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should return preferred backend even though backend 2 has lower load
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, AboveThresholdNoAlternativeReturnsPreferred) {
    // All backends are heavily loaded
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(9, std::memory_order_relaxed);
    backends[3].active_requests.store(9, std::memory_order_relaxed);

    // Load difference is only 1, which is <= diff_threshold (2)
    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, AboveThresholdReturnsLeastLoaded) {
    // Backend 1 is heavily overloaded, backend 2 has much lower load
    backends[1].active_requests.store(10, std::memory_order_relaxed);  // Preferred
    backends[2].active_requests.store(2, std::memory_order_relaxed);   // Much lower
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should route to backend 2 (load difference 10-2=8 > diff_threshold 2)
    EXPECT_EQ(result, 2);
}

TEST_F(LoadAwareRoutingTest, MarginalDifferenceReturnsPreferred) {
    // Backend 1 is above threshold, but difference to best is <= diff_threshold
    backends[1].active_requests.store(6, std::memory_order_relaxed);  // Preferred
    backends[2].active_requests.store(5, std::memory_order_relaxed);  // Difference = 1
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    // Difference of 1 is <= diff_threshold (2), so cache miss not justified
    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, ExactlyAtThresholdReturnsPreferred) {
    // Backend 1 load equals threshold exactly
    backends[1].active_requests.store(4, std::memory_order_relaxed);  // == threshold
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should return preferred (load <= threshold)
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, JustAboveThresholdWithGoodAlternative) {
    // Backend 1 just above threshold (5 > 4), with a good alternative
    backends[1].active_requests.store(5, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);  // Difference = 5 > 2

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should route to backend 2
    EXPECT_EQ(result, 2);
}

TEST_F(LoadAwareRoutingTest, EqualLoadDeterministicSelection) {
    // All backends have equal load above threshold
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(10, std::memory_order_relaxed);
    backends[3].active_requests.store(10, std::memory_order_relaxed);

    BackendId result1 = apply_load_aware_selection(backends, config, 1, live_backends);
    BackendId result2 = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should be deterministic (same result each time)
    EXPECT_EQ(result1, result2);
    // Should return preferred since no better alternative
    EXPECT_EQ(result1, 1);
}

TEST_F(LoadAwareRoutingTest, DisabledAlwaysReturnsPreferred) {
    config.load_aware_routing = false;

    // Even with very high load difference
    backends[1].active_requests.store(100, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should always return preferred when disabled
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, SkipsDrainingBackends) {
    // Backend 1 is overloaded, backend 2 is draining (should be skipped)
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[2].is_draining = true;
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should skip backend 2 (draining) and use backend 3 if difference is sufficient
    // Difference 10-5=5 > 2, so should route to backend 3
    EXPECT_EQ(result, 3);
}

TEST_F(LoadAwareRoutingTest, SkipsDeadBackends) {
    // Backend 1 is overloaded, backend 2 is dead (should be skipped)
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[2].is_dead = true;
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should skip backend 2 (dead) and use backend 3
    // Difference 10-3=7 > 2, so should route to backend 3
    EXPECT_EQ(result, 3);
}

TEST_F(LoadAwareRoutingTest, AllAlternativesDrainingReturnsPreferred) {
    // Backend 1 is overloaded, all alternatives are draining
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[2].is_draining = true;
    backends[3].active_requests.store(0, std::memory_order_relaxed);
    backends[3].is_draining = true;

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // No viable alternatives, should return preferred
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, EmptyLiveBackendsReturnsPreferred) {
    backends[1].active_requests.store(10, std::memory_order_relaxed);

    std::vector<BackendId> empty_backends;
    BackendId result = apply_load_aware_selection(backends, config, 1, empty_backends);

    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, PreferredNotInLiveBackends) {
    // Preferred backend not in live list (edge case)
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    std::vector<BackendId> only_backend2 = {2};
    // Backend 1 is preferred but not in live list
    // get_backend_load will return 10 for backend 1
    // Least loaded from {2} is backend 2 with load 0
    // Difference 10-0=10 > 2, so should return backend 2
    BackendId result = apply_load_aware_selection(backends, config, 1, only_backend2);

    EXPECT_EQ(result, 2);
}

TEST_F(LoadAwareRoutingTest, ZeroThresholdAlwaysConsidersAlternatives) {
    config.queue_depth_threshold = 0;  // Any load triggers consideration

    backends[1].active_requests.store(1, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    // Load 1 > threshold 0, difference 1-0=1 <= diff_threshold 2
    // So should still return preferred (marginal difference)
    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);

    // With larger difference
    backends[1].active_requests.store(5, std::memory_order_relaxed);
    // Difference 5-0=5 > 2, should route to backend 2
    result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 2);
}

TEST_F(LoadAwareRoutingTest, HighDiffThresholdReducesFallbacks) {
    config.queue_diff_threshold = 100;  // Very high threshold

    backends[1].active_requests.store(50, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    // Difference 50-0=50 <= diff_threshold 100, no fallback
    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    EXPECT_EQ(result, 1);
}

// =============================================================================
// BackendRequestGuard Tests
// =============================================================================

TEST_F(BackendRequestGuardTest, IncrementsOnConstruction) {
    EXPECT_EQ(backends[1].active_requests.load(), 0u);

    {
        TestBackendRequestGuard guard(backends, 1);
        EXPECT_EQ(backends[1].active_requests.load(), 1u);
        EXPECT_TRUE(guard.is_active());
        EXPECT_EQ(guard.backend_id(), 1);
    }
}

TEST_F(BackendRequestGuardTest, DecrementsOnDestruction) {
    {
        TestBackendRequestGuard guard(backends, 1);
        EXPECT_EQ(backends[1].active_requests.load(), 1u);
    }
    // Guard destroyed
    EXPECT_EQ(backends[1].active_requests.load(), 0u);
}

TEST_F(BackendRequestGuardTest, MoveTransfersOwnership) {
    TestBackendRequestGuard guard1(backends, 1);
    EXPECT_EQ(backends[1].active_requests.load(), 1u);

    TestBackendRequestGuard guard2(std::move(guard1));
    EXPECT_EQ(backends[1].active_requests.load(), 1u);  // Still 1, not 2
    EXPECT_FALSE(guard1.is_active());  // NOLINT: testing moved-from state
    EXPECT_TRUE(guard2.is_active());
}

TEST_F(BackendRequestGuardTest, MoveAssignmentTransfersOwnership) {
    TestBackendRequestGuard guard1(backends, 1);
    TestBackendRequestGuard guard2(backends, 2);
    EXPECT_EQ(backends[1].active_requests.load(), 1u);
    EXPECT_EQ(backends[2].active_requests.load(), 1u);

    guard2 = std::move(guard1);
    // guard2's old backend (2) should be decremented
    EXPECT_EQ(backends[2].active_requests.load(), 0u);
    // Backend 1 should still have 1 (transferred ownership)
    EXPECT_EQ(backends[1].active_requests.load(), 1u);
    EXPECT_FALSE(guard1.is_active());  // NOLINT
    EXPECT_TRUE(guard2.is_active());
}

TEST_F(BackendRequestGuardTest, MultipleGuardsSameBackend) {
    {
        TestBackendRequestGuard guard1(backends, 1);
        EXPECT_EQ(backends[1].active_requests.load(), 1u);

        {
            TestBackendRequestGuard guard2(backends, 1);
            EXPECT_EQ(backends[1].active_requests.load(), 2u);

            TestBackendRequestGuard guard3(backends, 1);
            EXPECT_EQ(backends[1].active_requests.load(), 3u);
        }
        // guard2 and guard3 destroyed
        EXPECT_EQ(backends[1].active_requests.load(), 1u);
    }
    // guard1 destroyed
    EXPECT_EQ(backends[1].active_requests.load(), 0u);
}

TEST_F(BackendRequestGuardTest, InvalidBackendIdCreatesInactiveGuard) {
    TestBackendRequestGuard guard(backends, 999);  // Non-existent backend
    EXPECT_FALSE(guard.is_active());
    EXPECT_EQ(guard.backend_id(), 999);
}

TEST_F(BackendRequestGuardTest, UnderflowProtection) {
    // Manually set load to 0 to simulate edge case
    backends[1].active_requests.store(0, std::memory_order_relaxed);

    // Create guard that thinks it incremented (simulating backend removal/re-add)
    {
        TestBackendRequestGuard guard(backends, 1);
        EXPECT_EQ(backends[1].active_requests.load(), 1u);

        // Manually set to 0 to simulate the edge case
        backends[1].active_requests.store(0, std::memory_order_relaxed);
    }
    // Guard's destructor should not underflow
    EXPECT_EQ(backends[1].active_requests.load(), 0u);  // Should stay 0, not wrap
}

// =============================================================================
// get_least_loaded_backend Tests
// =============================================================================

class GetLeastLoadedTest : public ::testing::Test {
protected:
    std::map<BackendId, TestBackendInfo> backends;
};

TEST_F(GetLeastLoadedTest, ReturnsLowestLoad) {
    backends[1] = TestBackendInfo(10);
    backends[2] = TestBackendInfo(5);
    backends[3] = TestBackendInfo(8);

    auto [id, load] = get_least_loaded_backend(backends, {1, 2, 3});

    EXPECT_EQ(id, 2);
    EXPECT_EQ(load, 5u);
}

TEST_F(GetLeastLoadedTest, EarlyExitOnZeroLoad) {
    backends[1] = TestBackendInfo(10);
    backends[2] = TestBackendInfo(0);
    backends[3] = TestBackendInfo(5);

    auto [id, load] = get_least_loaded_backend(backends, {1, 2, 3});

    EXPECT_EQ(id, 2);
    EXPECT_EQ(load, 0u);
}

TEST_F(GetLeastLoadedTest, SkipsDrainingBackends) {
    backends[1] = TestBackendInfo(10);
    backends[2] = TestBackendInfo(0, true);  // Draining
    backends[3] = TestBackendInfo(5);

    auto [id, load] = get_least_loaded_backend(backends, {1, 2, 3});

    EXPECT_EQ(id, 3);  // Skip backend 2
    EXPECT_EQ(load, 5u);
}

TEST_F(GetLeastLoadedTest, SkipsDeadBackends) {
    backends[1] = TestBackendInfo(10);
    backends[2] = TestBackendInfo(0, false, true);  // Dead
    backends[3] = TestBackendInfo(5);

    auto [id, load] = get_least_loaded_backend(backends, {1, 2, 3});

    EXPECT_EQ(id, 3);
    EXPECT_EQ(load, 5u);
}

TEST_F(GetLeastLoadedTest, EmptyCandidatesReturnsZero) {
    backends[1] = TestBackendInfo(0);

    auto [id, load] = get_least_loaded_backend(backends, {});

    EXPECT_EQ(id, 0);
    EXPECT_EQ(load, std::numeric_limits<uint64_t>::max());
}

TEST_F(GetLeastLoadedTest, UnknownBackendsSkipped) {
    backends[1] = TestBackendInfo(10);

    auto [id, load] = get_least_loaded_backend(backends, {1, 99, 100});

    EXPECT_EQ(id, 1);
    EXPECT_EQ(load, 10u);
}

TEST_F(GetLeastLoadedTest, AllDrainingReturnsZero) {
    backends[1] = TestBackendInfo(0, true);
    backends[2] = TestBackendInfo(0, true);

    auto [id, load] = get_least_loaded_backend(backends, {1, 2});

    EXPECT_EQ(id, 0);
    EXPECT_EQ(load, std::numeric_limits<uint64_t>::max());
}

// =============================================================================
// Type Traits Tests
// =============================================================================

TEST(LoadAwareTypeTraits, BackendRequestGuardIsNotCopyable) {
    EXPECT_FALSE(std::is_copy_constructible_v<TestBackendRequestGuard>);
    EXPECT_FALSE(std::is_copy_assignable_v<TestBackendRequestGuard>);
}

TEST(LoadAwareTypeTraits, BackendRequestGuardIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<TestBackendRequestGuard>);
    EXPECT_TRUE(std::is_move_assignable_v<TestBackendRequestGuard>);
}

// =============================================================================
// Integration-Style Tests (Algorithm Correctness)
// =============================================================================

class LoadAwareIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.load_aware_routing = true;
        config.queue_depth_threshold = 4;
        config.queue_diff_threshold = 2;

        backends.clear();
        backends[1] = TestBackendInfo(0);
        backends[2] = TestBackendInfo(0);
        backends[3] = TestBackendInfo(0);

        live_backends = {1, 2, 3};
    }

    TestLoadAwareConfig config;
    std::map<BackendId, TestBackendInfo> backends;
    std::vector<BackendId> live_backends;
};

TEST_F(LoadAwareIntegrationTest, SimulatedLoadBuildupCausesFallback) {
    // Simulate requests accumulating on backend 1
    std::vector<TestBackendRequestGuard> guards;

    // Add requests one by one until we hit threshold
    for (int i = 0; i < 5; ++i) {
        guards.emplace_back(backends, 1);
    }
    // Backend 1 now has 5 active requests (above threshold 4)

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

    // Should fall back to a less-loaded backend (2 or 3 with 0 load)
    // Difference 5-0=5 > diff_threshold 2
    EXPECT_NE(result, 1);
    EXPECT_TRUE(result == 2 || result == 3);
}

TEST_F(LoadAwareIntegrationTest, LoadReleaseTriggersPreferedReturn) {
    // Build up load on backend 1
    {
        std::vector<TestBackendRequestGuard> guards;
        for (int i = 0; i < 5; ++i) {
            guards.emplace_back(backends, 1);
        }
        // During this scope, backend 1 is overloaded
        BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
        EXPECT_NE(result, 1);  // Fallback expected
    }
    // Guards destroyed, load released

    // Now backend 1 should have 0 load
    EXPECT_EQ(backends[1].active_requests.load(), 0u);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);  // Preferred backend returned
}

TEST_F(LoadAwareIntegrationTest, BalancedLoadNoFallback) {
    // All backends have equal load below threshold
    backends[1].active_requests.store(3, std::memory_order_relaxed);
    backends[2].active_requests.store(3, std::memory_order_relaxed);
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    // Each backend as preferred should return itself
    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
    EXPECT_EQ(apply_load_aware_selection(backends, config, 2, live_backends), 2);
    EXPECT_EQ(apply_load_aware_selection(backends, config, 3, live_backends), 3);
}

TEST_F(LoadAwareIntegrationTest, GradualLoadImbalance) {
    // Test gradual load imbalance detection
    for (uint64_t load = 0; load <= 10; ++load) {
        backends[1].active_requests.store(load, std::memory_order_relaxed);
        backends[2].active_requests.store(0, std::memory_order_relaxed);

        BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

        if (load <= config.queue_depth_threshold) {
            // Below threshold, always return preferred
            EXPECT_EQ(result, 1) << "Load " << load << " should return preferred";
        } else if (load - 0 <= config.queue_diff_threshold) {
            // Above threshold but difference not significant
            EXPECT_EQ(result, 1) << "Load " << load << " has marginal difference";
        } else {
            // Above threshold with significant difference
            EXPECT_EQ(result, 2) << "Load " << load << " should fallback to backend 2";
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
