// Ranvier Core - Local Backend Discovery Implementation

#include "local_discovery.hpp"

#include <fmt/format.h>

#include <rapidjson/document.h>

#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

LocalDiscoveryService::LocalDiscoveryService(BackendRegistry& registry, Config config)
    : _registry(registry),
      _config(std::move(config)) {
}

seastar::future<> LocalDiscoveryService::start() {
    // Register metrics (Rule #6: deregister in stop() before member destruction)
    namespace sm = seastar::metrics;
    _metrics.add_group("ranvier", {
        sm::make_counter("discovery_probes_total", _probes_sent,
            sm::description("Total number of local discovery probes sent")),
        sm::make_counter("discovery_probes_success", _probes_succeeded,
            sm::description("Total number of successful local discovery probes")),
        sm::make_counter("discovery_probes_failed", _probes_failed,
            sm::description("Total number of failed local discovery probes")),
        sm::make_gauge("discovery_backends_active", [this] { return _known_backends.size(); },
            sm::description("Current number of discovered local backends")),
        sm::make_counter("discovery_backends_added_total", _backends_added,
            sm::description("Total number of local backends added")),
        sm::make_counter("discovery_backends_removed_total", _backends_removed,
            sm::description("Total number of local backends removed")),
        sm::make_counter("discovery_overflow_drops", _overflow_drops,
            sm::description("Times backend discovery was skipped due to MAX_KNOWN_BACKENDS limit")),
    });

    _running = true;
    // Store the loop future for clean shutdown tracking (Rule #5)
    _loop_future = discovery_loop();
    co_return;
}

seastar::future<> LocalDiscoveryService::stop() {
    // Rule #6: Deregister metrics first before any member destruction
    _metrics.clear();

    _running = false;
    // Abort the sleep so the loop exits promptly instead of waiting up to scan_interval
    _as.request_abort();
    // Close the gate (signals loop to exit and waits for in-flight probes)
    co_await _gate.close();
    // Await the loop future to ensure clean exit
    co_await std::move(_loop_future);
}

seastar::future<> LocalDiscoveryService::discovery_loop() {
    if (seastar::this_shard_id() != 0) co_return;  // Shard 0 only

    auto holder = _gate.hold();

    try {
        while (_running) {
            std::vector<DiscoveredBackend> results;

            // Probe all configured ports in parallel
            co_await seastar::max_concurrent_for_each(
                _config.discovery_ports, MAX_CONCURRENT_PROBES,
                [this, &results](uint16_t port) -> seastar::future<> {
                    _probes_sent++;
                    auto result = co_await probe_port(port);
                    if (result) {
                        _probes_succeeded++;
                        results.push_back(std::move(*result));
                    } else {
                        _probes_failed++;
                    }
                });

            // Reconcile with router
            co_await reconcile(std::move(results));

            co_await seastar::sleep_abortable(_config.scan_interval, _as);
        }
    } catch (const seastar::sleep_aborted&) {
        log_discovery.debug("Discovery loop exiting: sleep aborted");
    } catch (const seastar::gate_closed_exception&) {
        log_discovery.debug("Discovery loop exiting: gate closed");
    } catch (const std::exception& e) {
        log_discovery.warn("Discovery loop exiting unexpectedly: {}", e.what());
    } catch (...) {
        log_discovery.warn("Discovery loop exiting: unknown exception");
    }
}

