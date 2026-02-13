// Ranvier Core - Kubernetes Service Discovery for GPU Backends
//
// Implements a K8s EndpointSlice watcher that:
// - Watches the Kubernetes API for EndpointSlice changes
// - Syncs discovered endpoints with the RouterService
// - Maps Kubernetes annotations to Ranvier's weight/priority settings
//
// Annotations:
//   ranvier.io/weight: "200"   - Backend weight (default: 100)
//   ranvier.io/priority: "1"   - Priority group (default: 0, lower = higher priority)

#pragma once

#include "config.hpp"
#include "logging.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/gate.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/tls.hh>

namespace ranvier {

// Logger for K8s discovery
inline seastar::logger log_k8s("ranvier.k8s");

// Annotation keys for weight and priority
constexpr const char* K8S_ANNOTATION_WEIGHT = "ranvier.io/weight";
constexpr const char* K8S_ANNOTATION_PRIORITY = "ranvier.io/priority";

// Default values
constexpr uint32_t K8S_DEFAULT_WEIGHT = 100;
constexpr uint32_t K8S_DEFAULT_PRIORITY = 0;

// Maximum allowed values (for sanity checks)
// Weight: 1,000,000 allows fine-grained load distribution
// Priority: 1000 levels should be more than enough for any deployment
constexpr uint32_t K8S_MAX_WEIGHT = 1000000;
constexpr uint32_t K8S_MAX_PRIORITY = 1000;

// Rule #4: Bounds for containers and buffers to prevent OOM
// MAX_RESPONSE_SIZE: Cap on k8s_get response body (16 MB)
constexpr size_t K8S_MAX_RESPONSE_SIZE = 16 * 1024 * 1024;
// MAX_LINE_SIZE: Cap on watch stream buffer between newlines (1 MB)
constexpr size_t K8S_MAX_LINE_SIZE = 1 * 1024 * 1024;
// MAX_ENDPOINTS: Cap on total tracked endpoints in the map
constexpr size_t K8S_MAX_ENDPOINTS = 1000;
// MAX_TOKEN_SIZE: Cap on service account token file size (1 MB)
// K8s projected tokens are typically 1-4KB but can exceed 4KB with custom audiences.
// 1 MB is generous while preventing unbounded reads.
constexpr size_t K8S_MAX_TOKEN_SIZE = 1 * 1024 * 1024;

// Represents a discovered backend endpoint
struct K8sEndpoint {
    std::string uid;                      // Unique identifier (pod UID + address)
    std::string address;                  // IP address
    uint16_t port;                        // Port number
    bool ready;                           // Ready for traffic
    uint32_t weight = K8S_DEFAULT_WEIGHT;
    uint32_t priority = K8S_DEFAULT_PRIORITY;

    // Generate a stable BackendId from the endpoint
    // Uses FNV-1a 64-bit hash for quality distribution, truncated to 31 bits (positive int32_t).
    // Deterministic across restarts (no randomized seed unlike absl::Hash).
    BackendId to_backend_id() const;

    bool operator==(const K8sEndpoint& other) const {
        return uid == other.uid;
    }
};

// Callback types for router integration
using BackendRegisterCallback = std::function<seastar::future<>(
    BackendId id, seastar::socket_address addr, uint32_t weight, uint32_t priority)>;
using BackendDrainCallback = std::function<seastar::future<>(BackendId id)>;

// K8sDiscoveryService: Watches Kubernetes for GPU backend endpoints
// Runs on shard 0 only, syncs discovered endpoints with RouterService
class K8sDiscoveryService {
public:
    explicit K8sDiscoveryService(const K8sDiscoveryConfig& config);

    // Set callbacks for router integration
    void set_register_callback(BackendRegisterCallback callback) {
        _register_callback = std::move(callback);
    }

    void set_drain_callback(BackendDrainCallback callback) {
        _drain_callback = std::move(callback);
    }

    // Start the discovery service
    seastar::future<> start();

