// Ranvier Core - Kubernetes Service Discovery Implementation
//
// Implements watching K8s EndpointSlices and syncing with RouterService

#include "k8s_discovery_service.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include <rapidjson/error/en.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <seastar/core/coroutine.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/tls.hh>

namespace ranvier {

// Maximum concurrent endpoint operations to prevent overwhelming backends
constexpr size_t K8S_MAX_CONCURRENT_ENDPOINT_OPS = 16;

// Generate a stable BackendId from endpoint UID
BackendId K8sEndpoint::to_backend_id() const {
    // Use a simple hash of the UID string
    // This provides stability across restarts while being deterministic
    uint32_t hash = 0;
    for (char c : uid) {
        hash = hash * 31 + static_cast<uint32_t>(c);
    }
    // Ensure positive BackendId (use upper 31 bits)
    return static_cast<BackendId>(hash & 0x7FFFFFFF);
}

K8sDiscoveryService::K8sDiscoveryService(const K8sDiscoveryConfig& config)
    : _config(config),
      _watch_future(seastar::make_ready_future<>()) {

    if (!_config.enabled) {
        return;
    }

    // Register metrics
    _metrics.add_group("ranvier", {
        seastar::metrics::make_counter("k8s_syncs_total", _syncs_total,
            seastar::metrics::description("Total number of K8s endpoint syncs")),
        seastar::metrics::make_counter("k8s_syncs_failed", _syncs_failed,
            seastar::metrics::description("Total number of failed K8s endpoint syncs")),
        seastar::metrics::make_counter("k8s_endpoints_added", _endpoints_added,
            seastar::metrics::description("Total number of K8s endpoints added")),
        seastar::metrics::make_counter("k8s_endpoints_removed", _endpoints_removed,
            seastar::metrics::description("Total number of K8s endpoints removed")),
        seastar::metrics::make_counter("k8s_watch_reconnects", _watch_reconnects,
            seastar::metrics::description("Total number of K8s watch reconnections")),
        seastar::metrics::make_gauge("k8s_endpoints_current", [this] { return _endpoints.size(); },
            seastar::metrics::description("Current number of discovered K8s endpoints"))
    });
}

seastar::future<> K8sDiscoveryService::start() {
    if (!_config.enabled) {
        log_k8s.info("K8s discovery disabled");
        co_return;
    }

    if (_config.service_name.empty()) {
        log_k8s.error("K8s discovery enabled but no service_name configured");
        co_return;
    }

    log_k8s.info("Starting K8s discovery service for {}/{}",
                 _config.namespace_name, _config.service_name);

    _running = true;

    try {
        // Load auth and TLS
        co_await load_service_account_token();
        co_await init_tls();

        // Perform the initial full synchronization
        // This populates _endpoints and sets _resource_version
        co_await sync_endpoints();

        // Launch the background watch
        // We do NOT co_await this here because it runs forever
        _watch_future = watch_endpoints();

        // Start the periodic resync timer
        // This ensures state consistency even if the watch misses an event
        _poll_timer.set_callback([this] {
            // We launch this into the background and don't wait for it
            // but we use a gate or check _running to stay safe
            if (_running) {
                (void)sync_endpoints().handle_exception([](auto ep) {
                    log_k8s.warn("Periodic resync failed: {}", ep);
                });
            }
        });

        // Use the interval from config, or default to 60s
        _poll_timer.arm_periodic(std::chrono::seconds(60));

        log_k8s.info("K8s discovery service started successfully");
    } catch (const std::exception& e) {
        log_k8s.error("Failed to start K8s discovery: {}", e.what());
        _running = false;
        throw;
    }
}

seastar::future<> K8sDiscoveryService::stop() {
    if (!_running) {
        co_return;
    }

    log_k8s.info("Stopping K8s discovery service");
    _running = false;

    // Stop the poll timer
    _poll_timer.cancel();

    // Wait for any in-flight operations
    co_await _gate.close();

    // Wait for watch to complete
    try {
        co_await std::move(_watch_future);
    } catch (...) {
        log_k8s.debug("Watch future completed during shutdown");
    }

    log_k8s.info("K8s discovery service stopped");
    co_return;
}

seastar::future<> K8sDiscoveryService::resync() {
    return sync_endpoints();
}

seastar::future<> K8sDiscoveryService::load_service_account_token() {
    return seastar::file_exists(_config.token_path).then([this](bool exists) {
        if (!exists) {
            log_k8s.warn("Token file not found at {} - continuing without auth", _config.token_path);
            return seastar::make_ready_future<>();
        }

        return seastar::open_file_dma(_config.token_path, seastar::open_flags::ro)
            .then([](seastar::file f) {
                return seastar::make_file_input_stream(f).read_exactly(4096); // Tokens are usually small
            }).then([this](seastar::temporary_buffer<char> buf) {
                _bearer_token = std::string(buf.get(), buf.size());

                // Trim whitespace
                _bearer_token.erase(_bearer_token.find_last_not_of(" \n\r\t") + 1);
                _bearer_token.erase(0, _bearer_token.find_first_not_of(" \n\r\t"));

                log_k8s.debug("Loaded service account token ({} bytes)", _bearer_token.size());
            });
    }).handle_exception([this](auto ep) {
        log_k8s.warn("Could not load service account token: {} - continuing without auth", ep);
    });
}

seastar::future<> K8sDiscoveryService::init_tls() {
    if (!_config.api_server.starts_with("https://")) {
        log_k8s.debug("API server is not HTTPS, skipping TLS init");
        co_return;
    }

    try {
        auto builder = seastar::tls::credentials_builder();
        bool needs_system_trust = false;

        if (_config.verify_tls) {
            // Try to load CA certificate
            try {
                std::ifstream ca_file(_config.ca_cert_path);
                if (ca_file.is_open()) {
                    std::stringstream buffer;
                    buffer << ca_file.rdbuf();
                    builder.set_x509_trust(buffer.str(), seastar::tls::x509_crt_format::PEM);
                    log_k8s.debug("Loaded CA certificate from {}", _config.ca_cert_path);
                } else {
                    needs_system_trust = true;
                    log_k8s.debug("Using system CA certificates");
                }
            } catch (const std::exception& e) {
                log_k8s.warn("Failed to load CA cert, using system trust: {}", e.what());
                needs_system_trust = true;
            }

            if (needs_system_trust) {
                // Fall back to system CA
                // Note: co_await is must be outside the catch block.
                co_await builder.set_system_trust();
                log_k8s.debug("Using system CA certificates");
            }
        } else {
            // Disable verification (not recommended for production)
            builder.set_client_auth(seastar::tls::client_auth::NONE);
            log_k8s.warn("TLS verification disabled - not recommended for production");
        }

        _tls_creds = builder.build_certificate_credentials();

    } catch (const std::exception& e) {
        log_k8s.error("Failed to initialize TLS: {}", e.what());
        throw;
    }

    co_return;
}

std::pair<std::string, uint16_t> K8sDiscoveryService::parse_api_server() const {
    std::string url = _config.api_server;

    // Remove protocol prefix
    if (url.starts_with("https://")) {
        url = url.substr(8);
    } else if (url.starts_with("http://")) {
        url = url.substr(7);
    }

    // Find port separator
    auto colon_pos = url.rfind(':');
    auto slash_pos = url.find('/');

    std::string host;
    uint16_t port;

    if (colon_pos != std::string::npos && (slash_pos == std::string::npos || colon_pos < slash_pos)) {
        host = url.substr(0, colon_pos);
        std::string port_str = url.substr(colon_pos + 1);
        if (slash_pos != std::string::npos) {
            port_str = port_str.substr(0, slash_pos - colon_pos - 1);
        }
        port = static_cast<uint16_t>(std::stoi(port_str));
    } else {
        // No port specified, use default
        if (slash_pos != std::string::npos) {
            host = url.substr(0, slash_pos);
        } else {
            host = url;
        }
        port = _config.api_server.starts_with("https://") ? 443 : 80;
    }

    return {host, port};
}

std::string K8sDiscoveryService::build_url(const std::string& path) const {
    return _config.api_server + path;
}

seastar::future<std::string> K8sDiscoveryService::k8s_get(const std::string& path) {
    auto gate_holder = _gate.hold();

    auto [host, port] = parse_api_server();
    bool use_tls = _config.api_server.starts_with("https://");

    log_k8s.debug("K8s GET {} (host={}, port={}, tls={})", path, host, port, use_tls);

    try {
        // Resolve host
        seastar::socket_address addr;
        try {
            seastar::net::inet_address inet_addr(host);
            addr = seastar::socket_address(inet_addr, port);
        } catch (...) {
            // Host might be a DNS name - in K8s, kubernetes.default.svc resolves via DNS
            // For now, try direct IP parsing; real implementation would use DNS resolver
            log_k8s.error("Cannot resolve host: {} - DNS resolution not implemented", host);
            throw std::runtime_error("Cannot resolve host: " + host);
        }

        // Connect
        seastar::connected_socket sock;
        if (use_tls && _tls_creds) {
            seastar::tls::tls_options opts;
            opts.server_name = host;
            sock = co_await seastar::tls::connect(_tls_creds, addr, std::move(opts));
        } else {
            sock = co_await seastar::connect(addr);
        }

        auto in = sock.input();
        auto out = sock.output();

        // Build HTTP request
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n";
        req << "Host: " << host << "\r\n";
        req << "Accept: application/json\r\n";
        req << "Connection: close\r\n";
        if (!_bearer_token.empty()) {
            req << "Authorization: Bearer " << _bearer_token << "\r\n";
        }
        req << "\r\n";

        std::string request = req.str();
        co_await out.write(request);
        co_await out.flush();

        // Read response
        std::string response;
        while (!in.eof()) {
            auto buf = co_await in.read();
            if (buf.empty()) break;
            response.append(buf.get(), buf.size());
        }

        co_await out.close();
        co_await in.close();

        // Parse HTTP response
        auto header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            throw std::runtime_error("Invalid HTTP response: no header terminator");
        }

        std::string headers = response.substr(0, header_end);
        std::string body = response.substr(header_end + 4);

        // Check status code
        if (headers.find("200 OK") == std::string::npos &&
            headers.find("200 ") == std::string::npos) {
            // Extract status line
            auto status_end = headers.find("\r\n");
            std::string status = headers.substr(0, status_end);
            throw std::runtime_error("K8s API error: " + status);
        }

        // Handle chunked transfer encoding
        if (headers.find("Transfer-Encoding: chunked") != std::string::npos) {
            std::string decoded;
            size_t pos = 0;
            try {
                while (pos < body.size()) {
                    size_t line_end = body.find("\r\n", pos);
                    if (line_end == std::string::npos) break;

                    std::string size_line = body.substr(pos, line_end - pos);
                    if (size_line.empty()) { // Skip empty lines between chunks
                        pos = line_end + 2;
                        continue;
                    }

                    // Handle potential chunk extensions by finding the first non-hex char
                    size_t ext_pos = size_line.find_first_not_of("0123456789abcdefABCDEF");
                    size_t chunk_size = std::stoul(size_line.substr(0, ext_pos), nullptr, 16);

                    if (chunk_size == 0) break; // End of stream

                    pos = line_end + 2;
                    if (pos + chunk_size > body.size()) {
                        throw std::runtime_error("Incomplete chunk data");
                    }

                    decoded.append(body.data() + pos, chunk_size);
                    pos += chunk_size + 2; // Move past data and the trailing \r\n
                }
                body = std::move(decoded);
            } catch (const std::exception& e) {
                throw std::runtime_error("Failed to decode chunked response: " + std::string(e.what()));
            }
        }

        co_return body;

    } catch (const std::exception& e) {
        log_k8s.error("K8s GET {} failed: {}", path, e.what());
        throw;
    }
}

