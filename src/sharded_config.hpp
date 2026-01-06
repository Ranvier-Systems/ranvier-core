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
//
// Thread Safety:
// - Each shard has its own instance, so no locking is needed for reads
// - Updates are done via invoke_on_all() which serializes per-shard
// - The update() method should only be called from invoke_on_all()
class ShardedConfig {
public:
    // Default constructor - creates config with built-in defaults
    ShardedConfig() = default;

    // Initialize with a config (used by sharded<>::start())
    explicit ShardedConfig(RanvierConfig config) noexcept
        : _config(std::move(config)) {}

    // Copy/move - explicitly defaulted for clarity
    ShardedConfig(const ShardedConfig&) = default;
    ShardedConfig(ShardedConfig&&) noexcept = default;
    ShardedConfig& operator=(const ShardedConfig&) = default;
    ShardedConfig& operator=(ShardedConfig&&) noexcept = default;

    // Required by seastar::sharded<> - called during shutdown
    seastar::future<> stop() noexcept {
        return seastar::make_ready_future<>();
    }

    // Get the configuration (read-only access)
    // Use this for normal config access from services
    [[nodiscard]] const RanvierConfig& config() const noexcept {
        return _config;
    }

    // Update this config instance with new values (for hot-reload)
    // Called via sharded::invoke_on_all() to propagate changes to all cores
    //
    // Note: This replaces the entire config atomically. There's no partial
    // update - either all fields are updated or none are.
    void update(const RanvierConfig& new_config) {
        _config = new_config;
    }

    // Move-based update for efficiency when caller doesn't need the config anymore
    void update(RanvierConfig&& new_config) noexcept {
        _config = std::move(new_config);
    }

private:
    RanvierConfig _config;
};

}  // namespace ranvier
