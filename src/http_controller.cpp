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
    auto backend_id = _router.lookup(tokens);

    if (!backend_id.has_value()) {
        rep->write_body("json", "{\"error\": \"🐢 Cache Miss (No route found)\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    auto target_addr = _router.get_backend_address(backend_id.value());
    if (!target_addr.has_value()) {
        rep->write_body("json", "{\"error\": \"Backend ID found, but no IP address registered!\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // 2. Connect via Raw TCP (Stable API)
    return seastar::connect(target_addr.value()).then([body, rep = std::move(rep)](connected_socket fd) mutable {
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
            [rep = std::move(rep)](auto& fd, auto& out, auto& in, auto& http_req) mutable {

            return out.write(http_req).then([&out] {
                return out.flush();
            }).then([&in] {
                // Read the response until EOF (simplified for MVP)
                return in.read();
            }).then([rep = std::move(rep)](temporary_buffer<char> buf) mutable {
                // 5. Parse the Response (Naive)
                // In a real app, you'd parse headers. For MVP, we just find the JSON body.
                sstring raw_response(buf.get(), buf.size());

                // Find where headers end (\r\n\r\n)
                auto body_pos = raw_response.find("\r\n\r\n");
                if (body_pos != sstring::npos) {
                    sstring json_body = raw_response.substr(body_pos + 4);
                    rep->write_body("json", json_body);
                } else {
                    rep->write_body("json", raw_response); // Fallback
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
