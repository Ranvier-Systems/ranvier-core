// Ranvier Core - Agent Registry Unit Tests
//
// Tests for AgentRegistry: initialization from config, agent identification
// cascade (X-Ranvier-Agent → User-Agent match → auto-detect), pause/resume
// control, metrics recording, MAX_KNOWN_AGENTS bound enforcement, and
// normalize_agent_id behavior.
//
// Requires Seastar headers for seastar::http::request but does NOT need a
// running reactor — all operations are pure computation.

#include "agent_registry.hpp"
#include <gtest/gtest.h>

using namespace ranvier;

// =============================================================================
// Test Helpers
// =============================================================================

// Build a seastar::http::request with specified headers
static seastar::http::request make_request(
        const std::string& user_agent = "",
        const std::string& x_ranvier_agent = "") {
    seastar::http::request req;
    if (!user_agent.empty()) {
        req._headers["User-Agent"] = user_agent;
    }
    if (!x_ranvier_agent.empty()) {
        req._headers["X-Ranvier-Agent"] = x_ranvier_agent;
    }
    return req;
}

// Default config with the 4 standard agents
static AgentRegistryConfig default_config() {
    AgentRegistryConfig cfg;
    cfg.enabled = true;
    cfg.auto_detect_agents = true;
    return cfg;
}

// Config with auto-detect disabled
static AgentRegistryConfig no_auto_detect_config() {
    AgentRegistryConfig cfg;
    cfg.enabled = true;
    cfg.auto_detect_agents = false;
    return cfg;
}

// Config with no known agents
static AgentRegistryConfig empty_config() {
    AgentRegistryConfig cfg;
    cfg.enabled = true;
    cfg.auto_detect_agents = false;
    cfg.known_agents.clear();
    return cfg;
}

// =============================================================================
// Construction and Initialization
// =============================================================================

TEST(AgentRegistryTest, DefaultConfigInitializesFourAgents) {
    AgentRegistry reg(default_config());
    EXPECT_EQ(reg.agent_count(), 4u);
    EXPECT_EQ(reg.overflow_drops(), 0u);
}

TEST(AgentRegistryTest, EmptyConfigInitializesNoAgents) {
    AgentRegistry reg(empty_config());
    EXPECT_EQ(reg.agent_count(), 0u);
}

TEST(AgentRegistryTest, ListAgentsReturnsAllConfigured) {
    AgentRegistry reg(default_config());
    auto agents = reg.list_agents();
    EXPECT_EQ(agents.size(), 4u);

    // Verify expected agent IDs exist
    std::set<std::string> ids;
    for (const auto& a : agents) {
        ids.insert(a.agent_id);
    }
    EXPECT_TRUE(ids.count("cursor-ide"));
    EXPECT_TRUE(ids.count("claude-code"));
    EXPECT_TRUE(ids.count("cline"));
    EXPECT_TRUE(ids.count("aider"));
}

TEST(AgentRegistryTest, ConfiguredAgentPriorities) {
    AgentRegistry reg(default_config());

    auto cursor = reg.get_agent("cursor-ide");
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(cursor->default_priority, PriorityLevel::CRITICAL);
    EXPECT_EQ(cursor->display_name, "Cursor IDE");
    EXPECT_EQ(cursor->pattern, "Cursor");
    EXPECT_TRUE(cursor->allow_pause);

    auto claude = reg.get_agent("claude-code");
    ASSERT_TRUE(claude.has_value());
    EXPECT_EQ(claude->default_priority, PriorityLevel::CRITICAL);

    auto cline = reg.get_agent("cline");
    ASSERT_TRUE(cline.has_value());
    EXPECT_EQ(cline->default_priority, PriorityLevel::HIGH);

    auto aider = reg.get_agent("aider");
    ASSERT_TRUE(aider.has_value());
    EXPECT_EQ(aider->default_priority, PriorityLevel::HIGH);
}

TEST(AgentRegistryTest, GetUnknownAgentReturnsNullopt) {
    AgentRegistry reg(default_config());
    EXPECT_FALSE(reg.get_agent("nonexistent").has_value());
}

// =============================================================================
// normalize_agent_id (tested via configured agent IDs)
// =============================================================================