seastar::future<> K8sDiscoveryService::sync_endpoints() {
    if (!_running) {
        co_return;
    }

    log_k8s.info("Synchronizing endpoints for service: {}", _config.service_name);

    std::string path = "/apis/discovery.k8s.io/v1/namespaces/" + _config.namespace_name
                     + "/endpointslices?labelSelector=kubernetes.io/service-name=" + _config.service_name;

    std::string response = co_await k8s_get(path);

    rapidjson::Document doc;
    if (doc.Parse(response.c_str()).HasParseError()) {
        log_k8s.error("Failed to parse sync response: {}", rapidjson::GetParseError_En(doc.GetParseError()));
        co_return;
    }

    // Extract the Resource Version from List metadata
    if (doc.HasMember("metadata") && doc["metadata"].IsObject()) {
        const auto& meta = doc["metadata"];
        if (meta.HasMember("resourceVersion") && meta["resourceVersion"].IsString()) {
            _resource_version = meta["resourceVersion"].GetString();
            log_k8s.debug("Initial resourceVersion set to {}", _resource_version);
        }
    }

    // Process Items
    // Using absl::flat_hash_set for SIMD-accelerated UID tracking
    absl::flat_hash_set<std::string> current_uids;

    // Collect all operations first, then process in parallel
    // Pair: {endpoint, is_new}
    std::vector<std::pair<K8sEndpoint, bool>> operations;

    if (doc.HasMember("items") && doc["items"].IsArray()) {
        for (const auto& item : doc["items"].GetArray()) {
            auto discovered = parse_endpoint_slice(item);
            for (const auto& ep : discovered) {
                current_uids.insert(ep.uid);
                bool is_new = _endpoints.find(ep.uid) == _endpoints.end();
                operations.emplace_back(ep, is_new);
            }
        }
    }

    // Process endpoints in parallel with concurrency limit
    // Error in one endpoint doesn't fail the entire batch
    co_await seastar::max_concurrent_for_each(operations, K8S_MAX_CONCURRENT_ENDPOINT_OPS,
        [this](const std::pair<K8sEndpoint, bool>& op) -> seastar::future<> {
            const auto& [ep, is_new] = op;
            try {
                if (is_new) {
                    co_await handle_endpoint_added(ep);
                } else {
                    co_await handle_endpoint_modified(ep);
                }
            } catch (const std::exception& e) {
                log_k8s.warn("Failed to process endpoint {}: {}", ep.uid, e.what());
            }
        });

    // Garbage collect endpoints no longer present in K8s
    for (auto it = _endpoints.begin(); it != _endpoints.end(); ) {
        if (current_uids.find(it->first) == current_uids.end()) {
            std::string uid_to_remove = it->first;
            it++;
            co_await handle_endpoint_removed(uid_to_remove);
        } else {
            it++;
        }
    }
}

