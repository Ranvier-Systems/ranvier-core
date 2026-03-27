// Ranvier Core - Priority Tier Unit Tests
//
// Tests for priority level enum, string conversion, parsing, cascade logic,
// and PriorityTierConfig/BackpressureSettings structures.
// Uses standalone formula tests (no HttpController needed), matching the
// CostEstimationFormulaTest pattern in config_test.cpp.

#include "http_controller.hpp"
#include "config_schema.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace ranvier;

// =============================================================================
// PriorityLevel Enum Tests
// =============================================================================

TEST(PriorityLevelTest, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(PriorityLevel::CRITICAL), 0u);
    EXPECT_EQ(static_cast<uint8_t>(PriorityLevel::HIGH), 1u);
    EXPECT_EQ(static_cast<uint8_t>(PriorityLevel::NORMAL), 2u);
    EXPECT_EQ(static_cast<uint8_t>(PriorityLevel::LOW), 3u);
}

TEST(PriorityLevelTest, FourTotalLevels) {
    // Ensure all levels fit in a 4-element array (used by metrics)
    std::array<uint64_t, 4> counters = {0, 0, 0, 0};
    counters[static_cast<uint8_t>(PriorityLevel::CRITICAL)]++;
    counters[static_cast<uint8_t>(PriorityLevel::HIGH)]++;
    counters[static_cast<uint8_t>(PriorityLevel::NORMAL)]++;
    counters[static_cast<uint8_t>(PriorityLevel::LOW)]++;
    for (auto c : counters) {
        EXPECT_EQ(c, 1u);
    }
}

TEST(PriorityLevelTest, CriticalIsHighestPriority) {
    // Lower numeric value = higher priority
    EXPECT_LT(static_cast<uint8_t>(PriorityLevel::CRITICAL),
              static_cast<uint8_t>(PriorityLevel::HIGH));
    EXPECT_LT(static_cast<uint8_t>(PriorityLevel::HIGH),
              static_cast<uint8_t>(PriorityLevel::NORMAL));
    EXPECT_LT(static_cast<uint8_t>(PriorityLevel::NORMAL),
              static_cast<uint8_t>(PriorityLevel::LOW));
}

// =============================================================================
// priority_level_to_string Tests
// =============================================================================

TEST(PriorityLevelToStringTest, AllLevels) {
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::CRITICAL), "critical");
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::HIGH), "high");
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::NORMAL), "normal");
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::LOW), "low");
}

TEST(PriorityLevelToStringTest, RoundTrip) {
    // Each level maps to a distinct string
    std::string critical = priority_level_to_string(PriorityLevel::CRITICAL);
    std::string high = priority_level_to_string(PriorityLevel::HIGH);
    std::string normal = priority_level_to_string(PriorityLevel::NORMAL);
    std::string low = priority_level_to_string(PriorityLevel::LOW);
    EXPECT_NE(critical, high);
    EXPECT_NE(high, normal);
    EXPECT_NE(normal, low);
}

// =============================================================================
// Priority Parsing Logic Tests (replicate static helpers for unit testing)
// =============================================================================

