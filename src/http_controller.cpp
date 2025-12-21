#include "http_controller.hpp"
#include "logging.hpp"

#include "stream_parser.hpp"

#include <algorithm>

#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/net/api.hh>

using namespace seastar;

namespace ranvier {

// Helper: explicit seastar::httpd:: (Server) types
template <typename Func>
struct async_handler : public seastar::httpd::handler_base {
    Func _func;
    async_handler(Func&& f) : _func(std::move(f)) {}
    future<std::unique_ptr<seastar::httpd::reply>> handle(const sstring& path, std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) override {
        return _func(std::move(req), std::move(rep));
    }
};

void HttpController::register_routes(seastar::httpd::routes& r) {
    using namespace seastar::httpd;

    // 1. DATA PLANE
    r.add(operation_type::POST, url("/v1/chat/completions"), new async_handler([this](auto req, auto rep) {
        return this->handle_proxy(std::move(req), std::move(rep));
    }));

    // 2. CONTROL PLANE - Create/Update
    r.add(operation_type::POST, url("/admin/routes"), new async_handler([this](auto req, auto rep) {
        return this->handle_broadcast_route(std::move(req), std::move(rep));
    }));

    r.add(operation_type::POST, url("/admin/backends"), new async_handler([this](auto req, auto rep) {
        return this->handle_broadcast_backend(std::move(req), std::move(rep));
    }));

    // 3. CONTROL PLANE - Delete
    r.add(operation_type::DELETE, url("/admin/backends"), new async_handler([this](auto req, auto rep) {
        return this->handle_delete_backend(std::move(req), std::move(rep));
    }));

    r.add(operation_type::DELETE, url("/admin/routes"), new async_handler([this](auto req, auto rep) {
        return this->handle_delete_routes(std::move(req), std::move(rep));
    }));

    r.add(operation_type::POST, url("/admin/clear"), new async_handler([this](auto req, auto rep) {
        return this->handle_clear_all(std::move(req), std::move(rep));
    }));
}

// ---------------------------------------------------------
// PROXY HANDLER (Raw TCP Implementation)
// ---------------------------------------------------------
/*
future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_proxy(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    std::string body = req->content;

    // 1. Validation
    if (!_tokenizer.is_loaded()) {
        rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    auto tokens = _tokenizer.encode(body);
    std::cout << "Tokens size " << tokens.size() << "\n";

    auto route_hit = _router.lookup(tokens); // Did we find a route?

    BackendId target_id;

    if (route_hit.has_value()) {
        target_id = route_hit.value(); // Cache Hit
        std::cout << "Cache Hit - Target ID=" << target_id << "\n";
    } else {
        // Cache Miss: Pick a random backend so we can start the work
        auto random_id = _router.get_random_backend();
        if (!random_id.has_value()) {
            rep->write_body("json", "{\"error\": \"No backends registered!\"}");
            return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
        }
        target_id = random_id.value();
        std::cout << "Cache Miss - Random Target ID=" << target_id << "\n";
    }

    auto target_addr = _router.get_backend_address(target_id);
    if (!target_addr.has_value()) {
        rep->write_body("json", "{\"error\": \"Backend ID found, but no IP address registered!\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // 2. Connect via Raw TCP (Stable API)
    return seastar::connect(target_addr.value()).then([this, tokens, route_hit, target_id, body, rep = std::move(rep)](connected_socket fd) mutable {
        auto out = fd.output();
        auto in = fd.input();

        // 3. Manually construct HTTP Request
        // This bypasses all the enum/namespace errors.
        sstring http_req =
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: mock-gpu\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + to_sstring(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" +
            body;

        // 4. Send & Receive
        return do_with(std::move(fd), std::move(out), std::move(in), std::move(http_req),
            [this, tokens, route_hit, target_id, rep = std::move(rep)](auto& fd, auto& out, auto& in, auto& http_req) mutable {

            return out.write(http_req).then([&out] {
                return out.flush();
            }).then([&in] {

                // Read Loop (Handle Fragmentation)
                // We use a separate do_with for the accumulator string
                return do_with(sstring(), [&in](auto& full_response) {
                    return repeat([&in, &full_response] {
                        return in.read().then([&full_response](temporary_buffer<char> buf) {
                            if (buf.empty()) {
                                return stop_iteration::yes; // EOF
                            }
                            full_response += sstring(buf.get(), buf.size());
                            return stop_iteration::no;
                        });
                    }).then([&full_response] {
                        return full_response; // Return the full accumulated string
                    });
                });
            }).then([this, tokens, route_hit, target_id, rep = std::move(rep)](sstring raw_response) mutable {
                // Looser Snooping Check
                // We just look for " 200 " to allow HTTP/1.0 or HTTP/1.1
                bool is_success = raw_response.find(" 200 ") != sstring::npos;

                // Added Min-Length check (e.g., 4 tokens)
                // In production, this should be configurable (e.g. 64)
                const size_t kMinLengthThreshold = 4;
                bool is_worth_caching = tokens.size() >= kMinLengthThreshold;
                if (!is_worth_caching) {
                    std::cout << "Not caching tokens of size " << tokens.size() << ", minTokenLimit=" << kMinLengthThreshold << "\n";
                }

                // ---------------------------------------------------------
                // SNOOPING LOGIC
                // ---------------------------------------------------------
                if (is_success && !route_hit.has_value() && is_worth_caching) {
                    // Fire-and-forget: Teach all cores that this backend now owns these tokens.
                    // We don't wait for this future; we just let it run in the background.
                    // ONLY learn if it was a Miss previously
                    (void)_router.learn_route_global(tokens, target_id);
                    std::cout << "[Snoop] Passively learned route for GPU-" << target_id << "\n";
                }

                // 2. Return Response to User
                auto body_pos = raw_response.find("\r\n\r\n");
                if (body_pos != sstring::npos) {
                    rep->write_body("json", raw_response.substr(body_pos + 4));
                } else {
                    rep->write_body("json", raw_response);
                }

                return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
            });
        });
    });
}
*/

/*
// Helper Enum for the State Machine
enum class ProxyState {
    Headers,    // Waiting for \r\n\r\n
    ChunkSize,  // Waiting for "f1\r\n"
    ChunkData,  // Waiting for N bytes of data
    Done
};

// ---------------------------------------------------------
// STREAMING PROXY HANDLER
// ---------------------------------------------------------
future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_proxy(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    std::string body = req->content;

    if (!_tokenizer.is_loaded()) {
        rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    auto tokens = _tokenizer.encode(body);
    auto route_hit = _router.lookup(tokens);

    BackendId target_id;
    if (route_hit.has_value()) {
        target_id = route_hit.value();
    } else {
        auto random_id = _router.get_random_backend();
        if (!random_id.has_value()) {
            rep->write_body("json", "{\"error\": \"No backends registered!\"}");
            return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
        }
        target_id = random_id.value();
    }

    auto target_addr_opt = _router.get_backend_address(target_id);
    if (!target_addr_opt.has_value()) {
        rep->write_body("json", "{\"error\": \"Backend registered but no IP found\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }
    socket_address target_addr = target_addr_opt.value();

    // STREAMING RESPONSE
    rep->write_body("text/event-stream", [this, target_addr, body, tokens, route_hit, target_id](output_stream<char>& client_out) mutable {

        return seastar::connect(target_addr).then([this, body, tokens, route_hit, target_id, &client_out](connected_socket fd) mutable {
            auto backend_in = fd.input();
            auto backend_out = fd.output();

            sstring http_req =
                "POST /v1/chat/completions HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + to_sstring(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" +
                body;

            // STATE MACHINE:
            // We maintain a buffer ('accum') and a state ('state')
            // 'bytes_needed' tracks how much data we are waiting for in ChunkData mode
            return do_with(std::move(fd), std::move(backend_in), std::move(backend_out), std::move(http_req), sstring(), ProxyState::Headers, size_t(0),
                [this, tokens, route_hit, target_id, &client_out](auto& fd, auto& in, auto& out, auto& req_str, sstring& accum, ProxyState& state, size_t& bytes_needed) mutable {

                return out.write(req_str).then([&out] {
                    return out.flush();
                }).then([&in, &client_out, &accum, &state, &bytes_needed, this, tokens, route_hit, target_id] {

                    return repeat([&in, &client_out, &accum, &state, &bytes_needed, this, tokens, route_hit, target_id] {
                        return in.read().then([&client_out, &accum, &state, &bytes_needed, this, tokens, route_hit, target_id](temporary_buffer<char> chunk) {
                            if (chunk.empty()) return make_ready_future<stop_iteration>(stop_iteration::yes); // EOF

                            // Append new data to our persistent buffer
                            accum.append(chunk.get(), chunk.size());

                            // Process Buffer Loop
                            // We loop here because one read() might contain multiple chunks (Size+Data+Size...)
                            bool keep_processing = true;
                            while (keep_processing && !accum.empty()) {
                                keep_processing = false; // Default to stop unless we make progress

                                if (state == ProxyState::Headers) {
                                    auto header_end = accum.find("\r\n\r\n");
                                    if (header_end != sstring::npos) {
                                        // 1. Headers Found
                                        sstring headers = accum.substr(0, header_end);

                                        // Snooping
                                        if (headers.find(" 200 ") != sstring::npos && !route_hit.has_value() && tokens.size() >= 4) {
                                            (void)_router.learn_route_global(tokens, target_id);
                                            std::cout << "\n[Snoop] 🧠 LEARNED ROUTE: " << tokens.size() << " toks -> GPU-" << target_id << "\n";
                                        }

                                        // Remove headers from buffer
                                        accum = accum.substr(header_end + 4);
                                        state = ProxyState::ChunkSize;
                                        keep_processing = true;
                                    }
                                }
                                else if (state == ProxyState::ChunkSize) {
                                    auto line_end = accum.find("\r\n");
                                    if (line_end != sstring::npos) {
                                        // 2. Size Line Found (e.g. "f1\r\n")
                                        sstring size_line = accum.substr(0, line_end);

                                        // Parse Hex
                                        try {
                                            bytes_needed = std::stoul(size_line, nullptr, 16);
                                        } catch(...) { bytes_needed = 0; }

                                        // Consume line
                                        accum = accum.substr(line_end + 2);

                                        if (bytes_needed == 0) {
                                            state = ProxyState::Done;
                                            return make_ready_future<stop_iteration>(stop_iteration::yes);
                                        }

                                        state = ProxyState::ChunkData;
                                        keep_processing = true;
                                    }
                                }
                                else if (state == ProxyState::ChunkData) {
                                    // 3. Data Block (Need 'bytes_needed' + 2 for trailing \r\n)
                                    if (accum.size() >= bytes_needed + 2) {
                                        // We have the full chunk!
                                        sstring data = accum.substr(0, bytes_needed);

                                        // Consume data + \r\n
                                        accum = accum.substr(bytes_needed + 2);
                                        state = ProxyState::ChunkSize; // Back to size mode
                                        keep_processing = true;

                                        // Write clean data to user
                                        // IMPORTANT: We must wait for this write before looping again
                                        return client_out.write(data).then([&client_out] {
                                            return client_out.flush();
                                        }).then([] {
                                            return stop_iteration::no; // Continue outer repeat loop
                                        });
                                    }
                                }
                            }

                            return make_ready_future<stop_iteration>(stop_iteration::no);
                        });
                    });
                });
            });
        });
    });

    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}
*/
// ---------------------------------------------------------
// PROXY HANDLER (Coroutine Version)
// ---------------------------------------------------------
future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_proxy(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    std::string body = req->content;

    // 1. Validation
    if (!_tokenizer.is_loaded()) {
        rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
        co_return std::move(rep);
    }

    auto tokens = _tokenizer.encode(body);
    auto route_hit = _router.lookup(tokens);

    BackendId target_id;
    if (route_hit.has_value()) {
        target_id = route_hit.value();
    } else {
        auto random_id = _router.get_random_backend();
        if (!random_id.has_value()) {
            rep->write_body("json", "{\"error\": \"No backends registered!\"}");
            co_return std::move(rep);
        }
        target_id = random_id.value();
    }

    auto target_addr_opt = _router.get_backend_address(target_id);
    if (!target_addr_opt.has_value()) {
        rep->write_body("json", "{\"error\": \"Backend IP not found\"}");
        co_return std::move(rep);
    }
    socket_address target_addr = target_addr_opt.value();

    // 2. Setup Streaming
    // STREAMING RESPONSE
    rep->write_body("text/event-stream", [this, target_addr, body, tokens, route_hit, target_id](output_stream<char> client_out) -> future<> {

        // 1. GET CONNECTION FROM POOL
        // Note: We declare bundle outside try/catch so we can return it later
        ConnectionBundle bundle;

        try {
            // Use co_await to get a connection (either new or reused)
            bundle = co_await _pool.get(target_addr);

            sstring http_req =
                "POST /v1/chat/completions HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + to_sstring(body.size()) + "\r\n"
                "Connection: keep-alive\r\n\r\n" +  // <--- CHANGED: keep-alive
                body;

            co_await bundle.out.write(http_req);
            co_await bundle.out.flush();

            // 3. The Read Loop
            StreamParser parser;
            while (true) {
                // Read from the POOLED input stream
                auto chunk = co_await bundle.in.read();

                // EOF logic:
                // If chunk is empty, the server closed on us.
                // This is fine for 'Connection: close', but for keep-alive,
                // we actually rely on Content-Length or Chunked Encoding to know when to stop.
                // Since we are parsing Raw TCP, and Ollama might use Chunked,
                // our StreamParser logic needs to be robust.
                if (chunk.empty()) {
                    bundle.is_valid = false; // Server hung up, discard this connection
                    break;
                }

                auto res = parser.push(std::move(chunk));

                // Snooping Logic
                if (res.header_snoop_success && !route_hit.has_value() && tokens.size() >= _config.min_token_length) {
                    (void)_router.learn_route_global(tokens, target_id);
                    log_router.info("Learned route: {} tokens -> GPU-{}", tokens.size(), target_id);

                    // Persist the learned route
                    if (_persistence) {
                        _persistence->save_route(tokens, target_id);
                    }
                }

                if (!res.data.empty()) {
                    co_await client_out.write(res.data);
                    co_await client_out.flush();
                }

                if (res.done) {
                    // Success! We finished reading this specific request.
                    // We can stop reading from the socket now.
                    break;
                }
            }
        } catch (const std::exception& e) {
            log_proxy.error("Proxy error: {}", e.what());
            bundle.is_valid = false; // Mark broken so we don't reuse it
        }

        // 4. CLEANUP
        // Return backend connection to pool (or close if invalid)
        _pool.put(std::move(bundle));

        // Always close the USER connection
        co_await client_out.close();
    });

    co_return std::move(rep);
}

// ---------------------------------------------------------
// CONTROL PLANE HANDLERS
// ---------------------------------------------------------

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_broadcast_route(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    sstring id_str = req->get_query_param("backend_id");
    if (id_str.empty()) {
        rep->write_body("json", "{\"error\": \"Missing backend_id\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int backend_id = std::stoi(std::string(id_str));
    auto tokens = _tokenizer.encode(req->content);

    // Persist the route
    if (_persistence) {
        _persistence->save_route(tokens, backend_id);
    }

    return _router.learn_route_global(tokens, backend_id).then([backend_id, rep = std::move(rep)]() mutable {
         rep->write_body("json", "{\"status\": \"ok\", \"route_added\": " + std::to_string(backend_id) + "}");
         return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_broadcast_backend(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: POST /admin/backends?id=1&ip=192.168.4.51&port=11434
    sstring id_str = req->get_query_param("id");
    sstring ip_str = req->get_query_param("ip");
    sstring port_str = req->get_query_param("port");

    // Check for IP parameter
    if (id_str.empty() || port_str.empty() || ip_str.empty()) {
        rep->write_body("json", "{\"error\": \"Missing id, ip, or port\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int id = std::stoi(std::string(id_str));
    int port = std::stoi(std::string(port_str));

    // Use the provided IP string
    socket_address addr(ipv4_addr(std::string(ip_str), port));

    // Persist the backend registration
    if (_persistence) {
        _persistence->save_backend(id, std::string(ip_str), static_cast<uint16_t>(port));
    }

    return _router.register_backend_global(id, addr).then([id, ip_str, port, rep = std::move(rep)]() mutable {
        log_control.info("Registered Backend {} -> {}:{}", id, ip_str, port);
        rep->write_body("json", "{\"status\": \"ok\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

// ---------------------------------------------------------
// ADMIN DELETE HANDLERS
// ---------------------------------------------------------

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_delete_backend(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: DELETE /admin/backends?id=1
    sstring id_str = req->get_query_param("id");

    if (id_str.empty()) {
        rep->write_body("json", "{\"error\": \"Missing id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int id = std::stoi(std::string(id_str));

    // Remove from persistence first
    if (_persistence) {
        _persistence->remove_routes_for_backend(id);
        _persistence->remove_backend(id);
    }

    // Remove from in-memory state across all shards
    return _router.unregister_backend_global(id).then([id, rep = std::move(rep)]() mutable {
        log_control.info("Deleted Backend {} and its persisted routes", id);
        rep->write_body("json", "{\"status\": \"ok\", \"backend_deleted\": " + std::to_string(id) + "}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_delete_routes(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: DELETE /admin/routes?backend_id=1
    sstring id_str = req->get_query_param("backend_id");

    if (id_str.empty()) {
        rep->write_body("json", "{\"error\": \"Missing backend_id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int backend_id = std::stoi(std::string(id_str));

    // Remove routes from persistence
    if (_persistence) {
        _persistence->remove_routes_for_backend(backend_id);
    }

    // Note: In-memory routes in the RadixTree are not removed immediately.
    // They will be gone after restart. For full removal, we'd need to
    // traverse the entire tree which is expensive.
    log_control.info("Deleted persisted routes for Backend {} (in-memory routes cleared on restart)", backend_id);
    rep->write_body("json", "{\"status\": \"ok\", \"routes_deleted_for_backend\": " + std::to_string(backend_id) + ", \"note\": \"In-memory routes will be cleared on restart\"}");
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_clear_all(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: POST /admin/clear
    // WARNING: This is destructive!

    if (_persistence) {
        _persistence->clear_all();
    }

    log_control.warn("Cleared all persisted data (backends and routes). Restart required to clear in-memory state.");
    rep->write_body("json", "{\"status\": \"ok\", \"warning\": \"All persisted data cleared. Restart to clear in-memory state.\"}");
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

} // namespace ranvier
