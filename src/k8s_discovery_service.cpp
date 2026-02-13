// Ranvier Core - Kubernetes Service Discovery Implementation
//
// Implements watching K8s EndpointSlices and syncing with RouterService

#include "k8s_discovery_service.hpp"
#include "parse_utils.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <optional>
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
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/tls.hh>

namespace ranvier {

// Maximum concurrent endpoint operations to prevent overwhelming backends
constexpr size_t K8S_MAX_CONCURRENT_ENDPOINT_OPS = 16;

// Note: parse_port() is now provided by parse_utils.hpp using std::from_chars

// Parse the numeric HTTP status code from a response header block.
// HTTP status line format: "HTTP/<version> <code> <reason>\r\n..."
// Returns the 3-digit numeric code (e.g. 200, 404, 503), or std::nullopt
// if the status line is malformed or the code is not a valid integer.
static std::optional<int> parse_http_status_code(std::string_view headers) {
    // Extract the first line (status line)
    auto line_end = headers.find("\r\n");
    std::string_view status_line = headers.substr(0, line_end);

    // Find the status code field: "HTTP/x.x <code> <reason>"
    auto first_space = status_line.find(' ');
    if (first_space == std::string_view::npos) {
        return std::nullopt;
    }

    // Advance past whitespace to the start of the code
    auto code_start = first_space + 1;
    while (code_start < status_line.size() && status_line[code_start] == ' ') {
        ++code_start;
    }

    // Find the end of the code (next space or end of line)
    auto second_space = status_line.find(' ', code_start);
    auto code_end = (second_space != std::string_view::npos)
                        ? second_space
                        : status_line.size();

    if (code_start >= code_end) {
        return std::nullopt;
    }

    int code = 0;
    auto [ptr, ec] = std::from_chars(
        status_line.data() + code_start,
        status_line.data() + code_end,
        code);

    if (ec != std::errc{} || ptr != status_line.data() + code_end) {
        return std::nullopt;
    }

    return code;
}

// Generate a stable BackendId from endpoint UID using FNV-1a 64-bit hash.
// FNV-1a provides much better distribution than the previous hash*31+c polynomial,
// and is deterministic across restarts (unlike absl::Hash which uses ASLR-based seeds).
BackendId K8sEndpoint::to_backend_id() const {
    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;

    uint64_t hash = kFnvOffsetBasis;
    for (unsigned char c : uid) {
        hash ^= c;
        hash *= kFnvPrime;
    }
    // Ensure positive BackendId: truncate to 31 bits
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
            seastar::metrics::description("Current number of discovered K8s endpoints")),
        seastar::metrics::make_counter("k8s_dns_resolutions", _dns_resolutions,
            seastar::metrics::description("Total number of DNS resolutions for K8s API server")),
        seastar::metrics::make_counter("k8s_dns_failures", _dns_failures,
            seastar::metrics::description("Total number of DNS resolution failures")),
        seastar::metrics::make_counter("k8s_dns_timeouts", _dns_timeouts,
            seastar::metrics::description("Total number of DNS resolution timeouts")),
        seastar::metrics::make_counter("k8s_dns_cache_hits", _dns_cache_hits,
            seastar::metrics::description("Total number of DNS cache hits (fallback to cached address)")),
        seastar::metrics::make_counter("k8s_response_size_exceeded", _response_size_exceeded,
            seastar::metrics::description("Times K8s API response exceeded MAX_RESPONSE_SIZE")),
        seastar::metrics::make_counter("k8s_line_size_exceeded", _line_size_exceeded,
            seastar::metrics::description("Times K8s watch buffer exceeded MAX_LINE_SIZE")),
        seastar::metrics::make_counter("k8s_endpoints_limit_exceeded", _endpoints_limit_exceeded,
            seastar::metrics::description("Times endpoint insertion was rejected due to MAX_ENDPOINTS limit")),
        seastar::metrics::make_counter("k8s_backend_id_collisions", _backend_id_collisions,
            seastar::metrics::description("Times two different UIDs produced the same BackendId hash"))
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
            // Rule #5: Acquire gate holder for shutdown safety.
            // This prevents use-after-free if callback executes after stop() begins.
            seastar::gate::holder holder;
            try {
                holder = _gate.hold();
            } catch (const seastar::gate_closed_exception&) {
                log_k8s.debug("Periodic resync skipped: service is stopping");
                return;
            }

