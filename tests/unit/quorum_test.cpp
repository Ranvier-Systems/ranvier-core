// Ranvier Core - Quorum / Split-Brain Detection Unit Tests
//
// Tests for quorum calculation and state transitions.
// Includes deterministic timing tests using TestClock for peer liveness
// window expiration verification.
// These tests don't require Seastar runtime.

#include "test_clock.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

// =============================================================================
// QuorumState - Replicated here to avoid Seastar dependencies
// =============================================================================

enum class QuorumState : uint8_t {
    HEALTHY = 1,   // Quorum maintained, full operations
    DEGRADED = 0,  // Quorum lost, read-only mode for routes
};

// =============================================================================
// Quorum Calculation Helper (matches gossip_service.cpp logic)
// =============================================================================

// Calculate quorum required: floor(N * threshold) + 1, capped at N
// N = total nodes (peers + self)
// For threshold=0.5, this gives majority: floor(N/2) + 1
// The cap prevents impossible requirements (e.g., threshold=1.0 giving N+1)
size_t calculate_quorum_required(size_t total_peers, double threshold) {
    size_t total_nodes = total_peers + 1;  // +1 for self
    size_t required = static_cast<size_t>(std::floor(total_nodes * threshold)) + 1;
    return std::min(required, total_nodes);  // Cap at total_nodes
}

// Raw (uncapped) calculation for testing the formula itself
size_t calculate_quorum_required_raw(size_t total_peers, double threshold) {
    size_t total_nodes = total_peers + 1;  // +1 for self
    return static_cast<size_t>(std::floor(total_nodes * threshold)) + 1;
}