    // Stop the discovery service
    seastar::future<> stop();

    // Check if discovery is enabled
    bool is_enabled() const { return _config.enabled; }

    // Get current endpoint count
    size_t endpoint_count() const { return _endpoints.size(); }

    // Force a resync (for testing or manual refresh)
    seastar::future<> resync();

private:
    K8sDiscoveryConfig _config;

    // Callbacks for router integration
    BackendRegisterCallback _register_callback;
    BackendDrainCallback _drain_callback;

    // Current state of discovered endpoints
    // Using absl::flat_hash_map for SIMD-accelerated lookups and better cache locality
    absl::flat_hash_map<std::string, K8sEndpoint> _endpoints;  // UID -> Endpoint

    // Reverse map for BackendId collision detection: BackendId -> UID
    absl::flat_hash_map<BackendId, std::string> _backend_id_to_uid;

    // Running state
    bool _running = false;
    seastar::gate _gate;

    // Service account token (read from file)
    std::string _bearer_token;

    // TLS credentials for K8s API
    seastar::shared_ptr<seastar::tls::certificate_credentials> _tls_creds;

    // Timers
    seastar::timer<> _poll_timer;

    // Resource version for watch
    std::string _resource_version;

    // Metrics
    uint64_t _syncs_total = 0;
    uint64_t _syncs_failed = 0;
    uint64_t _endpoints_added = 0;
    uint64_t _endpoints_removed = 0;
    uint64_t _watch_reconnects = 0;
    uint64_t _dns_resolutions = 0;
    uint64_t _dns_failures = 0;
    uint64_t _dns_timeouts = 0;
    uint64_t _dns_cache_hits = 0;
    uint64_t _response_size_exceeded = 0;
    uint64_t _line_size_exceeded = 0;
    uint64_t _endpoints_limit_exceeded = 0;
    uint64_t _backend_id_collisions = 0;
    uint64_t _watch_410_gone = 0;

    // Seastar metrics registration
    seastar::metrics::metric_groups _metrics;

    // Futures for background tasks
    seastar::future<> _watch_future;

    // Cached API server address for graceful degradation
    std::optional<seastar::socket_address> _cached_api_server_addr;

    // Resolve API server hostname to socket address with retry and caching
    // Returns resolved address, or uses cached address on transient failures
    seastar::future<seastar::socket_address> resolve_api_server(
        const std::string& host, uint16_t port);

    // Load service account token from file
    seastar::future<> load_service_account_token();

    // Initialize TLS credentials
    seastar::future<> init_tls();

    // Load CA certificate from file asynchronously
    // Returns empty string if file doesn't exist or on read error (logs at warn level)
    seastar::future<std::string> load_ca_cert(const std::string& path);

    // Perform full sync of endpoints
    seastar::future<> sync_endpoints();

    // Start watching for changes (streaming)
    seastar::future<> watch_endpoints();

    // Parse EndpointSlice JSON response
    std::vector<K8sEndpoint> parse_endpoint_slices(const std::string& json);

    // Parse single EndpointSlice object
    std::vector<K8sEndpoint> parse_endpoint_slice(const rapidjson::Value& doc);

    // Handle endpoint changes
    seastar::future<> handle_endpoint_added(const K8sEndpoint& endpoint);
    seastar::future<> handle_endpoint_removed(const std::string& uid);
    seastar::future<> handle_endpoint_modified(const K8sEndpoint& endpoint);

    // Reconcile current state with discovered endpoints
    seastar::future<> reconcile(std::vector<K8sEndpoint> discovered);

    // HTTP helpers for K8s API
    seastar::future<std::string> k8s_get(const std::string& path);
    seastar::future<> k8s_watch(const std::string& path,
                                std::function<seastar::future<bool>(const std::string&)> on_event);

    // Build full URL for K8s API endpoint
    std::string build_url(const std::string& path) const;

    // Parse host and port from API server URL
    std::pair<std::string, uint16_t> parse_api_server() const;
};

}  // namespace ranvier
