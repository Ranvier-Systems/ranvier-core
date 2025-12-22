#include "http_controller.hpp"
#include "logging.hpp"

#include "stream_parser.hpp"

#include <algorithm>

#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/net/api.hh>

using namespace seastar;

namespace ranvier {

// Custom exception for request timeouts
class request_timeout_error : public std::runtime_error {
public:
    request_timeout_error() : std::runtime_error("Request timed out") {}
};

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
// PROXY HANDLER
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

    // 2. Setup Streaming with Timeout
    // Capture timeout config for the lambda
    auto connect_timeout = _config.connect_timeout;
    auto request_timeout = _config.request_timeout;

    rep->write_body("text/event-stream", [this, target_addr, body, tokens, route_hit, target_id, connect_timeout, request_timeout](output_stream<char> client_out) -> future<> {

        // Calculate request deadline
        auto request_deadline = lowres_clock::now() + request_timeout;

        ConnectionBundle bundle;
        bool timed_out = false;
        bool connection_failed = false;

        // 1. GET CONNECTION FROM POOL (with connect timeout)
        auto connect_deadline = lowres_clock::now() + connect_timeout;
        auto conn_future = with_timeout(connect_deadline, _pool.get(target_addr));

        bundle = co_await std::move(conn_future).handle_exception([&](auto ep) {
            log_proxy.warn("Connect timeout or error to backend");
            connection_failed = true;
            return ConnectionBundle{}; // Return empty bundle
        });

        if (connection_failed) {
            sstring error_msg = "data: {\"error\": \"Connect timeout\"}\n\n";
            co_await client_out.write(error_msg);
            co_await client_out.flush();
            co_await client_out.close();
            co_return;
        }

        // 2. SEND REQUEST (with timeout check)
        if (lowres_clock::now() >= request_deadline) {
            log_proxy.warn("Request timeout before sending");
            bundle.is_valid = false;
            _pool.put(std::move(bundle));
            sstring error_msg = "data: {\"error\": \"Request timed out\"}\n\n";
            co_await client_out.write(error_msg);
            co_await client_out.flush();
            co_await client_out.close();
            co_return;
        }

        sstring http_req =
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + to_sstring(body.size()) + "\r\n"
            "Connection: keep-alive\r\n\r\n" +
            body;

        co_await bundle.out.write(http_req);
        co_await bundle.out.flush();

        // 3. The Read Loop (with timeout)
        StreamParser parser;
        while (true) {
            // Check request timeout before each read
            if (lowres_clock::now() >= request_deadline) {
                log_proxy.warn("Request timeout after {}s", request_timeout.count());
                timed_out = true;
                bundle.is_valid = false;
                break;
            }

            // Read with per-chunk timeout (use remaining time, capped at 30s per read)
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(request_deadline - lowres_clock::now());
            auto read_timeout = std::min(remaining, std::chrono::seconds(30));
            auto read_deadline = lowres_clock::now() + read_timeout;

            bool read_failed = false;
            auto read_future = with_timeout(read_deadline, bundle.in.read());
            temporary_buffer<char> chunk = co_await std::move(read_future).handle_exception([&](auto ep) {
                log_proxy.warn("Read timeout waiting for backend response");
                read_failed = true;
                return temporary_buffer<char>{}; // Return empty buffer
            });

            if (read_failed) {
                timed_out = true;
                bundle.is_valid = false;
                break;
            }

            // EOF logic
            if (chunk.empty()) {
                bundle.is_valid = false;
                break;
            }

            auto res = parser.push(std::move(chunk));

            // Snooping Logic
            if (res.header_snoop_success && !route_hit.has_value() && tokens.size() >= _config.min_token_length) {
                (void)_router.learn_route_global(tokens, target_id);
                log_router.info("Learned route: {} tokens -> GPU-{}", tokens.size(), target_id);

                if (_persistence) {
                    _persistence->save_route(tokens, target_id);
                }
            }

            if (!res.data.empty()) {
                co_await client_out.write(res.data);
                co_await client_out.flush();
            }

            if (res.done) {
                break;
            }
        }

        // Send timeout error to client if needed
        if (timed_out) {
            sstring error_msg = "data: {\"error\": \"Request timed out\"}\n\n";
            co_await client_out.write(error_msg);
            co_await client_out.flush();
        }

        // 4. CLEANUP
        _pool.put(std::move(bundle));
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
