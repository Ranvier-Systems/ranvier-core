#pragma once

#include "connection_pool.hpp"
#include "router_service.hpp"
#include "tokenizer_service.hpp"

#include <seastar/http/httpd.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>

namespace ranvier {

class HttpController {
public:
    HttpController(TokenizerService& t, RouterService& r)
        : _tokenizer(t), _router(r) {}

    // Register all endpoints
    void register_routes(seastar::httpd::routes& r);

private:
    TokenizerService& _tokenizer;
    RouterService& _router;
    ConnectionPool _pool;

    // Helper handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_proxy(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_route(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
};

} // namespace ranvier
