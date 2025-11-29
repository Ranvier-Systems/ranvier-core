#include "http_controller.hpp"

#include <iostream>

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

    // 2. CONTROL PLANE
    r.add(operation_type::POST, url("/admin/routes"), new async_handler([this](auto req, auto rep) {
        return this->handle_broadcast_route(std::move(req), std::move(rep));
    }));

    r.add(operation_type::POST, url("/admin/backends"), new async_handler([this](auto req, auto rep) {
        return this->handle_broadcast_backend(std::move(req), std::move(rep));
    }));
}

// ---------------------------------------------------------
// PROXY HANDLER (Raw TCP Implementation)
// ---------------------------------------------------------
future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_proxy(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    std::string body = req->content;

    // 1. Validation
    if (!_tokenizer.is_loaded()) {
        rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    auto tokens = _tokenizer.encode(body);
    auto route_hit = _router.lookup(tokens); // Did we find a route?

    BackendId target_id;

    if (route_hit.has_value()) {
        target_id = route_hit.value(); // Cache Hit
    } else {
        // Cache Miss: Pick a random backend so we can start the work
        auto random_id = _router.get_random_backend();
        if (!random_id.has_value()) {
            rep->write_body("json", "{\"error\": \"No backends registered!\"}");
            return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
        }
        target_id = random_id.value();
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
                // Check for HTTP 200 OK (Simple check)
                //bool is_success = raw_response.find("HTTP/1.1 200") != sstring::npos;

                // ---------------------------------------------------------
                // SNOOPING LOGIC
                // ---------------------------------------------------------
                if (is_success && !route_hit.has_value()) {
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

    return _router.learn_route_global(tokens, backend_id).then([backend_id, rep = std::move(rep)]() mutable {
         rep->write_body("json", "{\"status\": \"ok\", \"route_added\": " + std::to_string(backend_id) + "}");
         return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_broadcast_backend(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    sstring id_str = req->get_query_param("id");
    sstring port_str = req->get_query_param("port");

    if (id_str.empty() || port_str.empty()) {
        rep->write_body("json", "{\"error\": \"Missing id or port\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int id = std::stoi(std::string(id_str));
    int port = std::stoi(std::string(port_str));

    socket_address addr(ipv4_addr("127.0.0.1", port));

    return _router.register_backend_global(id, addr).then([id, port, rep = std::move(rep)]() mutable {
        std::cout << "[Control Plane] Registered Backend " << id << " -> 127.0.0.1:" << port << "\n";
        rep->write_body("json", "{\"status\": \"ok\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

} // namespace ranvier
