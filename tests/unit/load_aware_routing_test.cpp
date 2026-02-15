/**
 * Unit tests for Load-Aware Routing (Relative Threshold)
 *
 * Tests the load-aware backend selection logic that routes requests to less-loaded
 * backends when the prefix-preferred backend is a genuine outlier compared to its
 * peers. Uses a relative threshold (median * factor + floor) that auto-adapts to
 * any workload, model size, or cluster size.
 *
 * Key properties tested:
 * - Balanced load returns preferred backend (no flapping)
 * - Genuine outlier is diverted to least-loaded alternative
 * - Equal load across backends never triggers diversion
 * - Floor prevents flapping at low/zero load
 * - Single backend always returns preferred
 * - Config disabled always returns preferred
 * - Draining/dead backends are skipped
 * - BackendRequestGuard correctly increments/decrements counters
 *
 * These tests verify the load-aware routing algorithm in isolation, using
 * simulated backend state to avoid Seastar dependencies.
 */

#include <gtest/gtest.h>

#include <algorithm>
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

// Simulated config for testing (relative threshold)
struct TestLoadAwareConfig {
    bool load_aware_routing = true;
    double load_imbalance_factor = 2.0;    // Divert when preferred > factor * median
    uint64_t load_imbalance_floor = 2;     // Additive floor to prevent flapping at low load
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
 * Apply load-aware selection using relative threshold (median-based).
 *
 * Preferred is "overloaded" when its queue depth exceeds:
 *   median_load * factor + floor
 *
 * This auto-adapts to any workload, model size, or cluster size.
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

    // Need at least 2 backends for load comparison
    if (live_backends.size() < 2) {
        return preferred_id;
    }

    // Check preferred backend's load
    uint64_t preferred_load = get_backend_load(backends, preferred_id);

    // Fast path: zero load is never overloaded
    if (preferred_load == 0) {
        return preferred_id;
    }

    // Collect loads from all live backends to compute median
    std::vector<uint64_t> loads;
    loads.reserve(live_backends.size());
    for (BackendId id : live_backends) {
        loads.push_back(get_backend_load(backends, id));
    }

    // Compute median via nth_element (O(n), partial sort)
    size_t mid = loads.size() / 2;
    std::nth_element(loads.begin(), loads.begin() + static_cast<ptrdiff_t>(mid), loads.end());
    uint64_t median = loads[mid];

    // Relative threshold: divert only if preferred is significantly above median
    uint64_t threshold = static_cast<uint64_t>(static_cast<double>(median) * config.load_imbalance_factor)
                         + config.load_imbalance_floor;
    if (preferred_load <= threshold) {
        return preferred_id;
    }

    // Preferred is a genuine outlier — find least-loaded alternative
    auto [least_loaded_id, least_load] = get_least_loaded_backend(backends, live_backends);

    if (least_loaded_id == 0) {
        return preferred_id;
    }

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
        // Default configuration: factor=2.0, floor=2
        config.load_aware_routing = true;
        config.load_imbalance_factor = 2.0;
        config.load_imbalance_floor = 2;

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
// Load-Aware Routing Tests (Relative Threshold)
// =============================================================================

TEST_F(LoadAwareRoutingTest, ZeroLoadReturnsPreferred) {
    // All backends at 0 load — fast path
    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, BalancedLoadReturnsPreferred) {
    // All backends at equal load — median=3, threshold=3*2+2=8
    // preferred_load=3 <= 8, so return preferred
    backends[1].active_requests.store(3, std::memory_order_relaxed);
    backends[2].active_requests.store(3, std::memory_order_relaxed);
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
    EXPECT_EQ(apply_load_aware_selection(backends, config, 2, live_backends), 2);
    EXPECT_EQ(apply_load_aware_selection(backends, config, 3, live_backends), 3);
}

TEST_F(LoadAwareRoutingTest, EqualHighLoadNeverDiverts) {
    // All backends at high equal load — median=10, threshold=10*2+2=22
    // No backend is an outlier — should never divert
    backends[1].active_requests.store(10, std::memory_order_relaxed);
    backends[2].active_requests.store(10, std::memory_order_relaxed);
    backends[3].active_requests.store(10, std::memory_order_relaxed);

    BackendId result1 = apply_load_aware_selection(backends, config, 1, live_backends);
    BackendId result2 = apply_load_aware_selection(backends, config, 1, live_backends);

    EXPECT_EQ(result1, 1);
    EXPECT_EQ(result2, 1);  // Deterministic
}

TEST_F(LoadAwareRoutingTest, MildImbalanceBelowThreshold) {
    // Loads: [6, 5, 5] — median=5, threshold=5*2+2=12
    // preferred_load=6 <= 12, so return preferred
    backends[1].active_requests.store(6, std::memory_order_relaxed);
    backends[2].active_requests.store(5, std::memory_order_relaxed);
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, GenuineOutlierDiverts) {
    // Loads: [20, 2, 5] — median=5, threshold=5*2+2=12
    // preferred_load=20 > 12, divert to least loaded
    backends[1].active_requests.store(20, std::memory_order_relaxed);
    backends[2].active_requests.store(2, std::memory_order_relaxed);
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 2);  // Least loaded
}

