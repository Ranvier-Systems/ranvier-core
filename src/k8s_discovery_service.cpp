// Ranvier Core - Kubernetes Service Discovery Implementation
//
// Implements watching K8s EndpointSlices and syncing with RouterService

#include "k8s_discovery_service.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include <seastar/core/coroutine.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/tls.hh>

namespace ranvier {

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

    log_k8s.debug("Periodic sync starting...");

    std::string path = "/apis/discovery.k8s.io/v1/namespaces/" + _config.namespace_name
                     + "/endpointslices?labelSelector=kubernetes.io/service-name=" + _config.service_name;

    std::string response = co_await k8s_get(path);

    // Capture the latest Resource Version
    auto metadata = k8s_json::get_object(response, "metadata");
    if (metadata) {
        auto rv = k8s_json::get_string(*metadata, "resourceVersion");
        if (rv) _resource_version = *rv;
    }

    auto items = k8s_json::get_array(response, "items");
    std::unordered_set<std::string> current_uids;

    for (const auto& item_json : items) {
        auto discovered = parse_endpoint_slice(item_json);
        for (const auto& ep : discovered) {
            current_uids.insert(ep.uid);

            auto it = _endpoints.find(ep.uid);
            if (it == _endpoints.end()) {
                co_await handle_endpoint_added(ep);
            } else {
                // If it exists, update it in case weights/annotations changed
                co_await handle_endpoint_modified(ep);
            }
        }
    }

    // Garbage collect endpoints that disappeared from K8s
    for (auto it = _endpoints.begin(); it != _endpoints.end(); ) {
        if (current_uids.find(it->first) == current_uids.end()) {
            std::string uid_to_remove = it->first;
            it++; // Advance iterator before deletion
            co_await handle_endpoint_removed(uid_to_remove);
        } else {
            it++;
        }
    }
}

std::vector<K8sEndpoint> K8sDiscoveryService::parse_endpoint_slices(const std::string& json) {
    std::vector<K8sEndpoint> endpoints;

    // Get the items array from the EndpointSliceList
    auto items = k8s_json::get_array(json, "items");

    for (const auto& item : items) {
        auto slice_endpoints = parse_endpoint_slice(item);
        endpoints.insert(endpoints.end(), slice_endpoints.begin(), slice_endpoints.end());
    }

    log_k8s.debug("Parsed {} endpoints from {} EndpointSlices", endpoints.size(), items.size());
    return endpoints;
}

std::vector<K8sEndpoint> K8sDiscoveryService::parse_endpoint_slice(const std::string& json) {
    std::vector<K8sEndpoint> endpoints;

    // Get metadata for annotations
    auto metadata = k8s_json::get_object(json, "metadata");
    uint32_t default_weight = K8S_DEFAULT_WEIGHT;
    uint32_t default_priority = K8S_DEFAULT_PRIORITY;

    if (metadata) {
        auto annotations = k8s_json::get_object(*metadata, "annotations");
        if (annotations) {
            auto weight_str = k8s_json::get_string(*annotations, K8S_ANNOTATION_WEIGHT);
            if (weight_str) {
                try {
                    default_weight = static_cast<uint32_t>(std::stoul(*weight_str));
                } catch (...) {
                    log_k8s.warn("Invalid weight annotation: {}", *weight_str);
                }
            }

            auto priority_str = k8s_json::get_string(*annotations, K8S_ANNOTATION_PRIORITY);
            if (priority_str) {
                try {
                    default_priority = static_cast<uint32_t>(std::stoul(*priority_str));
                } catch (...) {
                    log_k8s.warn("Invalid priority annotation: {}", *priority_str);
                }
            }
        }
    }

    // Get ports to find the target port
    uint16_t port = _config.target_port;
    auto ports = k8s_json::get_array(json, "ports");
    for (const auto& p : ports) {
        auto port_num = k8s_json::get_int(p, "port");
        if (port_num) {
            port = static_cast<uint16_t>(*port_num);
            break;  // Use first port
        }
    }

    // Get endpoints array
    auto endpoint_items = k8s_json::get_array(json, "endpoints");

    for (const auto& ep : endpoint_items) {
        // Check if endpoint is ready
        auto conditions = k8s_json::get_object(ep, "conditions");
        bool ready = true;
        if (conditions) {
            auto ready_val = k8s_json::get_bool(*conditions, "ready");
            if (ready_val) {
                ready = *ready_val;
            }
        }

        // Get target reference for UID
        std::string uid;
        auto target_ref = k8s_json::get_object(ep, "targetRef");
        if (target_ref) {
            auto ref_uid = k8s_json::get_string(*target_ref, "uid");
            if (ref_uid) {
                uid = *ref_uid;
            }
        }

        // Get addresses
        auto addresses = k8s_json::get_array(ep, "addresses");
        for (const auto& addr : addresses) {
            // addr is just a string in the array
            std::string address = addr;
            // Remove quotes if present
            if (address.size() >= 2 && address.front() == '"' && address.back() == '"') {
                address = address.substr(1, address.size() - 2);
            }

            K8sEndpoint endpoint;
            endpoint.uid = uid.empty() ? address : uid + "-" + address;
            endpoint.address = address;
            endpoint.port = port;
            endpoint.ready = ready;
            endpoint.weight = default_weight;
            endpoint.priority = default_priority;

            endpoints.push_back(std::move(endpoint));
        }
    }

    return endpoints;
}

