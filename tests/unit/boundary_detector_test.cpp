// Ranvier Core - Boundary Detector Unit Tests
//
// Tests for the pure boundary detection strategies: marker scan (exact) and
// proportional estimation (approximate).  Both functions are pure —
// no I/O, no mocks, no async infrastructure required.

#include "boundary_detector.hpp"
#include <gtest/gtest.h>

using namespace ranvier;

// Helper to build a TextWithBoundaryInfo with common fields.
static TextWithBoundaryInfo make_text_info(
        size_t total_messages,
        size_t system_messages,
        std::vector<size_t> char_ends = {},
        size_t system_prefix_end = 0,
        bool has_system_prefix = false) {
    TextWithBoundaryInfo info;
    info.total_message_count = total_messages;
    info.system_message_count = system_messages;
    info.has_system_messages = (system_messages > 0);
    info.has_system_prefix = has_system_prefix;
    info.system_prefix_end = system_prefix_end;
    info.message_char_ends = std::move(char_ends);
    return info;
}

// =============================================================================
// Marker Scan Tests
// =============================================================================

class MarkerScanTest : public ::testing::Test {
protected:
    BoundaryDetectionConfig multi_depth{true, 4};
    BoundaryDetectionConfig single_depth{false, 4};
};

// --- Multi-depth mode ---

TEST_F(MarkerScanTest, Llama3TwoMessages_MultiDepth) {
    // Simulate: [BOS, marker, sys_tokens..., eot, marker, user_tokens..., eot, marker, gen_prompt...]
    //           pos:  0    1     2  3  4  5    6     7      8  9  10 11    12    13      14
    // marker (100) at positions 1, 7, 13
    // BOS = 1 at position 0
    std::vector<int32_t> tokens = {
        1, 100, 10, 11, 12, 13, 50,  // BOS + msg0 (system) + eot
        100, 20, 21, 22, 23, 50,     // msg1 (user) + eot
        100, 30, 31                   // gen prompt
    };
    // markers at indices: 1, 7, 13

    auto info = make_text_info(2, 1);  // 2 messages, 1 system

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // Boundaries: marker[1]-marker[0]=7-1=6, marker[2]-marker[0]=13-1=12
    ASSERT_EQ(result.prefix_boundaries.size(), 2u);
    EXPECT_EQ(result.prefix_boundaries[0], 6u);
    EXPECT_EQ(result.prefix_boundaries[1], 12u);
    // System boundary: marker[sys_count=1] - bos_offset = 7-1 = 6
    EXPECT_EQ(result.prefix_boundary, 6u);
}

