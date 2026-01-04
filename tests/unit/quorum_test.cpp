// Ranvier Core - Quorum / Split-Brain Detection Unit Tests
//
// Tests for quorum calculation and state transitions.
// These tests don't require Seastar runtime.

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>

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

// Calculate quorum required: floor(N * threshold) + 1
// N = total nodes (peers + self)
// For threshold=0.5, this gives majority: floor(N/2) + 1
size_t calculate_quorum_required(size_t total_peers, double threshold) {
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

TEST_F(QuorumTest, Threshold_One) {
    // threshold=1.0 means all nodes required
    // 5 nodes: Required = floor(5 * 1.0) + 1 = 5 + 1 = 6
    // Wait, that's more than total nodes! This is a pathological case.
    // In practice, threshold should be < 1.0
    size_t required = calculate_quorum_required(4, 1.0);
    EXPECT_EQ(required, 6u);
    // With all peers alive, we have 5 nodes but need 6 -> DEGRADED
    EXPECT_EQ(calculate_quorum_state(4, 4, 1.0), QuorumState::DEGRADED);
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
