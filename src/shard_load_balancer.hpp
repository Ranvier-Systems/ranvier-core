// Ranvier Core - Shard-Aware Load Balancer
//
// Distributes incoming HTTP requests across Seastar shards based on real-time
// CPU load using the Power of Two Choices (P2C) algorithm.
//
// P2C Algorithm:
//   1. Randomly select 2 candidate shards
//   2. Query load metrics from both candidates
//   3. Route to the shard with lower load score
//
// This approach provides near-optimal load distribution with O(1) overhead,
// avoiding the O(n) cost of querying all shards for every request.
//
// References:
// - "The Power of Two Choices in Randomized Load Balancing" (Mitzenmacher, 2001)
// - "Join-Idle-Queue: A novel load balancing algorithm for dynamically scalable
//   web services" (Lu et al., 2011)

#pragma once

#include "shard_load_metrics.hpp"

#include <atomic>
#include <random>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/metrics_registration.hh>

namespace ranvier {

// Configuration for the shard load balancer
struct ShardLoadBalancerConfig {
    // Enable/disable cross-shard load balancing
    // When disabled, requests are processed on the receiving shard
    bool enabled = true;

    // Minimum load difference (as ratio) to trigger cross-shard dispatch
    // E.g., 0.2 means only dispatch if target shard has 20% lower load
    // Prevents unnecessary cross-shard communication for marginal gains
    double min_load_difference = 0.2;

    // Maximum concurrent requests per shard before triggering load balancing
    // If local shard is below this threshold, skip P2C and process locally
    uint64_t local_processing_threshold = 10;

    // Interval for refreshing shard load snapshots (in microseconds)
    // Cached snapshots reduce cross-shard communication overhead
    uint64_t snapshot_refresh_interval_us = 1000;  // 1ms default

    // Enable adaptive mode that learns from request latency patterns
    bool adaptive_mode = false;
};

// Shard Load Balancer - implements P2C algorithm for shard selection
// Each shard has its own instance with thread-local RNG and cached metrics
class ShardLoadBalancer {
public:
    explicit ShardLoadBalancer(ShardLoadBalancerConfig config = {})
        : _config(config)
        , _shard_count(seastar::smp::count)
        , _rng(std::random_device{}() + seastar::this_shard_id())
        , _local_dispatches(0)
        , _cross_shard_dispatches(0)
        , _p2c_selections(0) {
        // Pre-size snapshot cache for all shards
        _snapshot_cache.resize(_shard_count);
    }

    // Required by seastar::sharded<>
    seastar::future<> stop() {
        return seastar::make_ready_future<>();
    }

    // Select the best shard for processing a request using P2C
    // Returns the shard ID to dispatch to, or local shard if load balancing disabled
    uint32_t select_shard() {
        if (!_config.enabled || _shard_count <= 1) {
            return seastar::this_shard_id();
        }

        // Fast path: if local shard is lightly loaded, process locally
        if (shard_load_metrics_initialized()) {
            auto local_active = shard_load_metrics().active_requests();
            if (local_active < _config.local_processing_threshold) {
                ++_local_dispatches;
                return seastar::this_shard_id();
            }
        }

        // P2C: Select two random shards and pick the less loaded one
        return select_shard_p2c();
    }

    // Synchronous P2C selection using cached snapshots
    // This is the fast path used in the request hot path
    uint32_t select_shard_p2c() {
        uint32_t local_shard = seastar::this_shard_id();

        // Generate two random shard candidates
        std::uniform_int_distribution<uint32_t> dist(0, _shard_count - 1);
        uint32_t candidate1 = dist(_rng);
        uint32_t candidate2 = dist(_rng);

        // Ensure candidates are different (if possible)
        if (candidate1 == candidate2 && _shard_count > 1) {
            candidate2 = (candidate2 + 1) % _shard_count;
        }

        // Get cached snapshots for both candidates
        auto& snap1 = _snapshot_cache[candidate1];
        auto& snap2 = _snapshot_cache[candidate2];

        // Calculate load scores
        double score1 = snap1.load_score();
        double score2 = snap2.load_score();

        // Pick the lower-loaded shard
        uint32_t selected = (score1 <= score2) ? candidate1 : candidate2;

        // Check if cross-shard dispatch is worthwhile
        if (selected != local_shard) {
            auto& local_snap = _snapshot_cache[local_shard];
            double local_score = local_snap.load_score();
            double selected_score = (selected == candidate1) ? score1 : score2;

            // Only dispatch cross-shard if improvement exceeds threshold
            if (local_score > 0 &&
                (local_score - selected_score) / local_score < _config.min_load_difference) {
                ++_local_dispatches;
                return local_shard;
            }

            ++_cross_shard_dispatches;
        } else {
            ++_local_dispatches;
        }

        ++_p2c_selections;
        return selected;
    }