            if (!_running) {
                return;  // Logical check: service not in running state
            }

            // Keep gate holder alive for duration of async work via do_with
            (void)seastar::do_with(std::move(holder), [this](seastar::gate::holder&) {
                return sync_endpoints().handle_exception([](auto ep) {
                    log_k8s.warn("Periodic resync failed: {}", ep);
                });
            });
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

    // Rule #6: Deregister metrics FIRST to prevent use-after-free from
    // Prometheus scrapes that arrive after shutdown begins.
    _metrics.clear();

    // Rule #5: Close gate BEFORE cancelling timer.
    // This waits for any in-flight poll timer callbacks and k8s_get operations.
    // New callbacks will fail to acquire gate holder and return early.
    co_await _gate.close();

    // Now safe to cancel timer (no in-flight callbacks)
    _poll_timer.cancel();

    // Wait for watch to complete
    try {
        co_await std::move(_watch_future);
    } catch (const std::exception& e) {
        // Rule #9: Log at warn level with exception details. This can occur when the
        // watch connection is interrupted during shutdown - expected but worth noting.
        log_k8s.warn("Watch future completed with exception during shutdown: {}", e.what());
    } catch (...) {
        log_k8s.warn("Watch future completed with unknown exception during shutdown");
    }

    log_k8s.info("K8s discovery service stopped");
    co_return;
}

seastar::future<> K8sDiscoveryService::resync() {
    return sync_endpoints();
}

seastar::future<> K8sDiscoveryService::load_service_account_token() {
    bool exists = co_await seastar::file_exists(_config.token_path);
    if (!exists) {
        log_k8s.warn("Token file not found at {} - continuing without auth", _config.token_path);
        co_return;
    }

    try {
        auto file = co_await seastar::open_file_dma(_config.token_path, seastar::open_flags::ro);
        auto size = co_await file.size();

        if (size == 0) {
            log_k8s.warn("Token file is empty: {} - continuing without auth", _config.token_path);
            co_await file.close();
            co_return;
        }

        if (size > K8S_MAX_TOKEN_SIZE) {
            log_k8s.error("Token file {} is {} bytes, exceeds maximum allowed size ({} bytes) - "
                          "continuing without auth", _config.token_path, size, K8S_MAX_TOKEN_SIZE);
            co_await file.close();
            co_return;
        }

        auto stream = seastar::make_file_input_stream(file);
        auto buf = co_await stream.read_exactly(size);

        _bearer_token = std::string(buf.get(), buf.size());

        // Trim whitespace
        _bearer_token.erase(_bearer_token.find_last_not_of(" \n\r\t") + 1);
        _bearer_token.erase(0, _bearer_token.find_first_not_of(" \n\r\t"));

        log_k8s.debug("Loaded service account token ({} bytes)", _bearer_token.size());

        co_await stream.close();
        co_await file.close();
    } catch (const std::exception& e) {
        log_k8s.warn("Could not load service account token: {} - continuing without auth", e.what());
    }
}