seastar::future<std::optional<seastar::sstring>> LocalDiscoveryService::http_get_local(
        uint16_t port, seastar::sstring path,
        std::chrono::milliseconds timeout) {
    auto deadline = seastar::lowres_clock::now() + timeout;

    try {
        // 1. Connect with connect_timeout to 127.0.0.1:port
        seastar::socket_address addr(seastar::net::inet_address("127.0.0.1"), port);
        auto conn = co_await seastar::with_timeout(
            seastar::lowres_clock::now() + _config.connect_timeout,
            seastar::connect(addr));

        auto out = conn.output();
        auto in = conn.input();

        // Use exception_ptr to close streams outside catch (co_await not allowed in handlers)
        std::exception_ptr ep;
        seastar::sstring response_data;

        try {
            // 2. Write raw HTTP/1.1 GET request
            auto request = fmt::format(
                "GET {} HTTP/1.1\r\nHost: localhost:{}\r\nConnection: close\r\n\r\n",
                path, port);
            co_await out.write(request);
            co_await out.flush();

            // 3. Read response with probe_timeout deadline
            static constexpr size_t MAX_RESPONSE_SIZE = 65536;  // 64KB limit (Rule #4)

            while (response_data.size() < MAX_RESPONSE_SIZE) {
                auto buf = co_await seastar::with_timeout(deadline, in.read());
                if (buf.empty()) break;  // EOF
                response_data.append(buf.get(), buf.size());
            }
        } catch (...) {
            ep = std::current_exception();
        }

        // 4. Close streams in all paths (success, failure, timeout)
        try { co_await out.close(); } catch (...) {
            log_discovery.debug("Port {} stream close failed on output", port);
        }
        try { co_await in.close(); } catch (...) {
            log_discovery.debug("Port {} input stream close failed", port);
        }

        if (ep) {
            std::rethrow_exception(ep);
        }

        // 5. Parse HTTP status — look for "HTTP/1.1 200" or "HTTP/1.0 200"
        static constexpr size_t MIN_STATUS_LINE_LEN = 12;  // "HTTP/1.x 200"
        static constexpr const char HEADER_BODY_SEPARATOR[] = "\r\n\r\n";
        static constexpr size_t HEADER_BODY_SEPARATOR_LEN = sizeof(HEADER_BODY_SEPARATOR) - 1;

        if (response_data.size() < MIN_STATUS_LINE_LEN) {
            co_return std::nullopt;
        }

        auto status_end = response_data.find("\r\n");
        if (status_end == seastar::sstring::npos) {
            co_return std::nullopt;
        }

        auto status_line = response_data.substr(0, status_end);
        if (status_line.find("200") == seastar::sstring::npos) {
            log_discovery.debug("Port {} returned non-200: {}", port, status_line);
            co_return std::nullopt;
        }

        // 6. Extract body (after \r\n\r\n)
        auto body_start = response_data.find(HEADER_BODY_SEPARATOR);
        if (body_start == seastar::sstring::npos) {
            co_return std::nullopt;
        }

        co_return response_data.substr(body_start + HEADER_BODY_SEPARATOR_LEN);
    } catch (const seastar::timed_out_error&) {
        log_discovery.debug("Port {} probe timed out", port);
        co_return std::nullopt;
    } catch (const std::system_error& e) {
        log_discovery.debug("Port {} probe connection error: {}", port, e.what());
        co_return std::nullopt;
    } catch (const std::exception& e) {
        log_discovery.debug("Port {} probe failed: {}", port, e.what());
        co_return std::nullopt;
    }
}

seastar::future<std::optional<DiscoveredBackend>> LocalDiscoveryService::probe_port(uint16_t port) {
    // 1. Send GET /v1/models with probe_timeout
    auto body = co_await http_get_local(port, "/v1/models", _config.probe_timeout);
    if (!body) {
        co_return std::nullopt;
    }

    // 2. Parse the response
    auto backend = parse_models_response(port, *body);
    co_return std::make_optional(std::move(backend));
}

DiscoveredBackend LocalDiscoveryService::parse_models_response(
        uint16_t port, const seastar::sstring& body) {
    DiscoveredBackend backend;
    backend.port = port;
    backend.last_seen = std::chrono::steady_clock::now();

    // Parse JSON with RapidJSON
    rapidjson::Document doc;
    doc.Parse(body.c_str(), body.size());

    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("data") && doc["data"].IsArray()) {
        const auto& data = doc["data"];
        size_t model_count = 0;
        for (rapidjson::SizeType i = 0; i < data.Size() && model_count < DiscoveredBackend::MAX_MODELS; ++i) {
            if (data[i].IsObject() && data[i].HasMember("id") && data[i]["id"].IsString()) {
                // Rule #7 equivalent: null-guard C string from RapidJSON
                const char* model_id = data[i]["id"].GetString();
                if (model_id) {
                    backend.available_models.emplace_back(model_id);
                    ++model_count;
                }
            }
        }
    }

    // Detect server type using parsed models (avoids re-parsing JSON)
    backend.server_type = detect_server_type(port, body, backend.available_models);

    return backend;
}