TEST(AgentRegistryTest, NormalizeAgentIdLowercaseAndDashes) {
    // "Cursor IDE" → "cursor-ide"
    AgentRegistryConfig cfg;
    cfg.known_agents = {{"pat", "My Cool Agent", 2, true}};
    AgentRegistry reg(cfg);
    EXPECT_TRUE(reg.get_agent("my-cool-agent").has_value());
}

TEST(AgentRegistryTest, NormalizeAgentIdAlreadyLowercase) {
    AgentRegistryConfig cfg;
    cfg.known_agents = {{"pat", "simple", 2, true}};
    AgentRegistry reg(cfg);
    EXPECT_TRUE(reg.get_agent("simple").has_value());
}

// =============================================================================
// Agent Identification — User-Agent Substring Match
// =============================================================================

TEST(AgentRegistryTest, IdentifyCursorFromUserAgent) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("Cursor/0.45.5");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "cursor-ide");
}

TEST(AgentRegistryTest, IdentifyClaudeCodeFromUserAgent) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("claude-code/1.2.3");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "claude-code");
}

TEST(AgentRegistryTest, IdentifyClineFromUserAgent) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("Mozilla/5.0 cline/3.1");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "cline");
}

TEST(AgentRegistryTest, IdentifyAiderFromUserAgent) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("aider/0.50.0");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "aider");
}

TEST(AgentRegistryTest, NoMatchReturnsNulloptWhenAutoDetectOff) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("SomeUnknownClient/1.0");
    auto id = reg.identify_agent(req);
    EXPECT_FALSE(id.has_value());
}

TEST(AgentRegistryTest, EmptyUserAgentReturnsNullopt) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request();
    auto id = reg.identify_agent(req);
    EXPECT_FALSE(id.has_value());
}

TEST(AgentRegistryTest, SubstringMatchNotCaseSensitive) {
    // Pattern "Cursor" should match "Cursor/..." but not "cursor/..."
    // (current implementation is case-sensitive substring match, same as extract_priority)
    AgentRegistry reg(no_auto_detect_config());
    auto req_upper = make_request("Cursor/1.0");
    EXPECT_TRUE(reg.identify_agent(req_upper).has_value());

    auto req_lower = make_request("cursor/1.0");
    // "Cursor" pattern won't match "cursor" (case-sensitive)
    EXPECT_FALSE(reg.identify_agent(req_lower).has_value());
}

TEST(AgentRegistryTest, FirstPatternMatchWins) {
    // If multiple patterns could match, first in iteration order wins.
    // With absl::flat_hash_map, iteration order is unspecified, but
    // we can test that at least one match is returned.
    AgentRegistryConfig cfg;
    cfg.auto_detect_agents = false;
    cfg.known_agents = {
        {"test-agent", "Agent A", 0, true},
        {"test-agent-extended", "Agent B", 1, true},
    };
    AgentRegistry reg(cfg);
    auto req = make_request("I am test-agent-extended/1.0");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    // Both patterns match, one should win
    EXPECT_TRUE(*id == "agent-a" || *id == "agent-b");
}

// =============================================================================
// Agent Identification — X-Ranvier-Agent Header
// =============================================================================

TEST(AgentRegistryTest, CustomHeaderTakesPrecedence) {
    AgentRegistry reg(no_auto_detect_config());
    // User-Agent matches Cursor, but X-Ranvier-Agent says "cline"
    auto req = make_request("Cursor/1.0", "cline");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "cline");
}

TEST(AgentRegistryTest, CustomHeaderNormalized) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("", "Cursor IDE");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "cursor-ide");
}

TEST(AgentRegistryTest, CustomHeaderUnknownAgentAutoDetectOn) {
    AgentRegistry reg(default_config());
    auto req = make_request("", "MyCustomAgent");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "mycustomagent");
    // Should be auto-registered
    EXPECT_EQ(reg.agent_count(), 5u);
}

TEST(AgentRegistryTest, CustomHeaderUnknownAgentAutoDetectOff) {
    AgentRegistry reg(no_auto_detect_config());
    auto req = make_request("", "MyCustomAgent");
    auto id = reg.identify_agent(req);
    // Returns nullopt — unknown agent with auto-detect off should not
    // produce a phantom ID with no backing AgentInfo.
    EXPECT_FALSE(id.has_value());
    // Not auto-registered
    EXPECT_EQ(reg.agent_count(), 4u);
}