seastar::future<std::string> K8sDiscoveryService::load_ca_cert(const std::string& path) {
    // Check if file exists first (async)
    bool exists = co_await seastar::file_exists(path);
    if (!exists) {
        log_k8s.debug("CA cert file not found at {}", path);
        co_return std::string{};
    }

    try {
        auto file = co_await seastar::open_file_dma(path, seastar::open_flags::ro);
        auto size = co_await file.size();

        if (size == 0) {
            log_k8s.warn("CA cert file is empty: {}", path);
            co_await file.close();
            co_return std::string{};
        }

        // Read entire file contents
        auto stream = seastar::make_file_input_stream(file);
        auto buf = co_await stream.read_exactly(size);
        co_await stream.close();
        co_await file.close();

        std::string content(buf.get(), buf.size());

        // Trim trailing whitespace/newlines
        while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back()))) {
            content.pop_back();
        }

        log_k8s.debug("Loaded CA certificate from {} ({} bytes)", path, content.size());
        co_return content;

    } catch (const std::system_error& e) {
        log_k8s.warn("Failed to read CA cert file {}: {}", path, e.what());
        co_return std::string{};
    } catch (const std::exception& e) {
        log_k8s.warn("Failed to read CA cert file {}: {}", path, e.what());
        co_return std::string{};
    }
}