TEST_F(MarkerScanTest, ChatmlThreeMessages_MultiDepth) {
    // Simulate chatml: no BOS, marker at start of each message + gen prompt
    // marker (200) at positions 0, 5, 10, 14
    std::vector<int32_t> tokens = {
        200, 10, 11, 12, 13,     // msg0 (system): im_start + role + content + im_end + \n
        200, 20, 21, 22, 23,     // msg1 (user)
        200, 30, 31, 32,         // msg2 (assistant)
        200, 40                   // gen prompt
    };
    // markers at indices: 0, 5, 10, 14

    auto info = make_text_info(3, 1);  // 3 messages, 1 system

    auto result = detect_boundaries_by_marker_scan(tokens, 200, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // Boundaries: 5-0=5, 10-0=10, 14-0=14
    ASSERT_EQ(result.prefix_boundaries.size(), 3u);
    EXPECT_EQ(result.prefix_boundaries[0], 5u);
    EXPECT_EQ(result.prefix_boundaries[1], 10u);
    EXPECT_EQ(result.prefix_boundaries[2], 14u);
    // System boundary: marker[1]-marker[0] = 5-0 = 5
    EXPECT_EQ(result.prefix_boundary, 5u);
}

TEST_F(MarkerScanTest, MultipleSystemMessages_MultiDepth) {
    // 2 system messages, 1 user message, gen prompt
    std::vector<int32_t> tokens = {
        100, 10, 11,     // msg0 (system)
        100, 20, 21,     // msg1 (system)
        100, 30, 31,     // msg2 (user)
        100, 40          // gen prompt
    };
    // markers at indices: 0, 3, 6, 9

    auto info = make_text_info(3, 2);  // 3 messages, 2 system

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // System boundary: marker[2]-marker[0] = 6-0 = 6
    EXPECT_EQ(result.prefix_boundary, 6u);
    ASSERT_EQ(result.prefix_boundaries.size(), 3u);
    EXPECT_EQ(result.prefix_boundaries[0], 3u);
    EXPECT_EQ(result.prefix_boundaries[1], 6u);
    EXPECT_EQ(result.prefix_boundaries[2], 9u);
}

TEST_F(MarkerScanTest, NoSystemMessages_MultiDepth) {
    // All user messages, no system prefix
    std::vector<int32_t> tokens = {
        100, 10, 11,     // msg0 (user)
        100, 20, 21,     // msg1 (user)
        100, 30          // gen prompt
    };

    auto info = make_text_info(2, 0);  // 2 messages, 0 system

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // system_boundary = 0 → falls back to first prefix_boundary
    EXPECT_EQ(result.prefix_boundary, 3u);
    ASSERT_EQ(result.prefix_boundaries.size(), 2u);
}

TEST_F(MarkerScanTest, AllSystemMessages_MultiDepth) {
    // Edge case: every message is system
    std::vector<int32_t> tokens = {
        100, 10, 11,     // msg0 (system)
        100, 20, 21,     // msg1 (system)
        100, 30          // gen prompt
    };

    auto info = make_text_info(2, 2);  // 2 messages, 2 system

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // sys_count=2, marker[2]-marker[0] = 6-0 = 6
    EXPECT_EQ(result.prefix_boundary, 6u);
}

// --- Single-depth mode ---

TEST_F(MarkerScanTest, SingleDepth_SystemBoundary) {
    std::vector<int32_t> tokens = {
        1, 100, 10, 11, 12, 13, 50,   // BOS + msg0 (system)
        100, 20, 21, 22, 23, 50,       // msg1 (user)
        100, 30, 31                     // gen prompt
    };

    auto info = make_text_info(2, 1);
    info.has_system_messages = true;

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, single_depth);

    ASSERT_TRUE(result.detected);
    EXPECT_EQ(result.prefix_boundary, 6u);  // marker[1]-marker[0] = 7-1 = 6
    EXPECT_TRUE(result.prefix_boundaries.empty());  // No multi-depth boundaries
}

TEST_F(MarkerScanTest, SingleDepth_BelowMinTokens) {
    // System prefix is only 2 tokens — below min_prefix_boundary_tokens=4
    std::vector<int32_t> tokens = {
        100, 10,         // msg0 (system): only 2 tokens incl marker
        100, 20, 21,     // msg1 (user)
        100, 30          // gen prompt
    };

    auto info = make_text_info(2, 1);
    info.has_system_messages = true;

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, single_depth);

    // boundary = marker[1]-marker[0] = 2-0 = 2, which is < min_prefix_boundary_tokens=4
    EXPECT_FALSE(result.detected);
}

TEST_F(MarkerScanTest, SingleDepth_NoSystemMessages) {
    std::vector<int32_t> tokens = {
        100, 10, 11,
        100, 20, 21,
        100, 30
    };

    auto info = make_text_info(2, 0);  // No system messages

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, single_depth);

    EXPECT_FALSE(result.detected);
}

// --- Validation / error cases ---

TEST_F(MarkerScanTest, MarkerCountMismatch_Injection) {
    // Extra marker in content (simulates special-token injection)
    std::vector<int32_t> tokens = {
        100, 10, 100, 11,    // msg0 with injected marker
        100, 20, 21,         // msg1
        100, 30              // gen prompt
    };
    // 4 markers found, but expected 2+1=3

    auto info = make_text_info(2, 1);

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    EXPECT_FALSE(result.detected);  // Validation fails
}