// =============================================================================
// Agent Identification — Auto-Detect
// =============================================================================

TEST(AgentRegistryTest, AutoDetectExtractsFirstTokenBeforeSlash) {
    AgentRegistry reg(default_config());
    auto req = make_request("FancyEditor/2.0 (Linux)");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "fancyeditor");
    EXPECT_EQ(reg.agent_count(), 5u);  // 4 configured + 1 auto-detected

    // Verify the auto-registered agent info
    auto info = reg.get_agent("fancyeditor");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->display_name, "FancyEditor");
    EXPECT_EQ(info->default_priority, PriorityLevel::NORMAL);
    EXPECT_TRUE(info->allow_pause);
}

TEST(AgentRegistryTest, AutoDetectSecondRequestSameAgent) {
    AgentRegistry reg(default_config());
    auto req1 = make_request("NewTool/1.0");
    auto req2 = make_request("NewTool/2.0");
    auto id1 = reg.identify_agent(req1);
    auto id2 = reg.identify_agent(req2);
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    EXPECT_EQ(*id1, *id2);
    EXPECT_EQ(reg.agent_count(), 5u);  // Still 5, not 6
}

TEST(AgentRegistryTest, AutoDetectNoSlashUsesFullUA) {
    AgentRegistry reg(default_config());
    auto req = make_request("SimpleClient");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "simpleclient");
}

// =============================================================================
// MAX_KNOWN_AGENTS Bound (Rule #4)
// =============================================================================

TEST(AgentRegistryTest, MaxKnownAgentsBound) {
    AgentRegistryConfig cfg;
    cfg.auto_detect_agents = true;
    cfg.known_agents.clear();

    // Fill to MAX_KNOWN_AGENTS with configured agents
    for (size_t i = 0; i < AgentRegistryConfig::MAX_KNOWN_AGENTS; ++i) {
        cfg.known_agents.push_back({
            "pattern" + std::to_string(i),
            "Agent " + std::to_string(i),
            2,
            true
        });
    }

    AgentRegistry reg(cfg);
    EXPECT_EQ(reg.agent_count(), AgentRegistryConfig::MAX_KNOWN_AGENTS);
    EXPECT_EQ(reg.overflow_drops(), 0u);

    // Try to auto-detect one more — should be rejected
    auto req = make_request("OverflowAgent/1.0");
    auto id = reg.identify_agent(req);
    // No match (none of the configured patterns match "OverflowAgent")
    // and auto-register should fail due to bound
    EXPECT_FALSE(id.has_value());
    EXPECT_EQ(reg.overflow_drops(), 1u);
    EXPECT_EQ(reg.agent_count(), AgentRegistryConfig::MAX_KNOWN_AGENTS);
}

TEST(AgentRegistryTest, ConfigTruncatesExcessAgents) {
    AgentRegistryConfig cfg;
    cfg.auto_detect_agents = false;
    cfg.known_agents.clear();

    // Add more than MAX_KNOWN_AGENTS
    for (size_t i = 0; i < AgentRegistryConfig::MAX_KNOWN_AGENTS + 10; ++i) {
        cfg.known_agents.push_back({
            "pat" + std::to_string(i),
            "Agent" + std::to_string(i),
            2,
            true
        });
    }

    AgentRegistry reg(cfg);
    // Should be truncated to MAX_KNOWN_AGENTS
    EXPECT_EQ(reg.agent_count(), AgentRegistryConfig::MAX_KNOWN_AGENTS);
}

// =============================================================================
// Pause / Resume
// =============================================================================

TEST(AgentRegistryTest, PauseAgent) {
    AgentRegistry reg(default_config());
    EXPECT_FALSE(reg.is_paused("cursor-ide"));

    EXPECT_TRUE(reg.pause_agent("cursor-ide"));
    EXPECT_TRUE(reg.is_paused("cursor-ide"));
}

TEST(AgentRegistryTest, ResumeAgent) {
    AgentRegistry reg(default_config());
    reg.pause_agent("cursor-ide");
    EXPECT_TRUE(reg.is_paused("cursor-ide"));

    EXPECT_TRUE(reg.resume_agent("cursor-ide"));
    EXPECT_FALSE(reg.is_paused("cursor-ide"));
}

TEST(AgentRegistryTest, PauseUnknownAgentReturnsFalse) {
    AgentRegistry reg(default_config());
    EXPECT_FALSE(reg.pause_agent("nonexistent"));
}