    // Async P2C selection with fresh metric fetching
    // Used when cached snapshots may be stale
    seastar::future<uint32_t> select_shard_async() {
        if (!_config.enabled || _shard_count <= 1) {
            return seastar::make_ready_future<uint32_t>(seastar::this_shard_id());
        }

        // Generate two random candidates
        std::uniform_int_distribution<uint32_t> dist(0, _shard_count - 1);
        uint32_t candidate1 = dist(_rng);
        uint32_t candidate2 = dist(_rng);
        if (candidate1 == candidate2 && _shard_count > 1) {
            candidate2 = (candidate2 + 1) % _shard_count;
        }

        // Fetch fresh snapshots from both candidates
        return seastar::when_all_succeed(
            fetch_shard_snapshot(candidate1),
            fetch_shard_snapshot(candidate2)
        ).then([this, candidate1, candidate2](ShardLoadSnapshot snap1, ShardLoadSnapshot snap2) {
            // Update cache
            _snapshot_cache[candidate1] = snap1;
            _snapshot_cache[candidate2] = snap2;

            // Pick the lower-loaded shard
            uint32_t selected = (snap1.load_score() <= snap2.load_score()) ? candidate1 : candidate2;

            // Check if cross-shard dispatch is worthwhile
            uint32_t local_shard = seastar::this_shard_id();
            if (selected != local_shard) {
                ++_cross_shard_dispatches;
            } else {
                ++_local_dispatches;
            }
            ++_p2c_selections;

            return selected;
        });
    }

    // Update configuration (for hot-reload)
    void update_config(const ShardLoadBalancerConfig& config) {
        _config = config;
    }

    // Refresh snapshot cache for all shards
    // Call periodically from a background timer
    seastar::future<> refresh_all_snapshots() {
        std::vector<seastar::future<ShardLoadSnapshot>> futures;
        futures.reserve(_shard_count);

        for (uint32_t shard = 0; shard < _shard_count; ++shard) {
            futures.push_back(fetch_shard_snapshot(shard));
        }

        return seastar::when_all_succeed(futures.begin(), futures.end())
            .then([this](std::vector<ShardLoadSnapshot> snapshots) {
                for (size_t i = 0; i < snapshots.size(); ++i) {
                    _snapshot_cache[i] = snapshots[i];
                }
            });
    }

    // Update local shard's snapshot in cache (called locally, no cross-shard)
    void update_local_snapshot() {
        if (shard_load_metrics_initialized()) {
            _snapshot_cache[seastar::this_shard_id()] = shard_load_metrics().snapshot();
        }
    }

    // Get metrics for monitoring
    uint64_t local_dispatches() const { return _local_dispatches; }
    uint64_t cross_shard_dispatches() const { return _cross_shard_dispatches; }
    uint64_t p2c_selections() const { return _p2c_selections; }
    bool is_enabled() const { return _config.enabled; }
    uint32_t shard_count() const { return _shard_count; }

    // Get cached snapshot for a specific shard (for debugging/metrics)
    const ShardLoadSnapshot& get_cached_snapshot(uint32_t shard_id) const {
        return _snapshot_cache[shard_id];
    }

    // Register Prometheus metrics
    void register_metrics() {
        namespace sm = seastar::metrics;
        _metrics.add_group("ranvier_load_balancer", {
            sm::make_counter("local_dispatches", _local_dispatches,
                sm::description("Requests processed on receiving shard (no cross-shard dispatch)")),
            sm::make_counter("cross_shard_dispatches", _cross_shard_dispatches,
                sm::description("Requests dispatched to a different shard via P2C")),
            sm::make_counter("p2c_selections", _p2c_selections,
                sm::description("Total P2C shard selection operations")),
            sm::make_gauge("enabled", [this] { return _config.enabled ? 1.0 : 0.0; },
                sm::description("Whether shard load balancing is enabled (1=yes, 0=no)")),
        });
    }

private:
    // Fetch load snapshot from a specific shard
    seastar::future<ShardLoadSnapshot> fetch_shard_snapshot(uint32_t shard_id) {
        return seastar::smp::submit_to(shard_id, [] {
            if (shard_load_metrics_initialized()) {
                return shard_load_metrics().snapshot();
            }
            // Return empty snapshot if metrics not initialized
            ShardLoadSnapshot empty;
            empty.shard_id = seastar::this_shard_id();
            empty.timestamp = std::chrono::steady_clock::now();
            return empty;
        });
    }

    ShardLoadBalancerConfig _config;
    uint32_t _shard_count;

    // Thread-local RNG for P2C random selection
    // Seeded with device random + shard ID for independence across shards
    std::mt19937 _rng;

    // Cached snapshots of all shards' load metrics
    // Updated periodically or on-demand
    std::vector<ShardLoadSnapshot> _snapshot_cache;

    // Metrics counters
    uint64_t _local_dispatches;
    uint64_t _cross_shard_dispatches;
    uint64_t _p2c_selections;

    // Prometheus metrics registration
    seastar::metrics::metric_groups _metrics;
};

}  // namespace ranvier