seastar::future<> K8sDiscoveryService::reconcile(std::vector<K8sEndpoint> discovered) {
    // Build set of discovered UIDs
    std::unordered_set<std::string> discovered_uids;
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

    // Remove stale endpoints
    for (const auto& uid : to_remove) {
        co_await handle_endpoint_removed(uid);
    }

    // Add or update discovered endpoints
    for (auto& ep : discovered) {
        auto it = _endpoints.find(ep.uid);
        if (it == _endpoints.end()) {
            // New endpoint
            co_await handle_endpoint_added(ep);
        } else {
            // Check if endpoint changed (weight, priority, readiness)
            if (it->second.ready != ep.ready ||
                it->second.weight != ep.weight ||
                it->second.priority != ep.priority) {
                co_await handle_endpoint_modified(ep);
            }
        }
    }

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

        // We use a helper that processes the stream line by line
        co_await k8s_watch(path.str(), [this](const std::string& line) -> seastar::future<bool> {
            if (line.empty()) co_return true;

            // K8s Watch events are objects like: {"type": "ADDED", "object": {...}}
            auto type = k8s_json::get_string(line, "type");
            auto object = k8s_json::get_object(line, "object");

            if (!type || !object) {
                // Check if it's an error/expired resource version
                if (k8s_json::is_error(line)) {
                    log_k8s.warn("Watch error: {}", k8s_json::get_error_message(line));
                    co_return false; // Break the watch to trigger a full resync
                }
                co_return true;
            }

            // Update resource version to ensure we resume from this point if disconnected
            auto metadata = k8s_json::get_object(*object, "metadata");
            if (metadata) {
                auto rv = k8s_json::get_string(*metadata, "resourceVersion");
                if (rv) _resource_version = *rv;
            }

            // Parse the endpoints from the object (it's a single EndpointSlice)
            auto discovered = parse_endpoint_slice(*object);

            if (*type == "ADDED" || *type == "MODIFIED") {
                // Reconcile this specific slice
                for (const auto& ep : discovered) {
                    auto it = _endpoints.find(ep.uid);
                    if (it == _endpoints.end()) {
                        co_await handle_endpoint_added(ep);
                    } else {
                        co_await handle_endpoint_modified(ep);
                    }
                }
            } else if (*type == "DELETED") {
                for (const auto& ep : discovered) {
                    co_await handle_endpoint_removed(ep.uid);
                }
            }

            co_return true; // Keep watching
        });
    } catch (const std::exception& e) {
        log_k8s.error("Watch connection failed: {}. Retrying in {}s...",
                      e.what(), _config.watch_reconnect_delay.count());
        should_retry_delay = true;
    }

    if (should_retry_delay && _running) {
        co_await seastar::sleep(_config.watch_reconnect_delay);
    }

    // If we are still running, restart the watch
    if (_running) {
        _watch_future = watch_endpoints();
    }
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

// =============================================================================
// Minimal JSON Parser Implementation
// =============================================================================

