// Ranvier Core - Agent Registry (VISION 3.2)
//
// Shard-local agent identification, metrics, and pause/resume control.
// One instance per HttpController (not sharded separately).
//
// Identifies requesting agents (Cursor, Claude Code, Cline, Aider, etc.)
// from User-Agent headers and custom headers, assigns per-agent priorities,
// and exposes pause/resume control via the admin API.
//
// Hard Rules:
//   #0: No std::mutex or std::shared_ptr — Seastar shared-nothing
//   #1: No atomics — single-threaded per shard
//   #4: _agents map bounded by MAX_KNOWN_AGENTS with overflow counter
//   #5: No background fibers (pure in-memory data structure)
//   #6: Agent metrics deregistered in HttpController::stop()

#pragma once

#include "config_schema.hpp"
#include "request_scheduler.hpp"  // PriorityLevel

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <seastar/http/request.hh>

namespace ranvier {

// Runtime state for a known or auto-detected agent
struct AgentInfo {
    std::string agent_id;                              // Normalized key (lowercase, spaces → dashes)
    std::string display_name;                          // Human-readable name
    std::string pattern;                               // Matched User-Agent substring
    PriorityLevel default_priority = PriorityLevel::NORMAL;
    bool allow_pause = true;
    bool paused = false;

    // Per-agent metrics (shard-local counters, no atomics — Rule #1)
    uint64_t requests_total = 0;
    uint64_t requests_paused_rejected = 0;             // Requests rejected while paused
    std::chrono::steady_clock::time_point first_seen;
    std::chrono::steady_clock::time_point last_seen;
};

// Shard-local agent registry.
// Pure in-memory data structure — no I/O, no fibers, no timers.
class AgentRegistry {
public:
    explicit AgentRegistry(const AgentRegistryConfig& config);

    // Identify agent from request headers.
    // Cascade: X-Ranvier-Agent header → User-Agent substring match → auto-detect.
    // Returns agent_id string if matched, nullopt if no match.
    // Not const: auto-detect path mutates _agents for internal bookkeeping.
    std::optional<std::string> identify_agent(
        const seastar::http::request& req);

    // Get full agent info by ID (returns copy for safe iteration)
    std::optional<AgentInfo> get_agent(const std::string& agent_id) const;

    // Pause an agent — returns false if agent not found or allow_pause is false
    bool pause_agent(const std::string& agent_id);

    // Resume an agent — returns false if agent not found or not paused
    bool resume_agent(const std::string& agent_id);

    // Check if an agent is currently paused
    bool is_paused(const std::string& agent_id) const;

    // Record a successful request for metrics tracking
    void record_request(const std::string& agent_id);

    // Record a paused rejection for metrics tracking
    void record_paused_rejection(const std::string& agent_id);

    // List all known agents (configured + auto-detected)
    std::vector<AgentInfo> list_agents() const;

    // Get total number of tracked agents
    size_t agent_count() const;

    // Get overflow drop count (agents rejected due to MAX_KNOWN_AGENTS bound)
    uint64_t overflow_drops() const;

private:
    AgentRegistryConfig _config;

    // Known agents keyed by agent_id
    // Hard Rule #4: bounded by MAX_KNOWN_AGENTS
    absl::flat_hash_map<std::string, AgentInfo> _agents;
    uint64_t _overflow_drops = 0;

    // Populate _agents from config's known_agents list
    void init_from_config();

    // Generate agent_id from display name (lowercase, replace spaces with dashes)
    static std::string normalize_agent_id(const std::string& name);

    // Auto-register a previously unseen agent (from User-Agent header).
    // Returns agent_id. Respects MAX_KNOWN_AGENTS bound.
    // Note: mutable because it modifies _agents — called from identify_agent
    // which is logically const for the caller but mutates internal tracking state.
    std::string auto_register(const std::string& user_agent);
};

}  // namespace ranvier