// Determine quorum state based on alive nodes vs required
QuorumState calculate_quorum_state(size_t alive_peers, size_t total_peers, double threshold) {
    size_t alive_nodes = alive_peers + 1;  // +1 for self (always alive)
    size_t required = calculate_quorum_required(total_peers, threshold);
    return (alive_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;
}

// =============================================================================
// Quorum Calculation Tests
// =============================================================================

class QuorumTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(QuorumTest, QuorumRequiredMajority_3Nodes) {
    // 3 nodes total (2 peers + self), threshold=0.5
    // Required = floor(3 * 0.5) + 1 = floor(1.5) + 1 = 1 + 1 = 2
    EXPECT_EQ(calculate_quorum_required(2, 0.5), 2u);
}

TEST_F(QuorumTest, QuorumRequiredMajority_5Nodes) {
    // 5 nodes total (4 peers + self), threshold=0.5
    // Required = floor(5 * 0.5) + 1 = floor(2.5) + 1 = 2 + 1 = 3
    EXPECT_EQ(calculate_quorum_required(4, 0.5), 3u);
}

TEST_F(QuorumTest, QuorumRequiredMajority_7Nodes) {
    // 7 nodes total (6 peers + self), threshold=0.5
    // Required = floor(7 * 0.5) + 1 = floor(3.5) + 1 = 3 + 1 = 4
    EXPECT_EQ(calculate_quorum_required(6, 0.5), 4u);
}

TEST_F(QuorumTest, QuorumRequiredMajority_2Nodes) {
    // 2 nodes total (1 peer + self), threshold=0.5
    // Required = floor(2 * 0.5) + 1 = floor(1.0) + 1 = 1 + 1 = 2
    // Both nodes must be alive for quorum
    EXPECT_EQ(calculate_quorum_required(1, 0.5), 2u);
}

TEST_F(QuorumTest, QuorumRequiredMajority_SingleNode) {
    // 1 node (no peers, just self), threshold=0.5
    // Required = floor(1 * 0.5) + 1 = floor(0.5) + 1 = 0 + 1 = 1
    // Self is always alive, so single node always has quorum
    EXPECT_EQ(calculate_quorum_required(0, 0.5), 1u);
}

TEST_F(QuorumTest, QuorumRequiredHighThreshold) {
    // 5 nodes total, threshold=0.7 (stricter quorum)
    // Required = floor(5 * 0.7) + 1 = floor(3.5) + 1 = 3 + 1 = 4
    EXPECT_EQ(calculate_quorum_required(4, 0.7), 4u);
}

TEST_F(QuorumTest, QuorumRequiredLowThreshold) {
    // 5 nodes total, threshold=0.3 (looser quorum)
    // Required = floor(5 * 0.3) + 1 = floor(1.5) + 1 = 1 + 1 = 2
    EXPECT_EQ(calculate_quorum_required(4, 0.3), 2u);
}

// =============================================================================
// Quorum State Tests
// =============================================================================

TEST_F(QuorumTest, StateHealthy_AllPeersAlive) {
    // 5 nodes (4 peers + self), all 4 peers alive
    // Alive = 4 + 1 = 5, Required = 3 -> HEALTHY
    EXPECT_EQ(calculate_quorum_state(4, 4, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, StateHealthy_ExactQuorum) {
    // 5 nodes (4 peers + self), 2 peers alive + self = 3 nodes
    // Alive = 2 + 1 = 3, Required = 3 -> HEALTHY (exactly at quorum)
    EXPECT_EQ(calculate_quorum_state(2, 4, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, StateDegraded_BelowQuorum) {
    // 5 nodes (4 peers + self), 1 peer alive + self = 2 nodes
    // Alive = 1 + 1 = 2, Required = 3 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state(1, 4, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumTest, StateDegraded_NoPeersAlive) {
    // 5 nodes (4 peers + self), 0 peers alive + self = 1 node
    // Alive = 0 + 1 = 1, Required = 3 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state(0, 4, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumTest, StateHealthy_SingleNode) {
    // 1 node (no peers, just self)
    // Alive = 0 + 1 = 1, Required = 1 -> HEALTHY
    // Single node cluster always has quorum
    EXPECT_EQ(calculate_quorum_state(0, 0, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, StateHealthy_TwoNodes_BothAlive) {
    // 2 nodes (1 peer + self), peer alive
    // Alive = 1 + 1 = 2, Required = 2 -> HEALTHY
    EXPECT_EQ(calculate_quorum_state(1, 1, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, StateDegraded_TwoNodes_PeerDead) {
    // 2 nodes (1 peer + self), peer dead
    // Alive = 0 + 1 = 1, Required = 2 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state(0, 1, 0.5), QuorumState::DEGRADED);
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(QuorumTest, LargeCluster_QuorumCalculation) {
    // 100 nodes (99 peers + self), threshold=0.5
    // Required = floor(100 * 0.5) + 1 = 50 + 1 = 51
    EXPECT_EQ(calculate_quorum_required(99, 0.5), 51u);
}

TEST_F(QuorumTest, LargeCluster_ExactlyHalfDead) {
    // 100 nodes (99 peers + self), 49 peers alive + self = 50 nodes
    // Alive = 49 + 1 = 50, Required = 51 -> DEGRADED (just below quorum)
    EXPECT_EQ(calculate_quorum_state(49, 99, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumTest, LargeCluster_ExactlyHalfPlusOneAlive) {
    // 100 nodes (99 peers + self), 50 peers alive + self = 51 nodes
    // Alive = 50 + 1 = 51, Required = 51 -> HEALTHY (exactly at quorum)
    EXPECT_EQ(calculate_quorum_state(50, 99, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, Threshold_Zero) {
    // threshold=0 means only 1 node required (always healthy if self is alive)
    // Required = floor(5 * 0) + 1 = 0 + 1 = 1
    EXPECT_EQ(calculate_quorum_required(4, 0.0), 1u);
    EXPECT_EQ(calculate_quorum_state(0, 4, 0.0), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, Threshold_One_RawFormula) {
    // threshold=1.0 means all nodes required
    // 5 nodes: Raw formula = floor(5 * 1.0) + 1 = 5 + 1 = 6
    // This exceeds total nodes, which is why we cap
    size_t required_raw = calculate_quorum_required_raw(4, 1.0);
    EXPECT_EQ(required_raw, 6u);
}

TEST_F(QuorumTest, Threshold_One_Capped) {
    // With capping (matches actual implementation), threshold=1.0 is capped at N
    // 5 nodes: capped to min(6, 5) = 5
    size_t required = calculate_quorum_required(4, 1.0);
    EXPECT_EQ(required, 5u);

    // With all nodes alive, we have 5, need 5 -> HEALTHY
    EXPECT_EQ(calculate_quorum_state(4, 4, 1.0), QuorumState::HEALTHY);

    // If one node is down, have 4, need 5 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state(3, 4, 1.0), QuorumState::DEGRADED);
}

// =============================================================================
// Quorum State Enum Tests
// =============================================================================

TEST_F(QuorumTest, QuorumStateEnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(QuorumState::HEALTHY), 1u);
    EXPECT_EQ(static_cast<uint8_t>(QuorumState::DEGRADED), 0u);
}

// =============================================================================
// Warning Threshold Tests (simulated logic)
// =============================================================================

bool should_warn_quorum_loss(size_t alive_peers, size_t total_peers,
                              double threshold, uint32_t warning_threshold) {
    size_t alive_nodes = alive_peers + 1;
    size_t required = calculate_quorum_required(total_peers, threshold);
    QuorumState state = (alive_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;

    if (state == QuorumState::HEALTHY && warning_threshold > 0) {
        size_t margin = alive_nodes - required;
        return margin <= warning_threshold;
    }
    return false;
}

TEST_F(QuorumTest, WarningThreshold_NotTriggered) {
    // 5 nodes, all alive -> margin = 5 - 3 = 2, warning_threshold = 1
    // margin(2) > threshold(1) -> no warning
    EXPECT_FALSE(should_warn_quorum_loss(4, 4, 0.5, 1));
}

TEST_F(QuorumTest, WarningThreshold_Triggered) {
    // 5 nodes, 3 alive -> margin = 3 - 3 = 0, warning_threshold = 1
    // margin(0) <= threshold(1) -> warning!
    EXPECT_TRUE(should_warn_quorum_loss(2, 4, 0.5, 1));
}

TEST_F(QuorumTest, WarningThreshold_ExactlyAtThreshold) {
    // 5 nodes, 4 alive -> margin = 4 - 3 = 1, warning_threshold = 1
    // margin(1) <= threshold(1) -> warning!
    EXPECT_TRUE(should_warn_quorum_loss(3, 4, 0.5, 1));
}

TEST_F(QuorumTest, WarningThreshold_Disabled) {
    // warning_threshold = 0 means warnings disabled
    EXPECT_FALSE(should_warn_quorum_loss(2, 4, 0.5, 0));
}

TEST_F(QuorumTest, WarningThreshold_NotTriggeredWhenDegraded) {
    // When already degraded, don't trigger warning (only warn when healthy)
    // 5 nodes, 1 alive -> state = DEGRADED, no warning
    EXPECT_FALSE(should_warn_quorum_loss(1, 4, 0.5, 5));
}

// =============================================================================
// Warning Rate Limiting Tests (simulated logic)
// =============================================================================
// Note: The actual implementation in GossipService tracks _quorum_warning_active
// to avoid log spam. These tests verify the triggering logic, not the rate limiting.
// Full rate limiting behavior is tested via integration tests.

TEST_F(QuorumTest, WarningRateLimiting_EnterWarningZone) {
    // Simulates the state machine for warning rate limiting
    // Note: In actual implementation, state transition is logged before warning
    struct WarningState {
        bool warning_active = false;
        QuorumState current_state = QuorumState::HEALTHY;

        // Returns true if a warning should be logged (entering warning zone)
        bool check_and_update(size_t alive_peers, size_t total_peers,
                              double threshold, uint32_t warning_threshold) {
            size_t alive_nodes = alive_peers + 1;
            size_t required = calculate_quorum_required(total_peers, threshold);  // Already capped
            QuorumState new_state = (alive_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;

            // Handle state transition first (as in actual implementation)
            if (new_state != current_state) {
                if (new_state == QuorumState::DEGRADED) {
                    warning_active = false;
                }
                current_state = new_state;
            }

            // Then check warning zone
            if (new_state == QuorumState::HEALTHY && warning_threshold > 0) {
                size_t margin = alive_nodes - required;
                bool should_warn = margin <= warning_threshold;

                if (should_warn && !warning_active) {
                    warning_active = true;
                    return true;  // Log warning
                } else if (!should_warn && warning_active) {
                    warning_active = false;
                    return false;  // Log "warning cleared" (separate path)
                }
            }
            return false;  // No change, no log
        }
    };

    WarningState state;

    // Initial state: 5 nodes, all alive, margin = 2, no warning
    EXPECT_FALSE(state.check_and_update(4, 4, 0.5, 1));
    EXPECT_FALSE(state.warning_active);

    // One peer dies: margin = 1, enters warning zone
    EXPECT_TRUE(state.check_and_update(3, 4, 0.5, 1));
    EXPECT_TRUE(state.warning_active);

    // Still at margin = 1, already in warning zone, no new log
    EXPECT_FALSE(state.check_and_update(3, 4, 0.5, 1));
    EXPECT_TRUE(state.warning_active);

    // Peer recovers: margin = 2, exits warning zone
    EXPECT_FALSE(state.check_and_update(4, 4, 0.5, 1));
    EXPECT_FALSE(state.warning_active);

    // Another peer dies and we enter warning zone again
    EXPECT_TRUE(state.check_and_update(3, 4, 0.5, 1));
    EXPECT_TRUE(state.warning_active);
}

// =============================================================================
// Recently-Seen Quorum Calculation Tests (check_quorum logic)
// =============================================================================
// The check_quorum() method uses a stricter "recently seen within window" count
// rather than just the alive/dead state. These tests validate that logic.

struct PeerState {
    int64_t last_seen_seconds_ago;  // How many seconds ago was this peer seen
    bool is_alive;
};

// Count peers seen within the quorum check window
size_t count_recently_seen_peers(const std::vector<PeerState>& peers,
                                   int64_t window_seconds) {
    size_t count = 0;
    for (const auto& peer : peers) {
        if (peer.last_seen_seconds_ago <= window_seconds) {
            ++count;
        }
    }
    return count;
}

// Calculate quorum state using recently-seen count (stricter than alive count)
QuorumState calculate_quorum_state_recently_seen(
    const std::vector<PeerState>& peers,
    int64_t window_seconds,
    double threshold) {

    size_t recently_seen = count_recently_seen_peers(peers, window_seconds);
    size_t recently_seen_nodes = recently_seen + 1;  // +1 for self
    size_t required = calculate_quorum_required(peers.size(), threshold);

    return (recently_seen_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;
}

TEST_F(QuorumTest, RecentlySeen_AllPeersRecentlySeen) {
    // 5 nodes (4 peers + self), all seen within 30s window
    std::vector<PeerState> peers = {
        {5, true}, {10, true}, {15, true}, {20, true}
    };

    EXPECT_EQ(count_recently_seen_peers(peers, 30), 4u);
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers, 30, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, RecentlySeen_SomePeersStale) {
    // 5 nodes (4 peers + self), 2 seen recently, 2 are stale
    std::vector<PeerState> peers = {
        {5, true},   // Recently seen
        {10, true},  // Recently seen
        {40, true},  // Stale (beyond 30s window) but still marked alive
        {60, true}   // Stale (beyond 30s window) but still marked alive
    };

    // Only 2 peers seen recently + self = 3, need 3 -> HEALTHY
    EXPECT_EQ(count_recently_seen_peers(peers, 30), 2u);
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers, 30, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, RecentlySeen_TooManyStale) {
    // 5 nodes (4 peers + self), only 1 seen recently
    std::vector<PeerState> peers = {
        {5, true},   // Recently seen
        {40, true},  // Stale
        {50, true},  // Stale
        {60, true}   // Stale
    };

    // Only 1 peer seen recently + self = 2, need 3 -> DEGRADED
    EXPECT_EQ(count_recently_seen_peers(peers, 30), 1u);
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers, 30, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumTest, RecentlySeen_AllPeersStale) {
    // 5 nodes (4 peers + self), none seen recently
    std::vector<PeerState> peers = {
        {40, true}, {50, true}, {60, true}, {70, true}
    };

    // Only self = 1, need 3 -> DEGRADED
    EXPECT_EQ(count_recently_seen_peers(peers, 30), 0u);
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers, 30, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumTest, RecentlySeen_ExactlyAtWindowBoundary) {
    // Test edge case: peer seen exactly at window boundary
    std::vector<PeerState> peers = {
        {30, true},  // Exactly at boundary (should count as recently seen)
        {31, true},  // Just beyond boundary (should NOT count)
    };

    // 1 peer at boundary + self = 2, need 2 for 2-peer cluster
    EXPECT_EQ(count_recently_seen_peers(peers, 30), 1u);
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers, 30, 0.5), QuorumState::HEALTHY);
}

TEST_F(QuorumTest, RecentlySeen_ConfigurableWindow) {
    // Test with different window sizes
    std::vector<PeerState> peers = {
        {10, true}, {20, true}, {40, true}, {60, true}
    };

    // With 15s window: only 1 peer seen recently
    EXPECT_EQ(count_recently_seen_peers(peers, 15), 1u);

    // With 30s window: 2 peers seen recently
    EXPECT_EQ(count_recently_seen_peers(peers, 30), 2u);

    // With 50s window: 3 peers seen recently
    EXPECT_EQ(count_recently_seen_peers(peers, 50), 3u);

    // With 90s window: all 4 peers seen recently
    EXPECT_EQ(count_recently_seen_peers(peers, 90), 4u);
}

TEST_F(QuorumTest, RecentlySeen_DifferentFromAliveCount) {
    // Demonstrate that recently-seen is stricter than alive-count
    std::vector<PeerState> peers = {
        {5, true},    // Recent and alive
        {10, true},   // Recent and alive
        {40, true},   // NOT recent but alive (would pass alive check)
        {50, false},  // Neither recent nor alive
    };

    // Alive count: 3 peers (ignoring is_alive=false)
    // Recently seen: only 2 peers

    // Using alive count (less strict): 3 + 1 = 4 >= 3 -> HEALTHY
    EXPECT_EQ(calculate_quorum_state(3, 4, 0.5), QuorumState::HEALTHY);

    // Using recently-seen (stricter): 2 + 1 = 3 >= 3 -> HEALTHY (just barely)
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers, 30, 0.5), QuorumState::HEALTHY);

    // If we lose one more recent peer:
    std::vector<PeerState> peers2 = {
        {5, true},    // Recent and alive
        {40, true},   // NOT recent but alive
        {50, true},   // NOT recent but alive
        {60, false},  // NOT recent and not alive
    };

    // Alive count: 3 peers -> 3 + 1 = 4 >= 3 -> HEALTHY
    EXPECT_EQ(calculate_quorum_state(3, 4, 0.5), QuorumState::HEALTHY);

    // Recently-seen: 1 peer -> 1 + 1 = 2 < 3 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state_recently_seen(peers2, 30, 0.5), QuorumState::DEGRADED);
}

// =============================================================================
// Sequence Number Hardening Tests (Replay Attack Prevention)
// =============================================================================

// Simulate the sliding window duplicate detection
class SequenceWindow {
public:
    explicit SequenceWindow(size_t max_size) : _max_size(max_size) {}

    // Returns true if duplicate (already seen)
    bool is_duplicate(uint32_t seq_num) {
        if (_seen.count(seq_num) > 0) {
            return true;
        }

        _seen.insert(seq_num);
        _window.push_back(seq_num);

        // Slide window
        while (_window.size() > _max_size) {
            uint32_t oldest = _window.front();
            _window.pop_front();
            _seen.erase(oldest);
        }

        return false;
    }

    size_t size() const { return _window.size(); }
    bool contains(uint32_t seq_num) const { return _seen.count(seq_num) > 0; }

    // SECURITY: This simulates what should happen during resync
    // Notice we do NOT clear the window - this prevents replay attacks
    void simulate_resync() {
        // DO NOT clear _window or _seen!
        // Clearing would allow replay attacks
    }

private:
    size_t _max_size;
    std::deque<uint32_t> _window;
    std::unordered_set<uint32_t> _seen;
};

TEST_F(QuorumTest, SequenceWindow_DetectsDuplicates) {
    SequenceWindow window(100);

    // First occurrence is not a duplicate
    EXPECT_FALSE(window.is_duplicate(1));
    EXPECT_FALSE(window.is_duplicate(2));
    EXPECT_FALSE(window.is_duplicate(3));

    // Second occurrence IS a duplicate
    EXPECT_TRUE(window.is_duplicate(1));
    EXPECT_TRUE(window.is_duplicate(2));
    EXPECT_TRUE(window.is_duplicate(3));
}

TEST_F(QuorumTest, SequenceWindow_SlidesCorrectly) {
    SequenceWindow window(3);  // Small window for testing

    // Fill window
    EXPECT_FALSE(window.is_duplicate(1));
    EXPECT_FALSE(window.is_duplicate(2));
    EXPECT_FALSE(window.is_duplicate(3));

    // Window is now: [1, 2, 3]
    EXPECT_EQ(window.size(), 3u);
    EXPECT_TRUE(window.contains(1));
    EXPECT_TRUE(window.contains(2));
    EXPECT_TRUE(window.contains(3));

    // Add one more - should evict oldest (1)
    EXPECT_FALSE(window.is_duplicate(4));

    // Window is now: [2, 3, 4]
    EXPECT_EQ(window.size(), 3u);
    EXPECT_FALSE(window.contains(1));  // Evicted
    EXPECT_TRUE(window.contains(2));
    EXPECT_TRUE(window.contains(3));
    EXPECT_TRUE(window.contains(4));

    // 1 can now be reused (no longer in window)
    EXPECT_FALSE(window.is_duplicate(1));
}

TEST_F(QuorumTest, SequenceWindow_PersistsAcrossResync) {
    SequenceWindow window(100);

    // Record some sequence numbers
    EXPECT_FALSE(window.is_duplicate(100));
    EXPECT_FALSE(window.is_duplicate(101));
    EXPECT_FALSE(window.is_duplicate(102));

    // Simulate resync - window should persist
    window.simulate_resync();

    // CRITICAL: Replay attack attempt should still be detected!
    // If window was cleared, this would return false (security vulnerability)
    EXPECT_TRUE(window.is_duplicate(100));
    EXPECT_TRUE(window.is_duplicate(101));
    EXPECT_TRUE(window.is_duplicate(102));

    // New sequence numbers should still work
    EXPECT_FALSE(window.is_duplicate(103));
}

TEST_F(QuorumTest, SequenceWindow_ReplayAttackPrevention) {
    // Simulate a replay attack scenario
    SequenceWindow window(1000);

    // Attacker captures sequence numbers 1-50
    for (uint32_t i = 1; i <= 50; ++i) {
        EXPECT_FALSE(window.is_duplicate(i));
    }

    // System undergoes resync (network partition recovery, etc.)
    window.simulate_resync();

    // Attacker attempts to replay captured packets
    // WITHOUT the security fix (clearing window), all would return false
    // WITH the security fix (preserving window), all should return true
    for (uint32_t i = 1; i <= 50; ++i) {
        EXPECT_TRUE(window.is_duplicate(i))
            << "Replay attack not detected for seq_num=" << i;
    }

    // Normal operation continues with new sequence numbers
    EXPECT_FALSE(window.is_duplicate(51));
    EXPECT_FALSE(window.is_duplicate(52));
}

// =============================================================================
// Fail-Open Mode Tests
// =============================================================================
// Fail-open mode enables random routing to healthy backends during split-brain.
// This is useful for inference workloads that prioritize availability over
// strict routing consistency.

// Simulates GossipService::is_fail_open_mode() logic
bool is_fail_open_mode(bool quorum_enabled, bool fail_open_on_quorum_loss, QuorumState state) {
    return quorum_enabled && fail_open_on_quorum_loss && (state == QuorumState::DEGRADED);
}

// Simulates routing decision: should we use random routing?
// Returns: "random" for fail-open, "prefix" for normal routing
std::string get_routing_mode(bool quorum_enabled, bool fail_open_on_quorum_loss,
                               QuorumState state, std::string normal_mode) {
    if (is_fail_open_mode(quorum_enabled, fail_open_on_quorum_loss, state)) {
        return "random";
    }
    return normal_mode;
}

// Simulates incoming gossip acceptance logic
// Returns: true if gossip should be accepted, false if rejected
bool should_accept_incoming_gossip(bool quorum_enabled, bool reject_routes_on_quorum_loss,
                                     bool accept_gossip_on_quorum_loss, QuorumState state) {
    if (!quorum_enabled) {
        return true;  // Quorum disabled, always accept
    }
    if (state == QuorumState::HEALTHY) {
        return true;  // Healthy cluster, always accept
    }
    // DEGRADED state
    if (accept_gossip_on_quorum_loss) {
        return true;  // Fail-open: stale data > no data
    }
    if (reject_routes_on_quorum_loss) {
        return false;  // Fail-closed: reject stale gossip
    }
    return true;  // Neither flag set, default accept
}

// Simulates outbound route broadcast logic
// Returns: true if broadcast should proceed, false if rejected
bool should_broadcast_route(bool quorum_enabled, bool reject_routes_on_quorum_loss,
                              bool fail_open_on_quorum_loss, QuorumState state) {
    if (!quorum_enabled) {
        return true;  // Quorum disabled, always broadcast
    }
    if (state == QuorumState::HEALTHY) {
        return true;  // Healthy cluster, always broadcast
    }
    // DEGRADED state
    if (fail_open_on_quorum_loss) {
        return true;  // Fail-open: allow broadcast despite quorum loss
    }
    if (reject_routes_on_quorum_loss) {
        return false;  // Fail-closed: reject broadcast
    }
    return true;  // Neither flag set, default allow
}

TEST_F(QuorumTest, FailOpenMode_DisabledByDefault) {
    // Default: fail_open_on_quorum_loss = false
    // Even when degraded, fail-open mode is not active
    EXPECT_FALSE(is_fail_open_mode(true, false, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, FailOpenMode_EnabledWhenDegraded) {
    // fail_open_on_quorum_loss = true, state = DEGRADED
    // Fail-open mode should be active
    EXPECT_TRUE(is_fail_open_mode(true, true, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, FailOpenMode_NotActiveWhenHealthy) {
    // Even with fail_open_on_quorum_loss = true, healthy cluster doesn't trigger fail-open
    EXPECT_FALSE(is_fail_open_mode(true, true, QuorumState::HEALTHY));
}

TEST_F(QuorumTest, FailOpenMode_NotActiveWhenQuorumDisabled) {
    // quorum_enabled = false means no fail-open check
    EXPECT_FALSE(is_fail_open_mode(false, true, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, RoutingMode_NormalWhenHealthy) {
    // Healthy cluster uses normal routing mode
    EXPECT_EQ(get_routing_mode(true, true, QuorumState::HEALTHY, "prefix"), "prefix");
    EXPECT_EQ(get_routing_mode(true, true, QuorumState::HEALTHY, "hash"), "hash");
}

TEST_F(QuorumTest, RoutingMode_RandomWhenFailOpen) {
    // Degraded cluster with fail-open uses random routing
    EXPECT_EQ(get_routing_mode(true, true, QuorumState::DEGRADED, "prefix"), "random");
    EXPECT_EQ(get_routing_mode(true, true, QuorumState::DEGRADED, "hash"), "random");
}

TEST_F(QuorumTest, RoutingMode_NormalWhenFailOpenDisabled) {
    // Degraded cluster without fail-open keeps normal routing
    // (request handling happens elsewhere, this just shows routing decision)
    EXPECT_EQ(get_routing_mode(true, false, QuorumState::DEGRADED, "prefix"), "prefix");
}

TEST_F(QuorumTest, IncomingGossip_AcceptedWhenHealthy) {
    // Healthy cluster always accepts incoming gossip
    EXPECT_TRUE(should_accept_incoming_gossip(true, true, false, QuorumState::HEALTHY));
}

TEST_F(QuorumTest, IncomingGossip_RejectedWhenDegraded_FailClosed) {
    // Degraded + reject_routes_on_quorum_loss = true + accept_gossip_on_quorum_loss = false
    // Should reject incoming gossip
    EXPECT_FALSE(should_accept_incoming_gossip(true, true, false, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, IncomingGossip_AcceptedWhenDegraded_FailOpen) {
    // Degraded + accept_gossip_on_quorum_loss = true
    // Should accept incoming gossip (stale data > no data)
    EXPECT_TRUE(should_accept_incoming_gossip(true, true, true, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, IncomingGossip_AcceptedWhenQuorumDisabled) {
    // quorum_enabled = false means always accept
    EXPECT_TRUE(should_accept_incoming_gossip(false, true, false, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, BroadcastRoute_AllowedWhenHealthy) {
    // Healthy cluster always allows route broadcast
    EXPECT_TRUE(should_broadcast_route(true, true, false, QuorumState::HEALTHY));
}

TEST_F(QuorumTest, BroadcastRoute_RejectedWhenDegraded_FailClosed) {
    // Degraded + reject_routes_on_quorum_loss = true + fail_open_on_quorum_loss = false
    // Should reject route broadcast
    EXPECT_FALSE(should_broadcast_route(true, true, false, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, BroadcastRoute_AllowedWhenDegraded_FailOpen) {
    // Degraded + fail_open_on_quorum_loss = true
    // Should allow route broadcast
    EXPECT_TRUE(should_broadcast_route(true, true, true, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, BroadcastRoute_AllowedWhenQuorumDisabled) {
    // quorum_enabled = false means always allow
    EXPECT_TRUE(should_broadcast_route(false, true, false, QuorumState::DEGRADED));
}

TEST_F(QuorumTest, FailOpenMode_ConfigCombinations) {
    // Test all config flag combinations for completeness

    // Matrix: quorum_enabled × fail_open_on_quorum_loss × state
    // quorum_enabled=false always returns false (no fail-open)
    EXPECT_FALSE(is_fail_open_mode(false, false, QuorumState::HEALTHY));
    EXPECT_FALSE(is_fail_open_mode(false, false, QuorumState::DEGRADED));
    EXPECT_FALSE(is_fail_open_mode(false, true, QuorumState::HEALTHY));
    EXPECT_FALSE(is_fail_open_mode(false, true, QuorumState::DEGRADED));

    // quorum_enabled=true, fail_open_on_quorum_loss=false always returns false
    EXPECT_FALSE(is_fail_open_mode(true, false, QuorumState::HEALTHY));
    EXPECT_FALSE(is_fail_open_mode(true, false, QuorumState::DEGRADED));

    // quorum_enabled=true, fail_open_on_quorum_loss=true depends on state
    EXPECT_FALSE(is_fail_open_mode(true, true, QuorumState::HEALTHY));  // Healthy = no fail-open
    EXPECT_TRUE(is_fail_open_mode(true, true, QuorumState::DEGRADED));  // Degraded = fail-open
}

TEST_F(QuorumTest, FailOpen_IndependentFlags) {
    // Verify fail_open_on_quorum_loss and accept_gossip_on_quorum_loss are independent

    // Scenario 1: fail-open routing but reject gossip
    // (serve traffic but don't accept potentially stale routes)
    bool fail_open_routing = is_fail_open_mode(true, true, QuorumState::DEGRADED);
    bool accept_gossip = should_accept_incoming_gossip(true, true, false, QuorumState::DEGRADED);
    EXPECT_TRUE(fail_open_routing);
    EXPECT_FALSE(accept_gossip);

    // Scenario 2: fail-closed routing but accept gossip
    // (don't serve traffic but keep collecting routing info for recovery)
    fail_open_routing = is_fail_open_mode(true, false, QuorumState::DEGRADED);
    accept_gossip = should_accept_incoming_gossip(true, false, true, QuorumState::DEGRADED);
    EXPECT_FALSE(fail_open_routing);
    EXPECT_TRUE(accept_gossip);

    // Scenario 3: full fail-open (both enabled)
    fail_open_routing = is_fail_open_mode(true, true, QuorumState::DEGRADED);
    accept_gossip = should_accept_incoming_gossip(true, true, true, QuorumState::DEGRADED);
    EXPECT_TRUE(fail_open_routing);
    EXPECT_TRUE(accept_gossip);

    // Scenario 4: full fail-closed (both disabled, default)
    fail_open_routing = is_fail_open_mode(true, false, QuorumState::DEGRADED);
    accept_gossip = should_accept_incoming_gossip(true, true, false, QuorumState::DEGRADED);
    EXPECT_FALSE(fail_open_routing);
    EXPECT_FALSE(accept_gossip);
}

// =============================================================================
// Deterministic Peer Liveness Tests (TestClock - no sleeps, instant execution)
// =============================================================================
// These tests replicate the check_liveness() logic from GossipConsensus using
// TestClock for deterministic time control. This avoids the Seastar dependency
// while testing the same timing logic.

using namespace ranvier;

// Clock-aware peer state for deterministic liveness testing
template<typename Clock>
struct ClockPeerState {
    typename Clock::time_point last_seen;
    bool is_alive = true;
};

// Replicate GossipConsensus::check_liveness() logic with injectable clock
template<typename Clock>
void check_peer_liveness(std::vector<ClockPeerState<Clock>>& peers,
                          std::chrono::seconds liveness_timeout) {
    auto now = Clock::now();
    for (auto& peer : peers) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - peer.last_seen);
        if (elapsed > liveness_timeout) {
            peer.is_alive = false;
        }
    }
}

// Count alive peers for quorum calculation
template<typename Clock>
size_t count_alive_peers(const std::vector<ClockPeerState<Clock>>& peers) {
    size_t count = 0;
    for (const auto& peer : peers) {
        if (peer.is_alive) {
            ++count;
        }
    }
    return count;
}

// Count peers seen within a time window (stricter than alive check)
template<typename Clock>
size_t count_recently_seen_clock(const std::vector<ClockPeerState<Clock>>& peers,
                                  std::chrono::seconds window) {
    auto now = Clock::now();
    size_t count = 0;
    for (const auto& peer : peers) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - peer.last_seen);
        if (elapsed <= window) {
            ++count;
        }
    }
    return count;
}

class QuorumLivenessTimingTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(QuorumLivenessTimingTest, PeerAliveBeforeLivenessTimeout) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
    };

    // Advance to just before timeout (30s)
    TestClock::advance(std::chrono::seconds(29));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    EXPECT_TRUE(peers[0].is_alive);
}

TEST_F(QuorumLivenessTimingTest, PeerDeadAfterLivenessTimeout) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
    };

    // Advance past timeout
    TestClock::advance(std::chrono::seconds(31));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    EXPECT_FALSE(peers[0].is_alive);
}

TEST_F(QuorumLivenessTimingTest, PeerAliveAtExactTimeout) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
    };

    // Advance to exactly the timeout (not past it)
    TestClock::advance(std::chrono::seconds(30));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    // At exactly timeout, elapsed == timeout, not > timeout, so still alive
    EXPECT_TRUE(peers[0].is_alive);
}

TEST_F(QuorumLivenessTimingTest, HeartbeatRefreshesLiveness) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
    };

    // Advance 20 seconds
    TestClock::advance(std::chrono::seconds(20));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));
    EXPECT_TRUE(peers[0].is_alive);

    // Simulate heartbeat: update last_seen to current time
    peers[0].last_seen = TestClock::now();

    // Advance another 20 seconds (40 total, but only 20 since heartbeat)
    TestClock::advance(std::chrono::seconds(20));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    // Should still be alive (only 20s since last heartbeat)
    EXPECT_TRUE(peers[0].is_alive);
}

TEST_F(QuorumLivenessTimingTest, MultiplePeersWithDifferentTimestamps) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},                                                    // Peer 0: seen at t=0
        {start, true},                                                    // Peer 1: seen at t=0
        {start, true},                                                    // Peer 2: seen at t=0
    };

    // Advance 10 seconds, peer 1 sends heartbeat
    TestClock::advance(std::chrono::seconds(10));
    peers[1].last_seen = TestClock::now();

    // Advance 10 more seconds, peer 2 sends heartbeat
    TestClock::advance(std::chrono::seconds(10));
    peers[2].last_seen = TestClock::now();

    // Advance to t=31 (31s since start)
    TestClock::advance(std::chrono::seconds(11));

    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    // Peer 0: last_seen at t=0, now at t=31, elapsed=31s > 30s -> DEAD
    EXPECT_FALSE(peers[0].is_alive);

    // Peer 1: last_seen at t=10, now at t=31, elapsed=21s <= 30s -> ALIVE
    EXPECT_TRUE(peers[1].is_alive);

    // Peer 2: last_seen at t=20, now at t=31, elapsed=11s <= 30s -> ALIVE
    EXPECT_TRUE(peers[2].is_alive);
}

TEST_F(QuorumLivenessTimingTest, LivenessAffectsQuorumState) {
    auto start = TestClock::now();

    // 5 nodes total (4 peers + self), need 3 for quorum (threshold=0.5)
    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
        {start, true},
        {start, true},
        {start, true},
    };

    // All alive: 4 peers + self = 5, need 3 -> HEALTHY
    size_t alive = count_alive_peers(peers);
    EXPECT_EQ(alive, 4u);
    EXPECT_EQ(calculate_quorum_state(alive, 4, 0.5), QuorumState::HEALTHY);

    // Advance 31s - all peers go dead
    TestClock::advance(std::chrono::seconds(31));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    alive = count_alive_peers(peers);
    EXPECT_EQ(alive, 0u);
    EXPECT_EQ(calculate_quorum_state(alive, 4, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumLivenessTimingTest, GradualPeerLossDegradsQuorum) {
    auto start = TestClock::now();

    // 5 nodes (4 peers + self), need 3 for quorum
    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},  // Peer 0: never refreshes
        {start, true},  // Peer 1: never refreshes
        {start, true},  // Peer 2: refreshes at t=25
        {start, true},  // Peer 3: refreshes at t=25
    };

    // At t=25: refresh peers 2 and 3
    TestClock::advance(std::chrono::seconds(25));
    peers[2].last_seen = TestClock::now();
    peers[3].last_seen = TestClock::now();

    // At t=31: peers 0,1 dead (31s since t=0), peers 2,3 alive (6s since t=25)
    TestClock::advance(std::chrono::seconds(6));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    size_t alive = count_alive_peers(peers);
    EXPECT_EQ(alive, 2u);
    // 2 alive peers + self = 3, need 3 -> HEALTHY (exactly at quorum)
    EXPECT_EQ(calculate_quorum_state(alive, 4, 0.5), QuorumState::HEALTHY);

    // Advance to t=56: peers 2,3 now dead too (31s since t=25 refresh)
    TestClock::advance(std::chrono::seconds(25));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));

    alive = count_alive_peers(peers);
    EXPECT_EQ(alive, 0u);
    // 0 alive peers + self = 1, need 3 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state(alive, 4, 0.5), QuorumState::DEGRADED);
}

TEST_F(QuorumLivenessTimingTest, RecentlySeenWindowWithTestClock) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
        {start, true},
        {start, true},
        {start, true},
    };

    // Peer 0 refreshes at t=10
    TestClock::advance(std::chrono::seconds(10));
    peers[0].last_seen = TestClock::now();

    // Peer 1 refreshes at t=20
    TestClock::advance(std::chrono::seconds(10));
    peers[1].last_seen = TestClock::now();

    // Check at t=35: peers 2,3 last seen 35s ago, peer 0 at 25s ago, peer 1 at 15s ago
    TestClock::advance(std::chrono::seconds(15));

    // With 30s window: peers 0 (25s) and 1 (15s) are recent
    size_t recent = count_recently_seen_clock<TestClock>(peers, std::chrono::seconds(30));
    EXPECT_EQ(recent, 2u);

    // With 20s window: only peer 1 (15s) is recent
    recent = count_recently_seen_clock<TestClock>(peers, std::chrono::seconds(20));
    EXPECT_EQ(recent, 1u);

    // With 40s window: all peers are recent
    recent = count_recently_seen_clock<TestClock>(peers, std::chrono::seconds(40));
    EXPECT_EQ(recent, 4u);
}

TEST_F(QuorumLivenessTimingTest, ConfigurableLivenessTimeout) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
    };

    // Test with short timeout (5 seconds)
    TestClock::advance(std::chrono::seconds(4));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(5));
    EXPECT_TRUE(peers[0].is_alive);

    TestClock::advance(std::chrono::seconds(2));  // Now at 6s
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(5));
    EXPECT_FALSE(peers[0].is_alive);

    // Reset peer and test with long timeout (60 seconds)
    peers[0].is_alive = true;
    peers[0].last_seen = TestClock::now();

    TestClock::advance(std::chrono::seconds(59));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(60));
    EXPECT_TRUE(peers[0].is_alive);

    TestClock::advance(std::chrono::seconds(2));  // 61s since last seen
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(60));
    EXPECT_FALSE(peers[0].is_alive);
}

TEST_F(QuorumLivenessTimingTest, PeerRecoveryAfterDeath) {
    auto start = TestClock::now();

    std::vector<ClockPeerState<TestClock>> peers = {
        {start, true},
    };

    // Peer dies at t=31
    TestClock::advance(std::chrono::seconds(31));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));
    EXPECT_FALSE(peers[0].is_alive);

    // Peer sends heartbeat -> recovers
    peers[0].last_seen = TestClock::now();
    peers[0].is_alive = true;

    // Verify still alive after 10 more seconds
    TestClock::advance(std::chrono::seconds(10));
    check_peer_liveness<TestClock>(peers, std::chrono::seconds(30));
    EXPECT_TRUE(peers[0].is_alive);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