TEST_F(MarkerScanTest, MarkerCountMismatch_TooFew) {
    // Missing a marker (template error)
    std::vector<int32_t> tokens = {
        100, 10, 11,     // msg0
        20, 21,          // msg1 missing marker
        100, 30          // gen prompt
    };
    // 2 markers found, but expected 2+1=3

    auto info = make_text_info(2, 1);

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    EXPECT_FALSE(result.detected);
}

TEST_F(MarkerScanTest, EmptyTokens) {
    std::vector<int32_t> tokens;
    auto info = make_text_info(2, 1);

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    EXPECT_FALSE(result.detected);
}

TEST_F(MarkerScanTest, ZeroMessages) {
    std::vector<int32_t> tokens = {1, 2, 3};
    auto info = make_text_info(0, 0);

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    EXPECT_FALSE(result.detected);
}

TEST_F(MarkerScanTest, SingleMessageWithGenPrompt) {
    // Just 1 message + gen prompt
    std::vector<int32_t> tokens = {
        100, 10, 11, 12,    // msg0 (user)
        100, 20              // gen prompt
    };

    auto info = make_text_info(1, 0);

    auto result = detect_boundaries_by_marker_scan(tokens, 100, info, multi_depth);

    ASSERT_TRUE(result.detected);
    ASSERT_EQ(result.prefix_boundaries.size(), 1u);
    EXPECT_EQ(result.prefix_boundaries[0], 4u);  // marker[1]-marker[0]=4-0=4
}

// =============================================================================
// Proportional Estimation Tests
// =============================================================================

class ProportionalEstimateTest : public ::testing::Test {
protected:
    BoundaryDetectionConfig multi_depth{true, 4};
    BoundaryDetectionConfig single_depth{false, 4};
};

// --- Multi-depth mode ---

