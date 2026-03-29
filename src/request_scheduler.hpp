// Ranvier Core - Request Scheduler
//
// Priority-aware request queueing with per-agent fair scheduling.
// Shard-local (one instance per core, no cross-shard state).
//
// Templatized on the context type so unit tests can supply a lightweight
// stub without pulling in Seastar dependencies (same pattern as
// BasicCircuitBreaker<Clock>).

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>

namespace ranvier {

// Priority level for request classification (used by priority-aware scheduling)
// Lower numeric value = higher priority
enum class PriorityLevel : uint8_t {
    CRITICAL = 0,   // Interactive typing, must not block
    HIGH     = 1,   // Primary agent task
    NORMAL   = 2,   // Background processing (default)
    LOW      = 3,   // Batch jobs, can wait
};

// Convert PriorityLevel to string label for metrics and logging
inline const char* priority_level_to_string(PriorityLevel level) {
    switch (level) {
        case PriorityLevel::CRITICAL: return "critical";
        case PriorityLevel::HIGH:     return "high";
        case PriorityLevel::NORMAL:   return "normal";
        case PriorityLevel::LOW:      return "low";
    }
    return "normal";  // unreachable, but satisfies -Wreturn-type
}

// Minimal settings consumed by the scheduler.  The full BackpressureSettings
// (in http_controller.hpp) is a superset; this struct keeps the scheduler
// header free of unrelated fields.
struct SchedulerSettings {
    std::array<uint32_t, 4> tier_capacity = {64, 128, 256, 512};
    uint32_t max_per_agent_queued = 128;  // Per-agent queue depth limit (VISION 3.3)
};

// Type alias for pause check callback (VISION 3.3).
// Returns true if the given agent_id is currently paused.
// The scheduler itself doesn't own the AgentRegistry — it receives this predicate.
using PauseCheckFn = std::function<bool(std::string_view agent_id)>;

// =============================================================================
// BasicRequestScheduler<Context>
// =============================================================================
//
// Context must expose:
//   PriorityLevel                       priority;
//   std::string                         user_agent;
//   std::string                         agent_id;       (VISION 3.3)
//   std::chrono::steady_clock::time_point enqueue_time;  (written by enqueue)

template<typename Context>
class BasicRequestScheduler {
public:
    explicit BasicRequestScheduler(const SchedulerSettings& settings)
        : _max_per_agent_queued(settings.max_per_agent_queued > 0
              ? settings.max_per_agent_queued : DEFAULT_MAX_PER_AGENT_QUEUED) {
        for (size_t i = 0; i < 4; ++i) {
            _tier_capacity[i] = settings.tier_capacity[i] > 0
                ? settings.tier_capacity[i] : DEFAULT_TIER_CAPACITY;
        }
    }

    // Default-construct with default capacities (useful for tests)
    BasicRequestScheduler() : BasicRequestScheduler(SchedulerSettings{}) {}

    // Set the pause check function (called during dequeue to skip paused agents).
    // VISION 3.3: The scheduler doesn't own the AgentRegistry — it receives a predicate.
    void set_pause_check(PauseCheckFn fn) { _pause_check = std::move(fn); }

    // Returns true if enqueued, false if queue full or per-agent limit reached (caller returns 503).
    // Caller must signal the condition variable after a successful enqueue.
    bool enqueue(std::unique_ptr<Context> ctx) {
        auto tier = static_cast<uint8_t>(ctx->priority);
        if (tier >= 4) tier = 2;  // Fallback to NORMAL

        if (_queues[tier].size() >= _tier_capacity[tier]) {
            ++_overflow_drops[tier];
            return false;
        }

        // VISION 3.3: Per-agent queue depth limit.
        // Prevents a single paused agent from consuming an entire tier's capacity.
        if (!ctx->agent_id.empty()) {
            size_t agent_queued = count_agent_queued(ctx->agent_id);
            if (agent_queued >= _max_per_agent_queued) {
                ++_per_agent_drops;
                return false;
            }
        }

        ctx->enqueue_time = std::chrono::steady_clock::now();
        _queues[tier].push_back(std::move(ctx));
        ++_total_enqueued;
        return true;
    }

    // Dequeue highest-priority waiting request (fair within tier).
    // CRITICAL always wins (never subject to pause — interactive typing must not block).
    // Within each non-CRITICAL tier, picks the agent whose last_served timestamp
    // is oldest, but SKIPS any request whose agent_id is paused (VISION 3.3).
    std::optional<std::unique_ptr<Context>> dequeue() {
        // CRITICAL always wins — no pause check
        if (!_queues[0].empty()) {
            auto ctx = std::move(_queues[0].front());
            _queues[0].pop_front();
            update_agent_served(ctx.get());
            ++_total_dequeued;
            return ctx;
        }

        // Iterate HIGH → NORMAL → LOW
        for (size_t tier = 1; tier < 4; ++tier) {
            if (_queues[tier].empty()) continue;

            // Find the best non-paused candidate (oldest last_served)
            std::optional<size_t> best_idx;
            auto best_time = std::chrono::steady_clock::time_point::max();

            for (size_t i = 0; i < _queues[tier].size(); ++i) {
                // VISION 3.3: Skip paused agents
                if (_pause_check && !_queues[tier][i]->agent_id.empty()
                    && _pause_check(_queues[tier][i]->agent_id)) {
                    ++_paused_skips;
                    continue;
                }

                auto agent_key = get_agent_key(_queues[tier][i].get());
                auto it = _agent_last_served.find(agent_key);
                auto served_time = (it != _agent_last_served.end())
                    ? it->second : std::chrono::steady_clock::time_point::min();
                if (served_time < best_time) {
                    best_time = served_time;
                    best_idx = i;
                }
            }

            if (!best_idx) continue;  // All paused in this tier

            // Swap the winner to front and pop
            if (*best_idx != 0) {
                std::swap(_queues[tier][0], _queues[tier][*best_idx]);
            }
            auto ctx = std::move(_queues[tier].front());
            _queues[tier].pop_front();
            update_agent_served(ctx.get());
            ++_total_dequeued;
            return ctx;
        }

        return std::nullopt;
    }