seastar::future<> K8sDiscoveryService::reconcile(std::vector<K8sEndpoint> discovered) {
    // Build set of discovered UIDs
    // Using absl::flat_hash_set for SIMD-accelerated UID tracking
    absl::flat_hash_set<std::string> discovered_uids;
    for (const auto& ep : discovered) {
        discovered_uids.insert(ep.uid);
    }

    // Find endpoints to remove (in current but not in discovered)
    std::vector<std::string> to_remove;
    for (const auto& [uid, ep] : _endpoints) {
        if (discovered_uids.find(uid) == discovered_uids.end()) {
            to_remove.push_back(uid);
        }
    }

    // Remove stale endpoints in parallel
    co_await seastar::max_concurrent_for_each(to_remove, K8S_MAX_CONCURRENT_ENDPOINT_OPS,
        [this](const std::string& uid) -> seastar::future<> {
            try {
                co_await handle_endpoint_removed(uid);
            } catch (const std::exception& e) {
                log_k8s.warn("Failed to remove endpoint {}: {}", uid, e.what());
            }
        });

    // Collect add/update operations first, then process in parallel
    // Pair: {endpoint, is_new}
    std::vector<std::pair<K8sEndpoint, bool>> operations;
    for (const auto& ep : discovered) {
        auto it = _endpoints.find(ep.uid);
        if (it == _endpoints.end()) {
            // New endpoint
            operations.emplace_back(ep, true);
        } else {
            // Check if endpoint changed (weight, priority, readiness)
            if (it->second.ready != ep.ready ||
                it->second.weight != ep.weight ||
                it->second.priority != ep.priority) {
                operations.emplace_back(ep, false);
            }
        }
    }

    // Process add/update operations in parallel
    co_await seastar::max_concurrent_for_each(operations, K8S_MAX_CONCURRENT_ENDPOINT_OPS,
        [this](const std::pair<K8sEndpoint, bool>& op) -> seastar::future<> {
            const auto& [ep, is_new] = op;
            try {
                if (is_new) {
                    co_await handle_endpoint_added(ep);
                } else {
                    co_await handle_endpoint_modified(ep);
                }
            } catch (const std::exception& e) {
                log_k8s.warn("Failed to process endpoint {}: {}", ep.uid, e.what());
            }
        });

    co_return;
}