TEST_F(ProportionalEstimateTest, BasicMultiDepth) {
    // 3 messages: system (100 chars), user (200 chars), assistant (100 chars)
    // Total text: 400 chars, 200 tokens
    auto info = make_text_info(3, 1, {100, 300, 400});
    info.text = std::string(400, 'x');  // 400 chars total

    auto result = detect_boundaries_by_char_ratio(200, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // boundary[0] = 100 * 200/400 = 50
    // boundary[1] = 300 * 200/400 = 150
    // boundary[2] = 400 * 200/400 = 200 → NOT < 200, so filtered out
    ASSERT_EQ(result.prefix_boundaries.size(), 2u);
    EXPECT_EQ(result.prefix_boundaries[0], 50u);
    EXPECT_EQ(result.prefix_boundaries[1], 150u);
    // System boundary = char_ends[0] * ratio = 100 * 200/400 = 50
    EXPECT_EQ(result.prefix_boundary, 50u);
}

TEST_F(ProportionalEstimateTest, MultipleSystemMessages_MultiDepth) {
    // 2 system (50 chars each), 1 user (200 chars) = 300 total, 150 tokens
    auto info = make_text_info(3, 2, {50, 100, 300});
    info.text = std::string(300, 'x');

    auto result = detect_boundaries_by_char_ratio(150, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // System boundary = char_ends[sys_count-1] * ratio = char_ends[1] * 150/300 = 100*0.5 = 50
    EXPECT_EQ(result.prefix_boundary, 50u);
}

TEST_F(ProportionalEstimateTest, NoSystemMessages_MultiDepth) {
    auto info = make_text_info(2, 0, {100, 200});
    info.text = std::string(300, 'x');  // 300 chars includes gen prompt

    auto result = detect_boundaries_by_char_ratio(150, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // system_boundary = 0 → falls back to first prefix_boundary
    EXPECT_EQ(result.prefix_boundary, result.prefix_boundaries.front());
}

TEST_F(ProportionalEstimateTest, SmallFirstMessage_BoundaryIsZero) {
    // First message is tiny: 1 char out of 1000, ratio very low
    auto info = make_text_info(2, 0, {1, 500});
    info.text = std::string(1000, 'x');

    auto result = detect_boundaries_by_char_ratio(100, info, multi_depth);

    // boundary[0] = 1 * 100/1000 = 0 → filtered out (must be > 0)
    // boundary[1] = 500 * 100/1000 = 50
    ASSERT_TRUE(result.detected);
    ASSERT_EQ(result.prefix_boundaries.size(), 1u);
    EXPECT_EQ(result.prefix_boundaries[0], 50u);
}

// --- Single-depth mode ---

TEST_F(ProportionalEstimateTest, SingleDepth_SystemBoundary) {
    auto info = make_text_info(2, 1, {100, 400});
    info.text = std::string(400, 'x');
    info.has_system_prefix = true;
    info.system_prefix_end = 100;

    auto result = detect_boundaries_by_char_ratio(200, info, single_depth);

    ASSERT_TRUE(result.detected);
    // boundary = system_prefix_end * ratio = 100 * 200/400 = 50
    EXPECT_EQ(result.prefix_boundary, 50u);
    EXPECT_TRUE(result.prefix_boundaries.empty());
}

TEST_F(ProportionalEstimateTest, SingleDepth_BelowMinTokens) {
    // System prefix is small → boundary < min_prefix_boundary_tokens
    auto info = make_text_info(2, 1, {5, 500});
    info.text = std::string(500, 'x');
    info.has_system_prefix = true;
    info.system_prefix_end = 5;

    auto result = detect_boundaries_by_char_ratio(100, info, single_depth);

    // boundary = 5 * 100/500 = 1 → below min_prefix_boundary_tokens=4
    EXPECT_FALSE(result.detected);
}

TEST_F(ProportionalEstimateTest, SingleDepth_NoSystemPrefix) {
    auto info = make_text_info(2, 0, {100, 200});
    info.text = std::string(200, 'x');
    // has_system_messages=false, has_system_prefix=false

    auto result = detect_boundaries_by_char_ratio(100, info, single_depth);

    EXPECT_FALSE(result.detected);
}

// --- Error / edge cases ---

TEST_F(ProportionalEstimateTest, EmptyCharEnds) {
    auto info = make_text_info(0, 0);
    info.text = "some text";

    auto result = detect_boundaries_by_char_ratio(100, info, multi_depth);

    EXPECT_FALSE(result.detected);
}

TEST_F(ProportionalEstimateTest, EmptyText) {
    auto info = make_text_info(1, 0, {10});
    // info.text is empty by default

    auto result = detect_boundaries_by_char_ratio(100, info, multi_depth);

    EXPECT_FALSE(result.detected);
}

TEST_F(ProportionalEstimateTest, ZeroTokens) {
    auto info = make_text_info(1, 0, {10});
    info.text = "some text";

    auto result = detect_boundaries_by_char_ratio(0, info, multi_depth);

    EXPECT_FALSE(result.detected);
}

TEST_F(ProportionalEstimateTest, ProportionalAccuracy) {
    // Verify the proportional calculation with known values.
    // 1000-char text, 250 tokens → ratio 0.25
    auto info = make_text_info(4, 1, {200, 500, 800, 1000});
    info.text = std::string(1000, 'x');

    auto result = detect_boundaries_by_char_ratio(250, info, multi_depth);

    ASSERT_TRUE(result.detected);
    // boundary[0] = 200 * 250/1000 = 50
    // boundary[1] = 500 * 250/1000 = 125
    // boundary[2] = 800 * 250/1000 = 200
    // boundary[3] = 1000 * 250/1000 = 250 → filtered (not < 250)
    ASSERT_EQ(result.prefix_boundaries.size(), 3u);
    EXPECT_EQ(result.prefix_boundaries[0], 50u);
    EXPECT_EQ(result.prefix_boundaries[1], 125u);
    EXPECT_EQ(result.prefix_boundaries[2], 200u);
    // System boundary = char_ends[0] * ratio = 200 * 250/1000 = 50
    EXPECT_EQ(result.prefix_boundary, 50u);
}