    // Per-tier queue depth for metrics
    std::array<size_t, 4> queue_depths() const {
        return {_queues[0].size(), _queues[1].size(),
                _queues[2].size(), _queues[3].size()};
    }

    // Single-tier depth (avoids temporary array in per-tier metric lambdas)
    size_t queue_depth(size_t tier) const {
        return tier < 4 ? _queues[tier].size() : 0;
    }

    // Total enqueued across all tiers
    size_t total_queued() const {
        return _queues[0].size() + _queues[1].size() +
               _queues[2].size() + _queues[3].size();
    }

    // Lifetime counters
    uint64_t total_enqueued_count() const { return _total_enqueued; }
    uint64_t total_dequeued_count() const { return _total_dequeued; }
    const std::array<uint64_t, 4>& overflow_drops() const { return _overflow_drops; }
    size_t agents_tracked() const { return _agent_last_served.size(); }

    // VISION 3.3: Per-agent drop counter and paused-skip counter
    uint64_t per_agent_drops() const { return _per_agent_drops; }
    uint64_t paused_skips() const { return _paused_skips; }

    // VISION 3.3: Per-agent queue depth map (across all tiers).
    // O(total_queued) — called infrequently (admin API only).
    absl::flat_hash_map<std::string, size_t> queue_depths_by_agent() const {
        absl::flat_hash_map<std::string, size_t> result;
        for (size_t tier = 0; tier < 4; ++tier) {
            for (const auto& ctx : _queues[tier]) {
                if (!ctx->agent_id.empty()) {
                    ++result[ctx->agent_id];
                }
            }
        }
        return result;
    }

private:
    static constexpr uint32_t DEFAULT_TIER_CAPACITY = 512;
    static constexpr uint32_t DEFAULT_MAX_PER_AGENT_QUEUED = 128;
    static constexpr size_t MAX_AGENT_TRACKING = 256;

    // One bounded deque per priority tier (Rule #4: explicit MAX_SIZE + overflow counter)
    std::array<std::deque<std::unique_ptr<Context>>, 4> _queues;
    std::array<uint32_t, 4> _tier_capacity{};
    std::array<uint64_t, 4> _overflow_drops{};

    // VISION 3.3: Per-agent queue depth limit and pause check
    uint32_t _max_per_agent_queued{DEFAULT_MAX_PER_AGENT_QUEUED};
    uint64_t _per_agent_drops{0};
    uint64_t _paused_skips{0};
    PauseCheckFn _pause_check;

    // Lifetime counters
    uint64_t _total_enqueued{0};
    uint64_t _total_dequeued{0};

    // Fair scheduling: tracks last-served time per agent string
    // MAX_SIZE = 256 entries, LRU eviction (Hard Rule #4)
    absl::flat_hash_map<std::string, std::chrono::steady_clock::time_point> _agent_last_served;

    static constexpr std::string_view UNKNOWN_AGENT = "unknown";

    // Extract agent key from context (User-Agent or fallback).
    // Returns a view into ctx->user_agent — valid while the context is alive.
    static std::string_view get_agent_key(const Context* ctx) {
        if (!ctx->user_agent.empty()) {
            return ctx->user_agent;
        }
        return UNKNOWN_AGENT;
    }

    // Update last_served timestamp for the agent, with LRU eviction.
    // Evicts before insert to keep size strictly <= MAX_AGENT_TRACKING.
    void update_agent_served(const Context* ctx) {
        auto key = get_agent_key(ctx);
        auto now = std::chrono::steady_clock::now();

        // Check if this agent is already tracked (update in place, no eviction needed)
        auto existing = _agent_last_served.find(key);
        if (existing != _agent_last_served.end()) {
            existing->second = now;
            return;
        }

        // New agent: evict oldest if at capacity
        if (_agent_last_served.size() >= MAX_AGENT_TRACKING) {
            auto oldest_it = _agent_last_served.begin();
            for (auto it = _agent_last_served.begin(); it != _agent_last_served.end(); ++it) {
                if (it->second < oldest_it->second) {
                    oldest_it = it;
                }
            }
            _agent_last_served.erase(oldest_it);
        }

        _agent_last_served.emplace(std::string(key), now);
    }

    // VISION 3.3: Count how many requests from a given agent_id are queued across all tiers.
    // O(total_queued) but called only on the enqueue path when agent_id is non-empty.
    size_t count_agent_queued(std::string_view aid) const {
        size_t count = 0;
        for (size_t tier = 0; tier < 4; ++tier) {
            for (const auto& ctx : _queues[tier]) {
                if (ctx->agent_id == aid) {
                    ++count;
                }
            }
        }
        return count;
    }
};

}  // namespace ranvier
