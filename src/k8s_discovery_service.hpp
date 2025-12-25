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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/gate.hh>
#include <seastar/net/api.hh>
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

// Represents a discovered backend endpoint
struct K8sEndpoint {
    std::string uid;                      // Unique identifier (pod UID + address)
    std::string address;                  // IP address
    uint16_t port;                        // Port number
    bool ready;                           // Ready for traffic
    uint32_t weight = K8S_DEFAULT_WEIGHT;
    uint32_t priority = K8S_DEFAULT_PRIORITY;

    // Generate a stable BackendId from the endpoint
    // Uses a hash of the UID for consistency across restarts
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
    std::unordered_map<std::string, K8sEndpoint> _endpoints;  // UID -> Endpoint

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

    // Seastar metrics registration
    seastar::metrics::metric_groups _metrics;

    // Futures for background tasks
    seastar::future<> _watch_future;

    // Load service account token from file
    seastar::future<> load_service_account_token();

    // Initialize TLS credentials
    seastar::future<> init_tls();

    // Perform full sync of endpoints
    seastar::future<> sync_endpoints();

    // Start watching for changes (streaming)
    seastar::future<> watch_endpoints();

    // Parse EndpointSlice JSON response
    std::vector<K8sEndpoint> parse_endpoint_slices(const std::string& json);

    // Parse single EndpointSlice object
    std::vector<K8sEndpoint> parse_endpoint_slice(const std::string& json);

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

// Helper functions for JSON parsing (minimal, no external JSON library required)
namespace k8s_json {
    // Extract string value for a key from JSON object
    std::optional<std::string> get_string(const std::string& json, const std::string& key);

    // Extract integer value for a key from JSON object
    std::optional<int64_t> get_int(const std::string& json, const std::string& key);

    // Extract boolean value for a key from JSON object
    std::optional<bool> get_bool(const std::string& json, const std::string& key);

    // Extract array as vector of JSON object strings
    std::vector<std::string> get_array(const std::string& json, const std::string& key);

    // Extract nested object as JSON string
    std::optional<std::string> get_object(const std::string& json, const std::string& key);

    // Check if JSON represents an error response
    bool is_error(const std::string& json);

    // Get error message from K8s error response
    std::string get_error_message(const std::string& json);
}

}  // namespace ranvier