seastar::future<> K8sDiscoveryService::handle_endpoint_added(const K8sEndpoint& endpoint) {
    log_k8s.info("K8s endpoint added: {} ({}:{}, weight={}, priority={}, ready={})",
                 endpoint.uid, endpoint.address, endpoint.port,
                 endpoint.weight, endpoint.priority, endpoint.ready);

    _endpoints[endpoint.uid] = endpoint;
    ++_endpoints_added;

    // Only register ready endpoints
    if (endpoint.ready && _register_callback) {
        try {
            seastar::net::inet_address inet_addr(endpoint.address);
            seastar::socket_address addr(inet_addr, endpoint.port);

            co_await _register_callback(endpoint.to_backend_id(), addr,
                                         endpoint.weight, endpoint.priority);
        } catch (const std::exception& e) {
            log_k8s.error("Failed to register backend for {}: {}", endpoint.uid, e.what());
        }
    }

    co_return;
}

seastar::future<> K8sDiscoveryService::handle_endpoint_removed(const std::string& uid) {
    auto it = _endpoints.find(uid);
    if (it == _endpoints.end()) {
        co_return;
    }

    const auto& endpoint = it->second;
    log_k8s.info("K8s endpoint removed: {} ({}:{})",
                 endpoint.uid, endpoint.address, endpoint.port);

    ++_endpoints_removed;

    // Drain the backend
    if (_drain_callback) {
        try {
            co_await _drain_callback(endpoint.to_backend_id());
        } catch (const std::exception& e) {
            log_k8s.error("Failed to drain backend for {}: {}", uid, e.what());
        }
    }

    _endpoints.erase(it);
    co_return;
}