// Replicate the parse_priority_string logic from http_controller.cpp
// for standalone unit testing (the original is file-static)
static std::optional<PriorityLevel> test_parse_priority_string(std::string_view s) {
    if (s.size() < 3 || s.size() > 8) return std::nullopt;
    char buf[9];
    for (size_t i = 0; i < s.size(); ++i) {
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    std::string_view lower(buf, s.size());
    if (lower == "critical") return PriorityLevel::CRITICAL;
    if (lower == "high")     return PriorityLevel::HIGH;
    if (lower == "normal")   return PriorityLevel::NORMAL;
    if (lower == "low")      return PriorityLevel::LOW;
    return std::nullopt;
}

TEST(PriorityParsingTest, ValidLowercase) {
    EXPECT_EQ(test_parse_priority_string("critical"), PriorityLevel::CRITICAL);
    EXPECT_EQ(test_parse_priority_string("high"), PriorityLevel::HIGH);
    EXPECT_EQ(test_parse_priority_string("normal"), PriorityLevel::NORMAL);
    EXPECT_EQ(test_parse_priority_string("low"), PriorityLevel::LOW);
}

TEST(PriorityParsingTest, CaseInsensitive) {
    EXPECT_EQ(test_parse_priority_string("CRITICAL"), PriorityLevel::CRITICAL);
    EXPECT_EQ(test_parse_priority_string("High"), PriorityLevel::HIGH);
    EXPECT_EQ(test_parse_priority_string("NORMAL"), PriorityLevel::NORMAL);
    EXPECT_EQ(test_parse_priority_string("LOW"), PriorityLevel::LOW);
    EXPECT_EQ(test_parse_priority_string("CrItIcAl"), PriorityLevel::CRITICAL);
}

TEST(PriorityParsingTest, InvalidValues) {
    EXPECT_EQ(test_parse_priority_string(""), std::nullopt);
    EXPECT_EQ(test_parse_priority_string("hi"), std::nullopt);       // too short
    EXPECT_EQ(test_parse_priority_string("urgent"), std::nullopt);
    EXPECT_EQ(test_parse_priority_string("medium"), std::nullopt);
    EXPECT_EQ(test_parse_priority_string("veryhighpriority"), std::nullopt);  // too long
    EXPECT_EQ(test_parse_priority_string("123"), std::nullopt);
}

// =============================================================================
// Priority Cascade Logic Tests (formula-style, no HttpController)
// =============================================================================

// Replicate the cascade logic for testability:
// 1. Header value (if valid)
// 2. User-Agent match (with stream promotion)
// 3. Cost-based threshold

struct MockPriorityConfig {
    bool respect_header = true;
    std::string default_priority = "normal";
    double cost_threshold_high = 100.0;
    double cost_threshold_low = 10.0;
    std::vector<PriorityTierUserAgentEntry> known_user_agents = {
        {"Cursor",      0},   // CRITICAL
        {"claude-code", 0},   // CRITICAL
        {"cline",       1},   // HIGH
        {"aider",       1},   // HIGH
    };
};

static PriorityLevel default_from_string(const std::string& s) {
    auto p = test_parse_priority_string(s);
    return p.value_or(PriorityLevel::NORMAL);
}

static PriorityLevel test_extract_priority(
        const MockPriorityConfig& cfg,
        const std::string& header_value,    // empty = not present
        const std::string& user_agent,      // empty = not present
        double estimated_cost_units,
        std::string_view body_view) {

    // Step 1: Explicit header
    if (cfg.respect_header && !header_value.empty()) {
        auto parsed = test_parse_priority_string(header_value);
        if (parsed.has_value()) {
            return *parsed;
        }
        // Invalid header: fall through (would log warning in production)
    }

    // Step 2: User-Agent match
    if (!user_agent.empty()) {
        for (const auto& entry : cfg.known_user_agents) {
            if (user_agent.find(entry.pattern) != std::string::npos) {
                auto matched_priority = static_cast<PriorityLevel>(entry.priority);
                // Stream promotion
                if (matched_priority != PriorityLevel::CRITICAL) {
                    if (body_view.find("\"stream\":true") != std::string_view::npos ||
                        body_view.find("\"stream\": true") != std::string_view::npos) {
                        return PriorityLevel::CRITICAL;
                    }
                }
                return matched_priority;
            }
        }
    }

    // Step 3: Cost-based default
    if (estimated_cost_units > cfg.cost_threshold_high) {
        return PriorityLevel::HIGH;
    }
    if (estimated_cost_units < cfg.cost_threshold_low) {
        return PriorityLevel::LOW;
    }

    return default_from_string(cfg.default_priority);
}

class PriorityCascadeTest : public ::testing::Test {
protected:
    MockPriorityConfig cfg;
};

// --- Step 1: Header tests ---

TEST_F(PriorityCascadeTest, ExplicitHeaderCritical) {
    auto p = test_extract_priority(cfg, "critical", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, ExplicitHeaderHigh) {
    auto p = test_extract_priority(cfg, "HIGH", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, ExplicitHeaderLow) {
    auto p = test_extract_priority(cfg, "Low", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::LOW);
}

TEST_F(PriorityCascadeTest, ExplicitHeaderNormal) {
    auto p = test_extract_priority(cfg, "NORMAL", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::NORMAL);
}

TEST_F(PriorityCascadeTest, InvalidHeaderFallsThrough) {
    // Invalid header value → falls to cost-based (50 is between thresholds → NORMAL)
    auto p = test_extract_priority(cfg, "bogus", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::NORMAL);
}

TEST_F(PriorityCascadeTest, HeaderTakesPrecedenceOverUserAgent) {
    // Header says LOW, user-agent would match Cursor (CRITICAL) → header wins
    auto p = test_extract_priority(cfg, "low", "Cursor/1.0", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::LOW);
}

TEST_F(PriorityCascadeTest, HeaderDisabledFallsToUserAgent) {
    cfg.respect_header = false;
    auto p = test_extract_priority(cfg, "low", "Cursor/1.0", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

// --- Step 2: User-Agent match tests ---

TEST_F(PriorityCascadeTest, UserAgentCursorIsCritical) {
    auto p = test_extract_priority(cfg, "", "Cursor/1.0 (Linux)", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, UserAgentClaudeCodeIsCritical) {
    auto p = test_extract_priority(cfg, "", "claude-code/1.2.3", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, UserAgentClineIsHigh) {
    auto p = test_extract_priority(cfg, "", "cline/0.5", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, UserAgentAiderIsHigh) {
    auto p = test_extract_priority(cfg, "", "aider/2.0", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, UserAgentFirstMatchWins) {
    // "cline-aider" matches "cline" first → HIGH (not aider)
    auto p = test_extract_priority(cfg, "", "cline-aider", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, UserAgentSubstringMatch) {
    // "Mozilla/5.0 (compatible; Cursor)" should match "Cursor" substring
    auto p = test_extract_priority(cfg, "", "Mozilla/5.0 (compatible; Cursor)", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, UserAgentNoMatchFallsToCost) {
    auto p = test_extract_priority(cfg, "", "SomeOtherAgent/1.0", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::NORMAL);
}

// --- Step 2b: Stream promotion tests ---

TEST_F(PriorityCascadeTest, StreamPromotionClineToCompact) {
    // cline is HIGH, but "stream":true in body → promote to CRITICAL
    auto p = test_extract_priority(cfg, "", "cline/0.5", 50.0,
                                   R"({"model":"llama","stream":true})");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, StreamPromotionAiderToCritical) {
    auto p = test_extract_priority(cfg, "", "aider/2.0", 50.0,
                                   R"({"stream": true})");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, StreamPromotionWithSpace) {
    // "stream": true (with space after colon)
    auto p = test_extract_priority(cfg, "", "cline/0.5", 50.0,
                                   R"({"stream": true, "model": "x"})");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, NoStreamPromotionWhenAlreadyCritical) {
    // Cursor is already CRITICAL — stream doesn't change anything
    auto p = test_extract_priority(cfg, "", "Cursor/1.0", 50.0,
                                   R"({"stream":true})");
    EXPECT_EQ(p, PriorityLevel::CRITICAL);
}

TEST_F(PriorityCascadeTest, NoStreamPromotionWhenFalse) {
    // "stream":false should NOT promote
    auto p = test_extract_priority(cfg, "", "cline/0.5", 50.0,
                                   R"({"stream":false})");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, NoStreamPromotionForUnknownAgent) {
    // Unknown user-agent with stream:true should not get promotion
    // (promotion only applies to matched agents)
    auto p = test_extract_priority(cfg, "", "unknown/1.0", 50.0,
                                   R"({"stream":true})");
    EXPECT_EQ(p, PriorityLevel::NORMAL);  // Falls to cost-based
}

// --- Step 3: Cost-based default tests ---

TEST_F(PriorityCascadeTest, HighCostGetsHigh) {
    auto p = test_extract_priority(cfg, "", "", 150.0, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, LowCostGetsLow) {
    auto p = test_extract_priority(cfg, "", "", 5.0, "{}");
    EXPECT_EQ(p, PriorityLevel::LOW);
}

TEST_F(PriorityCascadeTest, MidCostGetsDefault) {
    auto p = test_extract_priority(cfg, "", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::NORMAL);
}

TEST_F(PriorityCascadeTest, CostAtHighThresholdGetsDefault) {
    // Exactly at threshold (not above) → falls to default
    auto p = test_extract_priority(cfg, "", "", 100.0, "{}");
    EXPECT_EQ(p, PriorityLevel::NORMAL);
}

TEST_F(PriorityCascadeTest, CostAtLowThresholdGetsDefault) {
    // Exactly at threshold (not below) → falls to default
    auto p = test_extract_priority(cfg, "", "", 10.0, "{}");
    EXPECT_EQ(p, PriorityLevel::NORMAL);
}

TEST_F(PriorityCascadeTest, CostJustAboveHighThreshold) {
    auto p = test_extract_priority(cfg, "", "", 100.1, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, CostJustBelowLowThreshold) {
    auto p = test_extract_priority(cfg, "", "", 9.9, "{}");
    EXPECT_EQ(p, PriorityLevel::LOW);
}

TEST_F(PriorityCascadeTest, CustomDefaultPriority) {
    cfg.default_priority = "high";
    auto p = test_extract_priority(cfg, "", "", 50.0, "{}");
    EXPECT_EQ(p, PriorityLevel::HIGH);
}

TEST_F(PriorityCascadeTest, ZeroCostGetsLow) {
    auto p = test_extract_priority(cfg, "", "", 0.0, "{}");
    EXPECT_EQ(p, PriorityLevel::LOW);
}

// =============================================================================
// BackpressureSettings Extension Tests
// =============================================================================

TEST(BackpressureSettingsTest, PriorityQueueDefaultDisabled) {
    BackpressureSettings bp;
    EXPECT_FALSE(bp.enable_priority_queue);
}

TEST(BackpressureSettingsTest, TierCapacityDefaultZero) {
    BackpressureSettings bp;
    for (auto cap : bp.tier_capacity) {
        EXPECT_EQ(cap, 0u);
    }
}

TEST(BackpressureSettingsTest, TierCapacityHasFourSlots) {
    BackpressureSettings bp;
    EXPECT_EQ(bp.tier_capacity.size(), 4u);
}

// =============================================================================
// PriorityTierUserAgentEntry Tests
// =============================================================================

TEST(PriorityTierUserAgentEntryTest, DefaultPriorityIsNormal) {
    PriorityTierUserAgentEntry entry;
    EXPECT_EQ(entry.priority, 2u);  // NORMAL
}

TEST(PriorityTierUserAgentEntryTest, PatternDefaultEmpty) {
    PriorityTierUserAgentEntry entry;
    EXPECT_TRUE(entry.pattern.empty());
}

// =============================================================================
// ProxyContext Priority Field Tests
// =============================================================================

TEST(ProxyContextTest, DefaultPriorityIsNormal) {
    ProxyContext ctx;
    EXPECT_EQ(ctx.priority, PriorityLevel::NORMAL);
}

TEST(ProxyContextTest, PriorityCanBeSet) {
    ProxyContext ctx;
    ctx.priority = PriorityLevel::CRITICAL;
    EXPECT_EQ(ctx.priority, PriorityLevel::CRITICAL);
    ctx.priority = PriorityLevel::LOW;
    EXPECT_EQ(ctx.priority, PriorityLevel::LOW);
}
