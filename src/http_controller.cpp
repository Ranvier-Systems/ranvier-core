#include "http_controller.hpp"

#include <iostream>

#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>

using namespace seastar;
using namespace seastar::httpd;

namespace ranvier {

// Helper for async handlers (same as before)
template <typename Func>
struct async_handler : public handler_base {
    Func _func;
    async_handler(Func&& f) : _func(std::move(f)) {}
    future<std::unique_ptr<reply>> handle(const sstring& path, std::unique_ptr<request> req, std::unique_ptr<reply> rep) override {
        return _func(std::move(req), std::move(rep));
    }
};

void HttpController::register_routes(routes& r) {
    // 1. DATA PLANE (Sync)
    r.add(operation_type::POST, url("/v1/chat/completions"), new function_handler([this](const_req req) {
        std::string body = req.content;

        if (!_tokenizer.is_loaded()) return std::string("{\"error\": \"Tokenizer not loaded\"}");

        auto tokens = _tokenizer.encode(body);
        auto backend_id = _router.lookup(tokens);

        std::string message;
        if (backend_id.has_value()) {
            message = "⚡ CACHE HIT! Routed to GPU-" + std::to_string(backend_id.value());
        } else {
            message = "🐢 Cache Miss. Routing to random GPU.";
        }
        return "{\"choices\":[{\"message\":{\"content\":\"" + message + "\"}}]}";
    }));

    // 2. CONTROL PLANE (Async)
    r.add(operation_type::POST, url("/admin/routes"), new async_handler([this](auto req, auto rep) {
        return this->handle_broadcast(std::move(req), std::move(rep));
    }));
}

future<std::unique_ptr<reply>> HttpController::handle_broadcast(std::unique_ptr<request> req, std::unique_ptr<reply> rep) {
    sstring id_str = req->get_query_param("backend_id");
    if (id_str.empty() || !_tokenizer.is_loaded()) {
        rep->write_body("json", "{\"error\": \"Bad Request\"}");
        return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
    }

    int backend_id = std::stoi(std::string(id_str));
    std::string prefix_text = req->content;
    auto tokens = _tokenizer.encode(prefix_text);

    return _router.learn_route_global(tokens, backend_id).then([backend_id, rep = std::move(rep)]() mutable {
         std::cout << "[Control Plane] Route added for GPU " << backend_id << "\n";
         rep->write_body("json", "{\"status\": \"ok\", \"backend\": " + std::to_string(backend_id) + "}");
         return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
    });
}

} // namespace ranvier