seastar::future<> K8sDiscoveryService::init_tls() {
    if (!_config.api_server.starts_with("https://")) {
        log_k8s.debug("API server is not HTTPS, skipping TLS init");
        co_return;
    }

    try {
        auto builder = seastar::tls::credentials_builder();

        if (_config.verify_tls) {
            // Load CA certificate asynchronously (non-blocking)
            std::string ca_cert = co_await load_ca_cert(_config.ca_cert_path);

            if (!ca_cert.empty()) {
                builder.set_x509_trust(ca_cert, seastar::tls::x509_crt_format::PEM);
                log_k8s.debug("Loaded CA certificate from {}", _config.ca_cert_path);
            } else {
                // Fall back to system CA
                co_await builder.set_system_trust();
                log_k8s.debug("CA cert not available, using system CA certificates");
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
        auto parsed_port = parse_port(port_str);
        if (!parsed_port) {
            log_k8s.error("Invalid port in API server URL: '{}' (must be 1-65535)", port_str);
            throw std::invalid_argument("Invalid port number in K8s API server URL: " + port_str);
        }
        port = *parsed_port;
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

seastar::future<seastar::socket_address> K8sDiscoveryService::resolve_api_server(
    const std::string& host, uint16_t port) {

    // Fast path: Try parsing as direct IP address
    try {
        seastar::net::inet_address inet_addr(host);
        auto addr = seastar::socket_address(inet_addr, port);
        // Cache successful resolution for graceful degradation
        _cached_api_server_addr = addr;
        co_return addr;
    } catch (const std::exception& e) {
        // Not a valid IP - this is expected flow for hostnames, triggers DNS resolution below.
        // Rule #9 note: Debug level since this is flow control, not an error condition.
        log_k8s.debug("API server '{}' is not a direct IP ({}), will use DNS resolution", host, e.what());
    }

    // DNS resolution with retry and exponential backoff
    uint32_t attempt = 0;
    auto backoff = _config.dns_initial_backoff;
    std::exception_ptr last_exception;

    while (attempt <= _config.dns_max_retries) {
        try {
            ++_dns_resolutions;

            // Use Seastar's async DNS resolver with timeout
            auto deadline = seastar::lowres_clock::now() + _config.dns_timeout;

            auto hostent = co_await seastar::with_timeout(
                deadline,
                seastar::net::dns::get_host_by_name(host)
            );

            if (hostent.addr_list.empty()) {
                log_k8s.error("DNS resolution returned no addresses for: {} - "
                              "check CoreDNS/kube-dns configuration and network connectivity",
                              host);
                ++_dns_failures;
                throw std::runtime_error("DNS resolution returned no addresses for: " + host);
            }

            // Use the first address
            auto addr = seastar::socket_address(hostent.addr_list[0], port);

            // Cache successful resolution for graceful degradation
            _cached_api_server_addr = addr;

            log_k8s.debug("DNS resolved {} to {} (attempt {})",
                          host, hostent.addr_list[0], attempt + 1);

            co_return addr;

        } catch (const seastar::timed_out_error&) {
            ++_dns_timeouts;
            last_exception = std::current_exception();

            log_k8s.warn("DNS resolution timed out for {} (attempt {}/{}, timeout={}s) - "
                         "check network connectivity and DNS server responsiveness",
                         host, attempt + 1, _config.dns_max_retries + 1,
                         _config.dns_timeout.count());

        } catch (const std::system_error& e) {
            ++_dns_failures;
            last_exception = std::current_exception();

            // Provide actionable guidance based on error type
            if (e.code().value() == ENOENT || e.code().value() == ENOTDIR) {
                log_k8s.warn("DNS resolution failed for {} (attempt {}/{}) - "
                             "host not found: {} - verify kubernetes.default.svc is resolvable "
                             "and /etc/resolv.conf points to cluster DNS",
                             host, attempt + 1, _config.dns_max_retries + 1, e.what());
            } else {
                log_k8s.warn("DNS resolution failed for {} (attempt {}/{}): {} - "
                             "check network connectivity and DNS configuration",
                             host, attempt + 1, _config.dns_max_retries + 1, e.what());
            }

        } catch (const std::exception& e) {
            ++_dns_failures;
            last_exception = std::current_exception();

            log_k8s.warn("DNS resolution failed for {} (attempt {}/{}): {}",
                         host, attempt + 1, _config.dns_max_retries + 1, e.what());
        }

        // Check if we should retry
        if (attempt < _config.dns_max_retries) {
            log_k8s.debug("Retrying DNS resolution in {}ms", backoff.count());
            co_await seastar::sleep(backoff);

            // Exponential backoff with 2x multiplier, capped at 5 seconds
            backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
        }

        ++attempt;
    }

    // All retries exhausted - try to use cached address for graceful degradation
    if (_cached_api_server_addr.has_value()) {
        ++_dns_cache_hits;
        log_k8s.warn("DNS resolution failed after {} attempts for {} - "
                     "falling back to cached address {} for graceful degradation",
                     _config.dns_max_retries + 1, host, _cached_api_server_addr.value());
        co_return _cached_api_server_addr.value();
    }

    // No cached address available - must fail
    log_k8s.error("DNS resolution failed after {} attempts for {} with no cached fallback - "
                  "discovery service will be unavailable until DNS is restored. "
                  "Actions: 1) Check CoreDNS pods are running: kubectl get pods -n kube-system -l k8s-app=kube-dns "
                  "2) Verify /etc/resolv.conf in the pod "
                  "3) Test with: kubectl exec <pod> -- nslookup {}",
                  _config.dns_max_retries + 1, host, host);

    std::rethrow_exception(last_exception);
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
        // Resolve host using DNS-aware resolver with retry and graceful degradation
        seastar::socket_address addr = co_await resolve_api_server(host, port);

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

        // Read response (Rule #4: bounded to K8S_MAX_RESPONSE_SIZE)
        std::string response;
        while (!in.eof()) {
            auto buf = co_await in.read();
            if (buf.empty()) break;
            if (response.size() + buf.size() > K8S_MAX_RESPONSE_SIZE) {
                ++_response_size_exceeded;
                log_k8s.error("K8s API response exceeds MAX_RESPONSE_SIZE ({} bytes) - "
                              "aborting to prevent OOM", K8S_MAX_RESPONSE_SIZE);
                co_await out.close();
                co_await in.close();
                throw std::runtime_error("K8s API response exceeded size limit");
            }
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

        // Check status code — parse the numeric code from the HTTP status line
        // rather than doing a brittle string search through all headers.
        auto status_code = parse_http_status_code(headers);
        if (!status_code || *status_code != 200) {
            auto status_end = headers.find("\r\n");
            std::string status_line = headers.substr(0, status_end);
            throw std::runtime_error("K8s API error: " + status_line);
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
                    std::string_view hex_part(size_line.data(), ext_pos == std::string::npos ? size_line.size() : ext_pos);
                    size_t chunk_size = 0;
                    auto [ptr, ec] = std::from_chars(hex_part.data(), hex_part.data() + hex_part.size(), chunk_size, 16);
                    if (ec != std::errc{} || ptr != hex_part.data() + hex_part.size()) {
                        throw std::runtime_error("Invalid chunk size in chunked encoding");
                    }

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
    // Collect UIDs first, then remove in parallel (Rule #2: no co_await in loops)
    std::vector<std::string> uids_to_remove;
    for (const auto& [uid, ep] : _endpoints) {
        if (current_uids.find(uid) == current_uids.end()) {
            uids_to_remove.push_back(uid);
        }
    }

    co_await seastar::max_concurrent_for_each(uids_to_remove, K8S_MAX_CONCURRENT_ENDPOINT_OPS,
        [this](const std::string& uid) -> seastar::future<> {
            try {
                co_await handle_endpoint_removed(uid);
            } catch (const std::exception& e) {
                log_k8s.warn("Failed to remove stale endpoint {}: {}", uid, e.what());
            }
        });
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
    // Rule #4: Bound endpoints map to prevent OOM from broad selector
    if (_endpoints.find(endpoint.uid) == _endpoints.end() &&
        _endpoints.size() >= K8S_MAX_ENDPOINTS) {
        ++_endpoints_limit_exceeded;
        log_k8s.error("Endpoint limit reached ({}) - rejecting new endpoint {} ({}:{}) - "
                      "check label selector specificity",
                      K8S_MAX_ENDPOINTS, endpoint.uid, endpoint.address, endpoint.port);
        co_return;
    }

    auto backend_id = endpoint.to_backend_id();

    // Collision detection: check if a different UID already maps to this BackendId
    auto collision_it = _backend_id_to_uid.find(backend_id);
    if (collision_it != _backend_id_to_uid.end() && collision_it->second != endpoint.uid) {
        ++_backend_id_collisions;
        log_k8s.error("BackendId collision detected: UID '{}' and UID '{}' both hash to BackendId {} - "
                      "the new endpoint will shadow the existing one. "
                      "This is a hash collision that may cause routing errors.",
                      endpoint.uid, collision_it->second, backend_id);
    }
    _backend_id_to_uid[backend_id] = endpoint.uid;

    log_k8s.info("K8s endpoint added: {} ({}:{}, weight={}, priority={}, ready={}, backend_id={})",
                 endpoint.uid, endpoint.address, endpoint.port,
                 endpoint.weight, endpoint.priority, endpoint.ready, backend_id);

    _endpoints[endpoint.uid] = endpoint;
    ++_endpoints_added;

    // Only register ready endpoints
    if (endpoint.ready && _register_callback) {
        try {
            seastar::net::inet_address inet_addr(endpoint.address);
            seastar::socket_address addr(inet_addr, endpoint.port);

            co_await _register_callback(backend_id, addr,
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
    auto backend_id = endpoint.to_backend_id();
    log_k8s.info("K8s endpoint removed: {} ({}:{}, backend_id={})",
                 endpoint.uid, endpoint.address, endpoint.port, backend_id);

    ++_endpoints_removed;

    // Clean up reverse map (only if this UID still owns the BackendId entry)
    auto rev_it = _backend_id_to_uid.find(backend_id);
    if (rev_it != _backend_id_to_uid.end() && rev_it->second == endpoint.uid) {
        _backend_id_to_uid.erase(rev_it);
    }

    // Drain the backend
    if (_drain_callback) {
        try {
            co_await _drain_callback(backend_id);
        } catch (const std::exception& e) {
            log_k8s.error("Failed to drain backend for {}: {}", uid, e.what());
        }
    }

    _endpoints.erase(it);
    co_return;
}

seastar::future<> K8sDiscoveryService::handle_endpoint_modified(const K8sEndpoint& endpoint) {
    auto backend_id = endpoint.to_backend_id();

    log_k8s.info("K8s endpoint modified: {} (ready={}, weight={}, priority={}, backend_id={})",
                 endpoint.uid, endpoint.ready, endpoint.weight, endpoint.priority, backend_id);

    // Keep reverse map consistent for the modify path
    _backend_id_to_uid[backend_id] = endpoint.uid;

    auto it = _endpoints.find(endpoint.uid);
    bool was_ready = it != _endpoints.end() && it->second.ready;

    _endpoints[endpoint.uid] = endpoint;

    if (endpoint.ready && !was_ready) {
        // Became ready - register
        if (_register_callback) {
            try {
                seastar::net::inet_address inet_addr(endpoint.address);
                seastar::socket_address addr(inet_addr, endpoint.port);

                co_await _register_callback(backend_id, addr,
                                             endpoint.weight, endpoint.priority);
            } catch (const std::exception& e) {
                log_k8s.error("Failed to register backend for {}: {}", endpoint.uid, e.what());
            }
        }
    } else if (!endpoint.ready && was_ready) {
        // Became not ready - drain
        if (_drain_callback) {
            try {
                co_await _drain_callback(backend_id);
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

                co_await _register_callback(backend_id, addr,
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
                const char* weight_str = ann[K8S_ANNOTATION_WEIGHT].GetString();
                auto weight_opt = parse_uint32(std::string_view(weight_str));
                if (weight_opt) {
                    if (*weight_opt > K8S_MAX_WEIGHT) {
                        log_k8s.warn("Annotation '{}' value '{}' exceeds maximum {} - clamping to max",
                                     K8S_ANNOTATION_WEIGHT, weight_str, K8S_MAX_WEIGHT);
                        base_weight = K8S_MAX_WEIGHT;
                    } else {
                        base_weight = *weight_opt;
                    }
                } else {
                    log_k8s.warn("Invalid '{}' annotation value '{}' - using default {}",
                                 K8S_ANNOTATION_WEIGHT, weight_str, base_weight);
                }
            }
            if (ann.HasMember(K8S_ANNOTATION_PRIORITY) && ann[K8S_ANNOTATION_PRIORITY].IsString()) {
                const char* priority_str = ann[K8S_ANNOTATION_PRIORITY].GetString();
                auto priority_opt = parse_uint32(std::string_view(priority_str));
                if (priority_opt) {
                    if (*priority_opt > K8S_MAX_PRIORITY) {
                        log_k8s.warn("Annotation '{}' value '{}' exceeds maximum {} - clamping to max",
                                     K8S_ANNOTATION_PRIORITY, priority_str, K8S_MAX_PRIORITY);
                        base_priority = K8S_MAX_PRIORITY;
                    } else {
                        base_priority = *priority_opt;
                    }
                } else {
                    log_k8s.warn("Invalid '{}' annotation value '{}' - using default {}",
                                 K8S_ANNOTATION_PRIORITY, priority_str, base_priority);
                }
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

    // Resolve host using DNS-aware resolver with retry and graceful degradation
    seastar::socket_address addr = co_await resolve_api_server(host, port);

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

        // Rule #4: Bound buffer between newlines to prevent Slowloris-style OOM
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (!line.empty() && line != "\r") {
                keep_going = co_await on_event(line);
                if (!keep_going) break;
            }
        }

        // If no newline found and buffer exceeds MAX_LINE_SIZE, abort watch
        if (buffer.size() > K8S_MAX_LINE_SIZE) {
            ++_line_size_exceeded;
            log_k8s.error("K8s watch buffer exceeds MAX_LINE_SIZE ({} bytes) without newline - "
                          "aborting watch to prevent OOM (possible Slowloris attack)",
                          K8S_MAX_LINE_SIZE);
            break;
        }
    }

    co_await out.close();
    co_await in.close();
}

}  // namespace ranvier
