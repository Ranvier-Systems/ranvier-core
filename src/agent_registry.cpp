// Ranvier Core - Agent Registry Implementation
//
// Shard-local agent identification from User-Agent and custom headers.
// See agent_registry.hpp for class documentation and Hard Rules compliance.

#include "agent_registry.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cctype>

namespace ranvier {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AgentRegistry::AgentRegistry(const AgentRegistryConfig& config)
    : _config(config) {
    init_from_config();
}

void AgentRegistry::init_from_config() {
    for (const auto& ac : _config.known_agents) {
        if (_agents.size() >= AgentRegistryConfig::MAX_KNOWN_AGENTS) {
            log_control.warn("AgentRegistry: known_agents exceeds MAX_KNOWN_AGENTS ({}), "
                             "truncating (Rule #4)", AgentRegistryConfig::MAX_KNOWN_AGENTS);
            break;
        }
        AgentInfo info;
        info.agent_id = normalize_agent_id(ac.name);
        info.display_name = ac.name;
        info.pattern = ac.pattern;
        info.default_priority = static_cast<PriorityLevel>(
            std::min<uint8_t>(ac.default_priority, static_cast<uint8_t>(PriorityLevel::LOW)));
        info.allow_pause = ac.allow_pause;
        // first_seen / last_seen remain default-initialized (epoch) until first request
        auto [it, inserted] = _agents.emplace(info.agent_id, std::move(info));
        if (!inserted) {
            log_control.warn("AgentRegistry: duplicate agent_id '{}' from config entry '{}', "
                             "keeping first occurrence", it->first, ac.name);
        }
    }
    log_control.info("AgentRegistry initialized with {} configured agents on shard {}",
                     _agents.size(), seastar::this_shard_id());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string AgentRegistry::normalize_agent_id(const std::string& name) {
    std::string id;
    id.reserve(name.size());
    for (char c : name) {
        if (c == ' ') {
            id.push_back('-');
        } else {
            id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return id;
}

std::string AgentRegistry::auto_register(const std::string& user_agent) {
    // Extract first token before "/" as agent name
    static constexpr size_t MAX_AUTO_DETECT_NAME_LENGTH = 32;
    std::string_view ua(user_agent);
    auto slash_pos = ua.find('/');
    std::string raw_name;
    if (slash_pos != std::string_view::npos && slash_pos > 0) {
        raw_name = std::string(ua.substr(0, slash_pos));
    } else {
        // Use entire UA if no slash (truncate to prevent unbounded agent names)
        raw_name = std::string(ua.substr(0, std::min<size_t>(ua.size(), MAX_AUTO_DETECT_NAME_LENGTH)));
    }

    std::string agent_id = normalize_agent_id(raw_name);
    if (agent_id.empty()) {
        return {};
    }

    // Check if already registered (may have been auto-registered by a previous request)
    if (_agents.contains(agent_id)) {
        return agent_id;
    }

    // Enforce MAX_KNOWN_AGENTS bound (Rule #4)
    if (_agents.size() >= AgentRegistryConfig::MAX_KNOWN_AGENTS) {
        ++_overflow_drops;
        log_control.warn("AgentRegistry: MAX_KNOWN_AGENTS ({}) reached, dropping auto-detect "
                         "for '{}' (overflow_drops={})",
                         AgentRegistryConfig::MAX_KNOWN_AGENTS, raw_name, _overflow_drops);
        return {};
    }

    AgentInfo info;
    info.agent_id = agent_id;
    info.display_name = raw_name;
    info.pattern = raw_name;  // Use the extracted name as the pattern
    info.default_priority = PriorityLevel::NORMAL;
    info.allow_pause = true;

    log_control.info("AgentRegistry: auto-detected new agent '{}' (id={}) on shard {}",
                     raw_name, agent_id, seastar::this_shard_id());

    _agents.emplace(agent_id, std::move(info));
    return agent_id;
}

// ---------------------------------------------------------------------------
// Agent Identification
// ---------------------------------------------------------------------------

std::optional<std::string> AgentRegistry::identify_agent(
        const seastar::http::request& req) {

    // 1. Custom header — X-Ranvier-Agent allows agents to self-identify
    auto custom_it = req._headers.find("X-Ranvier-Agent");
    if (custom_it != req._headers.end() && !custom_it->second.empty()) {
        std::string agent_id = normalize_agent_id(custom_it->second);
        if (_agents.contains(agent_id)) {
            return agent_id;
        }
        // If custom header value doesn't match a known agent and auto-detect is on,
        // auto-register it.
        if (_config.auto_detect_agents) {
            auto id = auto_register(custom_it->second);
            if (!id.empty()) return id;
        }
        // Unknown agent with auto-detect off or overflow — return nullopt
        // so the caller doesn't get a phantom ID with no backing AgentInfo.
        return std::nullopt;
    }

    // 2. User-Agent substring match against configured patterns
    auto ua_it = req._headers.find("User-Agent");
    if (ua_it == req._headers.end() || ua_it->second.empty()) {
        return std::nullopt;
    }
    const std::string& user_agent = ua_it->second;

    for (const auto& [id, info] : _agents) {
        if (!info.pattern.empty() && user_agent.find(info.pattern) != std::string::npos) {
            return id;
        }
    }

    // 3. Auto-detect: extract agent name from User-Agent first token
    if (_config.auto_detect_agents) {
        auto id = auto_register(user_agent);
        if (!id.empty()) return id;
    }

    // 4. No match
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<AgentInfo> AgentRegistry::get_agent(const std::string& agent_id) const {
    auto it = _agents.find(agent_id);
    if (it == _agents.end()) return std::nullopt;
    return it->second;
}

bool AgentRegistry::is_paused(const std::string& agent_id) const {
    auto it = _agents.find(agent_id);
    if (it == _agents.end()) return false;
    return it->second.paused;
}

std::vector<AgentInfo> AgentRegistry::list_agents() const {
    std::vector<AgentInfo> result;
    result.reserve(_agents.size());
    for (const auto& [id, info] : _agents) {
        result.push_back(info);
    }
    return result;
}

size_t AgentRegistry::agent_count() const {
    return _agents.size();
}

uint64_t AgentRegistry::overflow_drops() const {
    return _overflow_drops;
}

// ---------------------------------------------------------------------------
// Pause / Resume
// ---------------------------------------------------------------------------

bool AgentRegistry::pause_agent(const std::string& agent_id) {
    auto it = _agents.find(agent_id);
    if (it == _agents.end()) return false;
    if (!it->second.allow_pause) return false;
    if (it->second.paused) return false;  // Already paused
    it->second.paused = true;
    log_control.info("AgentRegistry: paused agent '{}' on shard {}",
                     agent_id, seastar::this_shard_id());
    return true;
}

bool AgentRegistry::resume_agent(const std::string& agent_id) {
    auto it = _agents.find(agent_id);
    if (it == _agents.end()) return false;
    if (!it->second.paused) return false;  // Not paused
    it->second.paused = false;
    log_control.info("AgentRegistry: resumed agent '{}' on shard {}",
                     agent_id, seastar::this_shard_id());
    return true;
}

// ---------------------------------------------------------------------------
// Metrics Recording
// ---------------------------------------------------------------------------

void AgentRegistry::record_request(const std::string& agent_id) {
    auto it = _agents.find(agent_id);
    if (it == _agents.end()) return;
    auto now = std::chrono::steady_clock::now();
    auto& info = it->second;
    ++info.requests_total;
    if (info.first_seen.time_since_epoch().count() == 0) {
        info.first_seen = now;
    }
    info.last_seen = now;
}

void AgentRegistry::record_paused_rejection(const std::string& agent_id) {
    auto it = _agents.find(agent_id);
    if (it == _agents.end()) return;
    ++it->second.requests_paused_rejected;
    it->second.last_seen = std::chrono::steady_clock::now();
}

}  // namespace ranvier