TEST_F(LoadAwareRoutingTest, ExactlyAtThresholdReturnsPreferred) {
    // Loads: [12, 5, 5] — median=5, threshold=5*2+2=12
    // preferred_load=12 <= 12, return preferred
    backends[1].active_requests.store(12, std::memory_order_relaxed);
    backends[2].active_requests.store(5, std::memory_order_relaxed);
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, JustAboveThresholdDiverts) {
    // Loads: [13, 5, 5] — median=5, threshold=5*2+2=12
    // preferred_load=13 > 12, divert
    backends[1].active_requests.store(13, std::memory_order_relaxed);
    backends[2].active_requests.store(5, std::memory_order_relaxed);
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_NE(result, 1);
}

TEST_F(LoadAwareRoutingTest, FloorPreventsFlappingAtLowLoad) {
    // Loads: [1, 0, 0] — median=0, threshold=0*2+2=2
    // preferred_load=1 <= 2, return preferred (floor prevents diversion)
    backends[1].active_requests.store(1, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[3].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, FloorExceededAtLowMedian) {
    // Loads: [5, 0, 0] — median=0, threshold=0*2+2=2
    // preferred_load=5 > 2, divert
    backends[1].active_requests.store(5, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[3].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_NE(result, 1);
}

TEST_F(LoadAwareRoutingTest, DisabledAlwaysReturnsPreferred) {
    config.load_aware_routing = false;

    backends[1].active_requests.store(100, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, SingleBackendReturnsPreferred) {
    // Only one backend — can't compare
    backends[1].active_requests.store(100, std::memory_order_relaxed);

    std::vector<BackendId> single = {1};
    BackendId result = apply_load_aware_selection(backends, config, 1, single);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, EmptyLiveBackendsReturnsPreferred) {
    backends[1].active_requests.store(10, std::memory_order_relaxed);

    std::vector<BackendId> empty_backends;
    BackendId result = apply_load_aware_selection(backends, config, 1, empty_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, SkipsDrainingBackends) {
    // Loads: [20, 0(draining), 5] — median=5 (from loads [20, 0, 5])
    // threshold=5*2+2=12, preferred_load=20 > 12
    // But backend 2 is draining, so least-loaded viable is backend 3
    backends[1].active_requests.store(20, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[2].is_draining = true;
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 3);
}

TEST_F(LoadAwareRoutingTest, SkipsDeadBackends) {
    // Backend 2 is dead, should be skipped by get_least_loaded_backend
    backends[1].active_requests.store(20, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[2].is_dead = true;
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 3);
}

TEST_F(LoadAwareRoutingTest, AllAlternativesDrainingReturnsPreferred) {
    // Backend 1 is outlier, but all alternatives are draining
    backends[1].active_requests.store(20, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[2].is_draining = true;
    backends[3].active_requests.store(0, std::memory_order_relaxed);
    backends[3].is_draining = true;

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, PreferredNotInLiveBackends) {
    // Preferred backend not in live list (edge case)
    // preferred_load = get_backend_load(1) = 20
    // live_backends = {2}, loads = [0], median = 0, threshold = 0*2+2 = 2
    // 20 > 2, so divert to backend 2
    backends[1].active_requests.store(20, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);

    std::vector<BackendId> only_backend2 = {2, 3};
    BackendId result = apply_load_aware_selection(backends, config, 1, only_backend2);
    EXPECT_NE(result, 1);
}

TEST_F(LoadAwareRoutingTest, HighFactorReducesSensitivity) {
    config.load_imbalance_factor = 10.0;

    // Loads: [20, 5, 5] — median=5, threshold=5*10+2=52
    // preferred_load=20 <= 52, no diversion
    backends[1].active_requests.store(20, std::memory_order_relaxed);
    backends[2].active_requests.store(5, std::memory_order_relaxed);
    backends[3].active_requests.store(5, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);
}

TEST_F(LoadAwareRoutingTest, ZeroFloorMakesFactorDominant) {
    config.load_imbalance_floor = 0;

    // Loads: [1, 0, 0] — median=0, threshold=0*2+0=0
    // preferred_load=1 > 0, divert (no floor protection)
    backends[1].active_requests.store(1, std::memory_order_relaxed);
    backends[2].active_requests.store(0, std::memory_order_relaxed);
    backends[3].active_requests.store(0, std::memory_order_relaxed);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_NE(result, 1);
}

// =============================================================================
// Threshold Auto-Scaling Tests
// =============================================================================
// These verify the key property: threshold scales with workload automatically

TEST_F(LoadAwareRoutingTest, ThresholdScalesWithLoad_Low) {
    // Low load: 10 users / 8 backends ≈ 1.25 req/GPU
    // Loads: [2, 1, 1] — median=1, threshold=1*2+2=4
    // preferred_load=2 <= 4, no diversion
    backends[1].active_requests.store(2, std::memory_order_relaxed);
    backends[2].active_requests.store(1, std::memory_order_relaxed);
    backends[3].active_requests.store(1, std::memory_order_relaxed);

    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
}

TEST_F(LoadAwareRoutingTest, ThresholdScalesWithLoad_Medium) {
    // Medium load: 20 users / 8 backends ≈ 2.5 req/GPU
    // Loads: [4, 2, 3] — median=3, threshold=3*2+2=8
    // preferred_load=4 <= 8, no diversion (this was the flapping zone before!)
    backends[1].active_requests.store(4, std::memory_order_relaxed);
    backends[2].active_requests.store(2, std::memory_order_relaxed);
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
}

TEST_F(LoadAwareRoutingTest, ThresholdScalesWithLoad_High) {
    // High load: 30 users / 8 backends ≈ 3.75 req/GPU
    // Loads: [5, 3, 4] — median=4, threshold=4*2+2=10
    // preferred_load=5 <= 10, no diversion (balanced)
    backends[1].active_requests.store(5, std::memory_order_relaxed);
    backends[2].active_requests.store(3, std::memory_order_relaxed);
    backends[3].active_requests.store(4, std::memory_order_relaxed);

    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
}

TEST_F(LoadAwareRoutingTest, ThresholdScalesWithLoad_GenuineHotspot) {
    // High load but with genuine hot spot
    // Loads: [15, 3, 4] — median=4, threshold=4*2+2=10
    // preferred_load=15 > 10, divert
    backends[1].active_requests.store(15, std::memory_order_relaxed);
    backends[2].active_requests.store(3, std::memory_order_relaxed);
    backends[3].active_requests.store(4, std::memory_order_relaxed);

    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 2);
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
        config.load_imbalance_factor = 2.0;
        config.load_imbalance_floor = 2;

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

    // Add requests one by one until backend 1 is a genuine outlier
    // With 0 load on others: median=0, threshold=0*2+2=2
    // Need preferred_load > 2 to trigger
    for (int i = 0; i < 5; ++i) {
        guards.emplace_back(backends, 1);
    }
    // Backend 1 now has 5 active requests, others have 0
    // median=0, threshold=0*2+2=2, 5 > 2 → divert

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

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

    EXPECT_EQ(backends[1].active_requests.load(), 0u);

    BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);
    EXPECT_EQ(result, 1);  // Preferred backend returned (zero load → fast path)
}

TEST_F(LoadAwareIntegrationTest, BalancedLoadNoFallback) {
    // All backends have equal load — never diverts regardless of load level
    backends[1].active_requests.store(3, std::memory_order_relaxed);
    backends[2].active_requests.store(3, std::memory_order_relaxed);
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    // median=3, threshold=3*2+2=8, preferred_load=3 <= 8
    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
    EXPECT_EQ(apply_load_aware_selection(backends, config, 2, live_backends), 2);
    EXPECT_EQ(apply_load_aware_selection(backends, config, 3, live_backends), 3);
}

TEST_F(LoadAwareIntegrationTest, BalancedHighLoadNoFallback) {
    // Even at very high equal load, no diversion
    backends[1].active_requests.store(50, std::memory_order_relaxed);
    backends[2].active_requests.store(50, std::memory_order_relaxed);
    backends[3].active_requests.store(50, std::memory_order_relaxed);

    // median=50, threshold=50*2+2=102, preferred_load=50 <= 102
    EXPECT_EQ(apply_load_aware_selection(backends, config, 1, live_backends), 1);
}

TEST_F(LoadAwareIntegrationTest, GradualLoadImbalance) {
    // Test that diversion triggers smoothly as imbalance grows
    // Others at 3, median=3, threshold=3*2+2=8
    backends[2].active_requests.store(3, std::memory_order_relaxed);
    backends[3].active_requests.store(3, std::memory_order_relaxed);

    for (uint64_t load = 0; load <= 12; ++load) {
        backends[1].active_requests.store(load, std::memory_order_relaxed);

        BackendId result = apply_load_aware_selection(backends, config, 1, live_backends);

        // Median depends on all 3 loads. With loads [load, 3, 3]:
        // For load <= 3: median=3, threshold=3*2+2=8, load <= 8 → preferred
        // For load=4..8: median=3, threshold=8, load <= 8 → preferred
        // For load=9..12: median=3, threshold=8, load > 8 → divert
        // (Note: when load >= 3, sorted=[3, 3, load], median=3)
        if (load <= 8) {
            EXPECT_EQ(result, 1) << "Load " << load << " should return preferred";
        } else {
            EXPECT_NE(result, 1) << "Load " << load << " should divert";
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