namespace k8s_json {

// Find the end of a JSON string (handles escapes)
static size_t find_string_end(const std::string& json, size_t start) {
    size_t pos = start;
    while (pos < json.size()) {
        if (json[pos] == '\\') {
            pos += 2;  // Skip escaped char
        } else if (json[pos] == '"') {
            return pos;
        } else {
            ++pos;
        }
    }
    return std::string::npos;
}

// Find matching bracket/brace
static size_t find_matching(const std::string& json, size_t start, char open, char close) {
    int depth = 1;
    size_t pos = start;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '"') {
            pos = find_string_end(json, pos + 1);
            if (pos == std::string::npos) return std::string::npos;
        } else if (json[pos] == open) {
            ++depth;
        } else if (json[pos] == close) {
            --depth;
            if (depth == 0) return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

// Find key in JSON object
static std::optional<std::pair<size_t, size_t>> find_key_value(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = 0;

    while (pos < json.size()) {
        pos = json.find(search_key, pos);
        if (pos == std::string::npos) return std::nullopt;

        // Find colon after key
        size_t colon = json.find(':', pos + search_key.size());
        if (colon == std::string::npos) return std::nullopt;

        // Skip whitespace after colon
        size_t value_start = colon + 1;
        while (value_start < json.size() && std::isspace(json[value_start])) {
            ++value_start;
        }

        if (value_start >= json.size()) return std::nullopt;

        // Determine value end based on value type
        size_t value_end;
        char c = json[value_start];

        if (c == '"') {
            // String value
            value_end = find_string_end(json, value_start + 1);
            if (value_end != std::string::npos) ++value_end;
        } else if (c == '{') {
            // Object value
            value_end = find_matching(json, value_start + 1, '{', '}');
            if (value_end != std::string::npos) ++value_end;
        } else if (c == '[') {
            // Array value
            value_end = find_matching(json, value_start + 1, '[', ']');
            if (value_end != std::string::npos) ++value_end;
        } else {
            // Number, boolean, or null
            value_end = value_start;
            while (value_end < json.size() &&
                   !std::isspace(json[value_end]) &&
                   json[value_end] != ',' &&
                   json[value_end] != '}' &&
                   json[value_end] != ']') {
                ++value_end;
            }
        }

        if (value_end != std::string::npos && value_end > value_start) {
            return std::make_pair(value_start, value_end);
        }

        pos += search_key.size();
    }

    return std::nullopt;
}

std::optional<std::string> get_string(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    if (start >= json.size() || json[start] != '"') return std::nullopt;

    // Extract string content (without quotes)
    return json.substr(start + 1, end - start - 2);
}

std::optional<int64_t> get_int(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    std::string value = json.substr(start, end - start);

    try {
        return std::stoll(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> get_bool(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    std::string value = json.substr(start, end - start);

    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

std::vector<std::string> get_array(const std::string& json, const std::string& key) {
    std::vector<std::string> items;

    auto result = find_key_value(json, key);
    if (!result) return items;

    auto [start, end] = *result;
    if (start >= json.size() || json[start] != '[') return items;

    // Parse array elements
    size_t pos = start + 1;
    while (pos < end - 1) {
        // Skip whitespace
        while (pos < end && std::isspace(json[pos])) ++pos;
        if (pos >= end - 1 || json[pos] == ']') break;

        // Find element
        size_t elem_start = pos;
        size_t elem_end;
        char c = json[pos];

        if (c == '"') {
            elem_end = find_string_end(json, pos + 1);
            if (elem_end != std::string::npos) ++elem_end;
        } else if (c == '{') {
            elem_end = find_matching(json, pos + 1, '{', '}');
            if (elem_end != std::string::npos) ++elem_end;
        } else if (c == '[') {
            elem_end = find_matching(json, pos + 1, '[', ']');
            if (elem_end != std::string::npos) ++elem_end;
        } else {
            elem_end = pos;
            while (elem_end < end && json[elem_end] != ',' && json[elem_end] != ']') {
                ++elem_end;
            }
        }

        if (elem_end != std::string::npos && elem_end > elem_start) {
            items.push_back(json.substr(elem_start, elem_end - elem_start));
        }

        // Skip comma
        pos = elem_end;
        while (pos < end && (std::isspace(json[pos]) || json[pos] == ',')) ++pos;
    }

    return items;
}

std::optional<std::string> get_object(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    if (start >= json.size() || json[start] != '{') return std::nullopt;

    return json.substr(start, end - start);
}

bool is_error(const std::string& json) {
    auto kind = get_string(json, "kind");
    return kind && *kind == "Status";
}

std::string get_error_message(const std::string& json) {
    auto message = get_string(json, "message");
    return message.value_or("Unknown error");
}

}  // namespace k8s_json

}  // namespace ranvier
