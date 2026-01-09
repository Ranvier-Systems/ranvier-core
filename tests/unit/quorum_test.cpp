// Ranvier Core - Quorum / Split-Brain Detection Unit Tests
//
// Tests for quorum calculation and state transitions.
// These tests don't require Seastar runtime.

#include <gtest/gtest.h>
#include <algorithm>
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