std::string LocalDiscoveryService::detect_server_type(uint16_t port, const seastar::sstring& body,
        const std::vector<std::string>& models) {
    // Check response body for distinctive markers first
    if (body.find("ollama") != seastar::sstring::npos) {
        return "ollama";
    }

    // Check if any model ID contains ":" (like "llama3:8b") — likely Ollama
    for (const auto& model : models) {
        if (model.find(':') != std::string::npos) {
            return "ollama";
        }
    }

    // Check for vLLM markers
    if (port == local_ports::VLLM && (body.find("vllm") != seastar::sstring::npos ||
                                       body.find("model_permission") != seastar::sstring::npos)) {
        return "vllm";
    }

    // Port-based defaults
    switch (port) {
        case local_ports::OLLAMA:     return "ollama";
        case local_ports::LMSTUDIO:   return "lmstudio";
        case local_ports::LLAMACPP:   return "llamacpp";
        case local_ports::TEXTGENUI:  return "textgenui";
        case local_ports::LOCALAI:    return "localai";
        default:                      return "unknown";
    }
}

seastar::future<> LocalDiscoveryService::reconcile(
        std::vector<DiscoveredBackend> discovered) {
    // Build set of ports that responded successfully
    absl::flat_hash_map<uint16_t, DiscoveredBackend*> discovered_by_port;
    for (auto& d : discovered) {
        discovered_by_port[d.port] = &d;
    }

    // 1. Check for disappeared backends (in _known_backends but not in results)
    std::vector<uint16_t> to_remove;
    for (auto& [port, known] : _known_backends) {
        auto it = discovered_by_port.find(port);
        if (it == discovered_by_port.end()) {
            // Backend missed this probe
            known.consecutive_failures++;
            if (known.consecutive_failures >= REMOVAL_THRESHOLD) {
                to_remove.push_back(port);
            }
        } else {
            // Backend still healthy — reset failure counter, update state
            known.consecutive_failures = 0;
            known.last_seen = it->second->last_seen;

            // Check if models changed
            if (known.available_models != it->second->available_models) {
                log_discovery.info("Backend on port {} models changed: {} -> {}",
                    port, known.available_models.size(), it->second->available_models.size());
                known.available_models = std::move(it->second->available_models);
            }
        }
    }

    // Remove disappeared backends
    for (auto port : to_remove) {
        auto it = _known_backends.find(port);
        if (it != _known_backends.end()) {
            auto id = it->second.id;
            co_await _registry.unregister_backend_global(id);
            log_discovery.info("Backend on port {} disappeared, unregistered (id={})", port, id);
            _known_backends.erase(it);
            _backends_removed++;
        }
    }

    // 2. Check for new backends (in results but not in _known_backends)
    auto effective_limit = std::min(_config.max_backends, MAX_KNOWN_BACKENDS);
    for (auto& d : discovered) {
        if (_known_backends.contains(d.port)) {
            continue;  // Already tracked
        }

        // Check capacity limit (configurable, hard-capped by MAX_KNOWN_BACKENDS)
        if (_known_backends.size() >= effective_limit) {
            _overflow_drops++;
            log_discovery.warn("Backend limit ({}) reached, dropping backend on port {}",
                effective_limit, d.port);
            continue;
        }

        // Assign ID and register
        d.id = _next_backend_id++;
        seastar::socket_address addr(seastar::net::inet_address("127.0.0.1"), d.port);
        co_await _registry.register_backend_global(d.id, addr, 100, 0);

        // Build models string for logging (truncate for readability)
        std::string models_str;
        for (size_t i = 0; i < d.available_models.size() && i < MAX_LOG_MODELS; ++i) {
            if (i > 0) models_str += ", ";
            models_str += d.available_models[i];
        }
        if (d.available_models.size() > MAX_LOG_MODELS) {
            models_str += fmt::format(", ... (+{})", d.available_models.size() - MAX_LOG_MODELS);
        }

        log_discovery.info("Discovered {} on port {} (id={}, models: [{}])",
            d.server_type, d.port, d.id, models_str);

        _known_backends[d.port] = std::move(d);
        _backends_added++;
    }
}

}  // namespace ranvier