TEST(AgentRegistryTest, ResumeUnknownAgentReturnsFalse) {
    AgentRegistry reg(default_config());
    EXPECT_FALSE(reg.resume_agent("nonexistent"));
}

TEST(AgentRegistryTest, PauseAlreadyPausedReturnsFalse) {
    AgentRegistry reg(default_config());
    EXPECT_TRUE(reg.pause_agent("cursor-ide"));
    EXPECT_FALSE(reg.pause_agent("cursor-ide"));  // Already paused
}

TEST(AgentRegistryTest, ResumeNotPausedReturnsFalse) {
    AgentRegistry reg(default_config());
    EXPECT_FALSE(reg.resume_agent("cursor-ide"));  // Not paused
}

TEST(AgentRegistryTest, PauseDisallowedAgent) {
    AgentRegistryConfig cfg;
    cfg.known_agents = {{"pat", "NoPause Agent", 2, false}};
    AgentRegistry reg(cfg);

    EXPECT_FALSE(reg.pause_agent("nopause-agent"));
    EXPECT_FALSE(reg.is_paused("nopause-agent"));
}

TEST(AgentRegistryTest, IsPausedUnknownAgentReturnsFalse) {
    AgentRegistry reg(default_config());
    EXPECT_FALSE(reg.is_paused("nonexistent"));
}

TEST(AgentRegistryTest, PausedAgentVisibleInList) {
    AgentRegistry reg(default_config());
    reg.pause_agent("cursor-ide");

    auto agents = reg.list_agents();
    bool found = false;
    for (const auto& a : agents) {
        if (a.agent_id == "cursor-ide") {
            EXPECT_TRUE(a.paused);
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(AgentRegistryTest, PausedAgentVisibleInGetAgent) {
    AgentRegistry reg(default_config());
    reg.pause_agent("cline");

    auto info = reg.get_agent("cline");
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->paused);
}

// =============================================================================
// Metrics Recording
// =============================================================================

TEST(AgentRegistryTest, RecordRequestIncrementsCounter) {
    AgentRegistry reg(default_config());
    auto before = reg.get_agent("cursor-ide");
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->requests_total, 0u);

    reg.record_request("cursor-ide");
    reg.record_request("cursor-ide");
    reg.record_request("cursor-ide");

    auto after = reg.get_agent("cursor-ide");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->requests_total, 3u);
}

TEST(AgentRegistryTest, RecordRequestSetsFirstAndLastSeen) {
    AgentRegistry reg(default_config());

    auto before = reg.get_agent("cursor-ide");
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->first_seen.time_since_epoch().count(), 0);

    reg.record_request("cursor-ide");

    auto after = reg.get_agent("cursor-ide");
    ASSERT_TRUE(after.has_value());
    EXPECT_NE(after->first_seen.time_since_epoch().count(), 0);
    EXPECT_NE(after->last_seen.time_since_epoch().count(), 0);
    EXPECT_LE(after->first_seen, after->last_seen);
}

TEST(AgentRegistryTest, RecordRequestFirstSeenOnlySetOnce) {
    AgentRegistry reg(default_config());
    reg.record_request("cursor-ide");

    auto first = reg.get_agent("cursor-ide");
    auto first_seen = first->first_seen;

    reg.record_request("cursor-ide");

    auto second = reg.get_agent("cursor-ide");
    EXPECT_EQ(second->first_seen, first_seen);  // Unchanged
    EXPECT_GE(second->last_seen, first->last_seen);  // Updated
}

TEST(AgentRegistryTest, RecordRequestUnknownAgentNoOp) {
    AgentRegistry reg(default_config());
    // Should not crash or throw
    reg.record_request("nonexistent");
    EXPECT_EQ(reg.agent_count(), 4u);
}

TEST(AgentRegistryTest, RecordPausedRejection) {
    AgentRegistry reg(default_config());
    reg.pause_agent("cursor-ide");

    reg.record_paused_rejection("cursor-ide");
    reg.record_paused_rejection("cursor-ide");

    auto info = reg.get_agent("cursor-ide");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->requests_paused_rejected, 2u);
}

