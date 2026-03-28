// Ranvier Core - Local Backend Discovery Service
//
// Auto-discovers local LLM servers (Ollama, vLLM, LM Studio, llama.cpp, etc.)
// using semantic liveness checks. Probes configured ports with real HTTP
// requests to detect healthy backends, identify server types, and hot-add/remove
// them from the router.
//
// Runs on shard 0 only, following the HealthService pattern.

#pragma once

#include "config.hpp"
#include "logging.hpp"
#include "router_service.hpp"
#include "types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics.hh>

namespace ranvier {

// Logger for local discovery
inline seastar::logger log_discovery("ranvier.discovery");

// Represents a discovered local LLM backend
struct DiscoveredBackend {
    BackendId id = 0;                                  // Assigned during registration
    std::string address = "127.0.0.1";                 // Always localhost for local mode
    uint16_t port = 0;
    std::string server_type;                           // "ollama", "vllm", "lmstudio", "llamacpp", "localai", "textgenui", "unknown"
    std::vector<std::string> available_models;         // From /v1/models response
    std::chrono::steady_clock::time_point last_seen;   // Last successful probe
    uint32_t consecutive_failures = 0;                 // Missed probe counter for removal hysteresis
    static constexpr size_t MAX_MODELS = 128;          // Hard Rule #4
};

class LocalDiscoveryService {
public:
    struct Config {
        std::vector<uint16_t> discovery_ports;
        std::chrono::seconds scan_interval{10};        // How often to re-scan
        std::chrono::milliseconds probe_timeout{50};   // Semantic liveness timeout
        std::chrono::milliseconds connect_timeout{20}; // TCP connect timeout
        size_t max_backends = 32;                      // Hard Rule #4
    };

    explicit LocalDiscoveryService(RouterService& router, Config config);

    // Lifecycle — runs on shard 0 only
    seastar::future<> start();
    seastar::future<> stop();

private:
    RouterService& _router;
    Config _config;
    seastar::gate _gate;
    bool _running = false;
    seastar::future<> _loop_future = seastar::make_ready_future<>();

    // Currently tracked backends (port -> DiscoveredBackend)
    absl::flat_hash_map<uint16_t, DiscoveredBackend> _known_backends;
    static constexpr size_t MAX_KNOWN_BACKENDS = 64;   // Hard Rule #4
    uint64_t _overflow_drops = 0;

    // BackendId assignment — simple incrementing counter starting above
    // any manually-configured backend IDs to avoid collisions
    BackendId _next_backend_id = 10000;

    // Consecutive missed probes before removing a backend
    static constexpr uint32_t REMOVAL_THRESHOLD = 3;

    // Background loop
    seastar::future<> discovery_loop();

    // Probe a single port — returns nullopt if unhealthy/unreachable
    seastar::future<std::optional<DiscoveredBackend>> probe_port(uint16_t port);

    // Raw HTTP GET with timeout — lightweight, no connection pooling
    seastar::future<std::optional<seastar::sstring>> http_get_local(
        uint16_t port, std::string_view path,
        std::chrono::milliseconds timeout);

    // Parse /v1/models response to detect server type + model list
    DiscoveredBackend parse_models_response(
        uint16_t port, const seastar::sstring& body);

    // Detect server type from response headers/body heuristics
    std::string detect_server_type(uint16_t port, const seastar::sstring& body);

    // Reconcile probe results with known state — add/remove from router
    seastar::future<> reconcile(
        std::vector<DiscoveredBackend> discovered);

    // Metrics
    seastar::metrics::metric_groups _metrics;
    uint64_t _probes_sent = 0;
    uint64_t _probes_succeeded = 0;
    uint64_t _probes_failed = 0;
    uint64_t _backends_added = 0;
    uint64_t _backends_removed = 0;
};

}  // namespace ranvier
