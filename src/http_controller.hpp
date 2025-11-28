#pragma once

#include "router_service.hpp"
#include "tokenizer_service.hpp"

#include <seastar/http/httpd.hh>

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

    // Helper handlers
    seastar::future<std::unique_ptr<seastar::httpd::reply>> handle_broadcast(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep);
};

} // namespace ranvier