TEST(AgentRegistryTest, RecordPausedRejectionUpdatesLastSeen) {
    AgentRegistry reg(default_config());
    reg.pause_agent("cursor-ide");

    reg.record_paused_rejection("cursor-ide");
    auto info = reg.get_agent("cursor-ide");
    ASSERT_TRUE(info.has_value());
    EXPECT_NE(info->last_seen.time_since_epoch().count(), 0);
}

TEST(AgentRegistryTest, RecordPausedRejectionUnknownAgentNoOp) {
    AgentRegistry reg(default_config());
    // Should not crash
    reg.record_paused_rejection("nonexistent");
}

// =============================================================================
// Identification + Pause Integration
// =============================================================================

TEST(AgentRegistryTest, PausedAgentStillIdentified) {
    AgentRegistry reg(no_auto_detect_config());
    reg.pause_agent("cursor-ide");

    auto req = make_request("Cursor/1.0");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "cursor-ide");

    // Caller is responsible for checking is_paused and rejecting
    EXPECT_TRUE(reg.is_paused(*id));
}

// =============================================================================
// Custom Agent Config
// =============================================================================

TEST(AgentRegistryTest, CustomSingleAgent) {
    AgentRegistryConfig cfg;
    cfg.auto_detect_agents = false;
    cfg.known_agents = {
        {"vscode-copilot", "VS Code Copilot", 1, false},
    };

    AgentRegistry reg(cfg);
    EXPECT_EQ(reg.agent_count(), 1u);

    auto info = reg.get_agent("vs-code-copilot");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->display_name, "VS Code Copilot");
    EXPECT_EQ(info->default_priority, PriorityLevel::HIGH);
    EXPECT_FALSE(info->allow_pause);

    auto req = make_request("Mozilla/5.0 vscode-copilot/1.0");
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "vs-code-copilot");
}

TEST(AgentRegistryTest, PriorityClampedToValidRange) {
    AgentRegistryConfig cfg;
    cfg.known_agents = {{"pat", "Agent", 99, true}};  // 99 > 3, should clamp
    AgentRegistry reg(cfg);

    auto info = reg.get_agent("agent");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->default_priority, PriorityLevel::LOW);  // Clamped to 3
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(AgentRegistryTest, EmptyUserAgentHeaderPresent) {
    AgentRegistry reg(default_config());
    seastar::http::request req;
    req._headers["User-Agent"] = "";
    auto id = reg.identify_agent(req);
    EXPECT_FALSE(id.has_value());
}

TEST(AgentRegistryTest, EmptyXRanvierAgentHeaderIgnored) {
    AgentRegistry reg(no_auto_detect_config());
    seastar::http::request req;
    req._headers["X-Ranvier-Agent"] = "";
    req._headers["User-Agent"] = "Cursor/1.0";
    auto id = reg.identify_agent(req);
    ASSERT_TRUE(id.has_value());
    // Empty X-Ranvier-Agent is ignored, falls through to User-Agent match
    EXPECT_EQ(*id, "cursor-ide");
}

TEST(AgentRegistryTest, OverflowDropsCounter) {
    AgentRegistryConfig cfg;
    cfg.auto_detect_agents = true;
    cfg.known_agents.clear();
    // Fill to max
    for (size_t i = 0; i < AgentRegistryConfig::MAX_KNOWN_AGENTS; ++i) {
        cfg.known_agents.push_back({"p" + std::to_string(i), "A" + std::to_string(i), 2, true});
    }
    AgentRegistry reg(cfg);
    EXPECT_EQ(reg.overflow_drops(), 0u);

    // Three auto-detect attempts that fail
    for (int i = 0; i < 3; ++i) {
        auto req = make_request("Overflow" + std::to_string(i) + "/1.0");
        reg.identify_agent(req);
    }
    EXPECT_EQ(reg.overflow_drops(), 3u);
}

TEST(AgentRegistryTest, MultipleAgentsIndependentCounters) {
    AgentRegistry reg(default_config());
    reg.record_request("cursor-ide");
    reg.record_request("cursor-ide");
    reg.record_request("cline");

    auto cursor = reg.get_agent("cursor-ide");
    auto cline = reg.get_agent("cline");
    ASSERT_TRUE(cursor.has_value());
    ASSERT_TRUE(cline.has_value());
    EXPECT_EQ(cursor->requests_total, 2u);
    EXPECT_EQ(cline->requests_total, 1u);
}
