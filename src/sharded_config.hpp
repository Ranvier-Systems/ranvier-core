// Ranvier Core - Sharded Configuration Service
//
// Wraps RanvierConfig for use with seastar::sharded<> to provide:
// - Per-core configuration distribution (one copy per shard)
// - Hot-reload via SIGHUP using sharded::invoke_on_all()
// - Thread-safe read access without locking (each shard has its own copy)
//
// This separation keeps config.hpp as a plain data structure that can be
// used before Seastar starts (e.g., in main.cpp for config loading).

#pragma once

#include "config.hpp"

#include <seastar/core/future.hh>

namespace ranvier {

// ShardedConfig wraps RanvierConfig for Seastar's sharded<> container.
// Each CPU core gets its own instance for lock-free access.
class ShardedConfig {
public:
    // Default constructor - creates empty config
    ShardedConfig() = default;

    // Initialize with a config (used by sharded<>::start())
    explicit ShardedConfig(RanvierConfig config)
        : _config(std::move(config)) {}

    // Required by seastar::sharded<> - called during shutdown
    seastar::future<> stop() { return seastar::make_ready_future<>(); }

    // Get the configuration (read-only access)
    const RanvierConfig& config() const { return _config; }

    // Get mutable access (for update operations)
    RanvierConfig& config() { return _config; }

    // Update this config instance with new values (for hot-reload)
    // Called via sharded::invoke_on_all() to propagate changes to all cores
    void update(const RanvierConfig& new_config) {
        _config = new_config;
    }

private:
    RanvierConfig _config;
};

}  // namespace ranvier