seastar::future<> K8sDiscoveryService::handle_endpoint_modified(const K8sEndpoint& endpoint) {
    log_k8s.info("K8s endpoint modified: {} (ready={}, weight={}, priority={})",
                 endpoint.uid, endpoint.ready, endpoint.weight, endpoint.priority);

    auto it = _endpoints.find(endpoint.uid);
    bool was_ready = it != _endpoints.end() && it->second.ready;

    _endpoints[endpoint.uid] = endpoint;

    if (endpoint.ready && !was_ready) {
        // Became ready - register
        if (_register_callback) {
            try {
                seastar::net::inet_address inet_addr(endpoint.address);
                seastar::socket_address addr(inet_addr, endpoint.port);

                co_await _register_callback(endpoint.to_backend_id(), addr,
                                             endpoint.weight, endpoint.priority);
            } catch (const std::exception& e) {
                log_k8s.error("Failed to register backend for {}: {}", endpoint.uid, e.what());
            }
        }
    } else if (!endpoint.ready && was_ready) {
        // Became not ready - drain
        if (_drain_callback) {
            try {
                co_await _drain_callback(endpoint.to_backend_id());
            } catch (const std::exception& e) {
                log_k8s.error("Failed to drain backend for {}: {}", endpoint.uid, e.what());
            }
        }
    } else if (endpoint.ready) {
        // Still ready but weight/priority changed - re-register
        if (_register_callback) {
            try {
                seastar::net::inet_address inet_addr(endpoint.address);
                seastar::socket_address addr(inet_addr, endpoint.port);

                co_await _register_callback(endpoint.to_backend_id(), addr,
                                             endpoint.weight, endpoint.priority);
            } catch (const std::exception& e) {
                log_k8s.error("Failed to update backend for {}: {}", endpoint.uid, e.what());
            }
        }
    }

    co_return;
}

