// Ranvier Core - Kubernetes Service Discovery for GPU Backends
//
// Implements a K8s EndpointSlice watcher that:
// - Watches the Kubernetes API for EndpointSlice changes
// - Syncs discovered endpoints with the RouterService
// - Maps Kubernetes annotations to Ranvier's weight/priority settings
//
// Annotations:
//   ranvier.io/weight: "200"                    - Backend weight (default: 100)
//   ranvier.io/priority: "1"                    - Priority group (default: 0)
//   ranvier.io/backend-type: "cerebras"         - BackendType tag (default: vllm)
//   ranvier.io/api-key-secret-ref: "my-secret"  - Name of a K8s Secret in the
//                                                  same namespace whose `api-key`
//                                                  field holds the credential
//                                                  forwarded as Authorization: Bearer.

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
// Annotation keys for heterogeneous-backend support (see BACKLOG §19).
constexpr const char* K8S_ANNOTATION_BACKEND_TYPE = "ranvier.io/backend-type";
constexpr const char* K8S_ANNOTATION_API_KEY_SECRET_REF = "ranvier.io/api-key-secret-ref";
// Annotation key for disaggregated prefill/decode pools (BACKLOG §20.1 P0.3).
// Values: "unified" (default) | "prefill" | "decode".
constexpr const char* K8S_ANNOTATION_POOL_ROLE = "ranvier.io/pool-role";
// Annotation key for the native KV-event stream port (BACKLOG §20.1 P0.1).
// The subscriber connects to tcp://<pod-ip>:<port>; absent/0 = no stream.
constexpr const char* K8S_ANNOTATION_KV_EVENTS_PORT = "ranvier.io/kv-events-port";
// Port of the publisher's optional replay ROUTER socket; absent/0 = gaps
// reset instead of replaying. Requires kv-events-port.
constexpr const char* K8S_ANNOTATION_KV_EVENTS_REPLAY_PORT = "ranvier.io/kv-events-replay-port";

// Conventional Secret data-map field that holds the API key. Operators run
//   kubectl create secret generic my-key --from-literal=api-key=sk-...
// to provision a credential consumable via api-key-secret-ref.
constexpr const char* K8S_SECRET_API_KEY_FIELD = "api-key";

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
    // Heterogeneous-backend metadata. Default `VLLM` matches the historical
    // assumption baked into the rest of the codebase.
    BackendType type = BackendType::VLLM;
    // Disaggregated pool role from the ranvier.io/pool-role annotation.
    // Default UNIFIED keeps unannotated fleets routing exactly as before.
    PoolRole role = PoolRole::UNIFIED;
    // Native KV-event stream port (ranvier.io/kv-events-port). 0 = none.
    uint16_t kv_events_port = 0;
    // Replay ROUTER port (ranvier.io/kv-events-replay-port). 0 = no replay.
    uint16_t kv_events_replay_port = 0;
    // Name of a K8s Secret whose `api-key` field holds the credential.
    // Empty string means "no auth header" — correct for vLLM on a cluster-
    // internal network. The Secret value is resolved at endpoint-discovery
    // time and never stored on this struct (only the reference is).
    std::string api_key_secret_ref;

    // Generate a stable BackendId from the endpoint
    // Uses FNV-1a 64-bit hash for quality distribution, truncated to 31 bits (positive int32_t).
    // Deterministic across restarts (no randomized seed unlike absl::Hash).
    BackendId to_backend_id() const;

    bool operator==(const K8sEndpoint& other) const {
        return uid == other.uid;
    }
};

// Callback types for router integration. `type` and `api_key` were added for
// heterogeneous-fleet support, `role` for disaggregated prefill/decode pools —
// callers that don't care can pass VLLM/""/UNIFIED.
using BackendRegisterCallback = std::function<seastar::future<>(
    BackendId id, seastar::socket_address addr, uint32_t weight, uint32_t priority,
    BackendType type, std::string api_key, PoolRole role, uint16_t kv_events_port,
    uint16_t kv_events_replay_port)>;
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
        std::string host, uint16_t port);

    // Load service account token from file
    seastar::future<> load_service_account_token();

    // Initialize TLS credentials
    seastar::future<> init_tls();

    // Load CA certificate from file asynchronously
    // Returns empty string if file doesn't exist or on read error (logs at warn level)
    seastar::future<std::string> load_ca_cert(std::string path);

    // Perform full sync of endpoints
    seastar::future<> sync_endpoints();

    // Start watching for changes (streaming)
    seastar::future<> watch_endpoints();

    // Parse EndpointSlice JSON response
    std::vector<K8sEndpoint> parse_endpoint_slices(const std::string& json);

    // Parse single EndpointSlice object
    std::vector<K8sEndpoint> parse_endpoint_slice(const rapidjson::Value& doc);

    // Handle endpoint changes
    seastar::future<> handle_endpoint_added(K8sEndpoint endpoint);
    seastar::future<> handle_endpoint_removed(std::string uid);
    seastar::future<> handle_endpoint_modified(K8sEndpoint endpoint);

    // Resolve the endpoint's api-key Secret (if any) and invoke the
    // register callback. Logs and returns without registering if the
    // Secret can't be resolved. Centralises the secret-fetch + register
    // dance shared between handle_endpoint_added and handle_endpoint_modified.
    seastar::future<> register_with_secret(const K8sEndpoint& endpoint, BackendId backend_id);

    // Reconcile current state with discovered endpoints
    seastar::future<> reconcile(std::vector<K8sEndpoint> discovered);

    // HTTP helpers for K8s API
    seastar::future<std::string> k8s_get(std::string path);
    seastar::future<> k8s_watch(std::string path,
                                std::function<seastar::future<bool>(const std::string&)> on_event);

    // Fetch the `api-key` field from a namespace-local Secret. Returns the
    // decoded value, or std::nullopt on any failure (404, RBAC denied,
    // malformed JSON, missing/empty `api-key` field, base64 decode error).
    // Used at endpoint-discovery time to resolve `ranvier.io/api-key-secret-ref`.
    // The returned string is never logged.
    seastar::future<std::optional<std::string>> fetch_secret_api_key(std::string secret_name);

    // Build full URL for K8s API endpoint
    std::string build_url(const std::string& path) const;

    // Parse host and port from API server URL
    std::pair<std::string, uint16_t> parse_api_server() const;
};

}  // namespace ranvier