seastar::future<> K8sDiscoveryService::watch_endpoints() {
    if (!_running) {
        co_return;
    }

    log_k8s.info("Starting K8s watch for EndpointSlices (resourceVersion: {})", _resource_version);
    _watch_reconnects++;

    bool should_retry_delay = false;

    try {
        std::ostringstream path;
        path << "/apis/discovery.k8s.io/v1/namespaces/" << _config.namespace_name
             << "/endpointslices?watch=true&labelSelector=kubernetes.io/service-name=" << _config.service_name;

        if (!_resource_version.empty()) {
            path << "&resourceVersion=" << _resource_version;
        }

        // --- THE CO_AWAIT MUST BE INSIDE THIS FUNCTION ---
        co_await k8s_watch(path.str(), [this](const std::string& line) -> seastar::future<bool> {
            if (line.empty()) co_return true;

            rapidjson::Document event;
            if (event.Parse(line.c_str()).HasParseError()) co_return true;

            if (event.HasMember("kind") && std::string(event["kind"].GetString()) == "Status") {
                log_k8s.warn("Watch error: {}", event["message"].GetString());
                co_return false;
            }

            if (!event.HasMember("type") || !event.HasMember("object")) co_return true;

            std::string type = event["type"].GetString();
            const auto& obj = event["object"];

            if (obj.HasMember("metadata") && obj["metadata"].HasMember("resourceVersion")) {
                _resource_version = obj["metadata"]["resourceVersion"].GetString();
            }

            auto discovered = parse_endpoint_slice(obj);

            if (type == "ADDED" || type == "MODIFIED") {
                // Collect add/modify operations first, then process in parallel
                std::vector<std::pair<K8sEndpoint, bool>> operations;
                for (const auto& ep : discovered) {
                    bool is_new = _endpoints.find(ep.uid) == _endpoints.end();
                    operations.emplace_back(ep, is_new);
                }

                co_await seastar::max_concurrent_for_each(operations, K8S_MAX_CONCURRENT_ENDPOINT_OPS,
                    [this](const std::pair<K8sEndpoint, bool>& op) -> seastar::future<> {
                        const auto& [ep, is_new] = op;
                        try {
                            if (is_new) {
                                co_await handle_endpoint_added(ep);
                            } else {
                                co_await handle_endpoint_modified(ep);
                            }
                        } catch (const std::exception& e) {
                            log_k8s.warn("Failed to process endpoint {}: {}", ep.uid, e.what());
                        }
                    });
            } else if (type == "DELETED") {
                // Collect UIDs and remove in parallel
                std::vector<std::string> uids_to_remove;
                for (const auto& ep : discovered) {
                    uids_to_remove.push_back(ep.uid);
                }

                co_await seastar::max_concurrent_for_each(uids_to_remove, K8S_MAX_CONCURRENT_ENDPOINT_OPS,
                    [this](const std::string& uid) -> seastar::future<> {
                        try {
                            co_await handle_endpoint_removed(uid);
                        } catch (const std::exception& e) {
                            log_k8s.warn("Failed to remove endpoint {}: {}", uid, e.what());
                        }
                    });
            }

            co_return true;
        }); // End of lambda and k8s_watch call

    } catch (const std::exception& e) {
        log_k8s.error("Watch connection failed: {}. Will retry in 5s...", e.what());
        should_retry_delay = true;
    }

    if (should_retry_delay && _running) {
        co_await seastar::sleep(std::chrono::seconds(5));
    }

    if (_running) {
        _watch_future = watch_endpoints();
    }
}

std::vector<K8sEndpoint> K8sDiscoveryService::parse_endpoint_slices(const std::string& json) {
    std::vector<K8sEndpoint> all_endpoints;
    rapidjson::Document doc;

    if (doc.Parse(json.c_str()).HasParseError()) {
        log_k8s.error("JSON parse error: {} at offset {}",
                      rapidjson::GetParseError_En(doc.GetParseError()),
                      doc.GetErrorOffset());
        return all_endpoints;
    }

    if (doc.HasMember("items") && doc["items"].IsArray()) {
        for (const auto& item : doc["items"].GetArray()) {
            auto discovered = parse_endpoint_slice(item);
            all_endpoints.insert(all_endpoints.end(),
                                 std::make_move_iterator(discovered.begin()),
                                 std::make_move_iterator(discovered.end()));
        }
    }
    return all_endpoints;
}

std::vector<K8sEndpoint> K8sDiscoveryService::parse_endpoint_slice(const rapidjson::Value& doc) {
    std::vector<K8sEndpoint> endpoints;

    uint32_t base_weight = K8S_DEFAULT_WEIGHT;
    uint32_t base_priority = K8S_DEFAULT_PRIORITY;

    // 1. Extract Annotations (Weight/Priority)
    if (doc.HasMember("metadata") && doc["metadata"].IsObject()) {
        const auto& meta = doc["metadata"];
        if (meta.HasMember("annotations") && meta["annotations"].IsObject()) {
            const auto& ann = meta["annotations"];
            if (ann.HasMember(K8S_ANNOTATION_WEIGHT) && ann[K8S_ANNOTATION_WEIGHT].IsString()) {
                try { base_weight = std::stoul(ann[K8S_ANNOTATION_WEIGHT].GetString()); } catch (...) {}
            }
            if (ann.HasMember(K8S_ANNOTATION_PRIORITY) && ann[K8S_ANNOTATION_PRIORITY].IsString()) {
                try { base_priority = std::stoul(ann[K8S_ANNOTATION_PRIORITY].GetString()); } catch (...) {}
            }
        }
    }

    // 2. Extract Port
    uint16_t target_port = _config.target_port;
    if (doc.HasMember("ports") && doc["ports"].IsArray() && doc["ports"].Size() > 0) {
        const auto& p = doc["ports"][0];
        if (p.HasMember("port") && p["port"].IsInt()) {
            target_port = static_cast<uint16_t>(p["port"].GetInt());
        }
    }

    // 3. Process Endpoints
    if (doc.HasMember("endpoints") && doc["endpoints"].IsArray()) {
        for (const auto& ep : doc["endpoints"].GetArray()) {
            bool ready = true;
            if (ep.HasMember("conditions") && ep["conditions"].IsObject()) {
                const auto& cond = ep["conditions"];
                if (cond.HasMember("ready") && cond["ready"].IsBool()) {
                    ready = cond["ready"].GetBool();
                }
            }

            std::string pod_uid;
            if (ep.HasMember("targetRef") && ep["targetRef"].IsObject()) {
                const auto& ref = ep["targetRef"];
                if (ref.HasMember("uid") && ref["uid"].IsString()) {
                    pod_uid = ref["uid"].GetString();
                }
            }

            if (ep.HasMember("addresses") && ep["addresses"].IsArray()) {
                for (const auto& addr_node : ep["addresses"].GetArray()) {
                    if (addr_node.IsString()) {
                        K8sEndpoint endpoint;
                        endpoint.address = addr_node.GetString();
                        // Stability: Use Pod UID if available, otherwise fallback to address
                        endpoint.uid = pod_uid.empty() ? endpoint.address : pod_uid;
                        endpoint.port = target_port;
                        endpoint.ready = ready;
                        endpoint.weight = base_weight;
                        endpoint.priority = base_priority;
                        endpoints.push_back(std::move(endpoint));
                    }
                }
            }
        }
    }
    return endpoints;
}

seastar::future<> K8sDiscoveryService::k8s_watch(
    const std::string& path,
    std::function<seastar::future<bool>(const std::string&)> on_event) {

    auto [host, port] = parse_api_server();
    bool use_tls = _config.api_server.starts_with("https://");

    seastar::socket_address addr(seastar::net::inet_address(host), port);

    seastar::connected_socket sock;
    if (use_tls && _tls_creds) {
        seastar::tls::tls_options opts;
        opts.server_name = host;
        sock = co_await seastar::tls::connect(_tls_creds, addr, std::move(opts));
    } else {
        sock = co_await seastar::connect(addr);
    }

    auto out = sock.output();
    auto in = sock.input();

    // Send HTTP Request
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Authorization: Bearer " << _bearer_token << "\r\n"
        << "Accept: application/json\r\n"
        << "Connection: keep-alive\r\n\r\n";

    co_await out.write(req.str());
    co_await out.flush();

    // Simple line-based processing for the stream
    // Note: K8s sends one JSON object per line in watch mode
    bool keep_going = true;
    std::string buffer;

    while (keep_going && !in.eof()) {
        auto buf = co_await in.read();
        if (buf.empty()) break;

        buffer.append(buf.get(), buf.size());

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (!line.empty() && line != "\r") {
                keep_going = co_await on_event(line);
                if (!keep_going) break;
            }
        }
    }

    co_await out.close();
    co_await in.close();
}

}  // namespace ranvier
