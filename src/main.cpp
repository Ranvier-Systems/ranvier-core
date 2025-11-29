/*
// Demo 1 - Run seastar
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <iostream>
#include <tokenizers_cpp.h> // Prove we linked the Rust library correctly

seastar::future<> run() {
    std::cout << "⚡ Ranvier System: Online on " << seastar::smp::count << " cores.\n";
    std::cout << "✅ Seastar Initialized.\n";
    std::cout << "✅ Tokenizer Library Linked.\n";
    return seastar::make_ready_future<>();
}

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, run);
}
*/

#if 0
// Demo 2 - Integrate with RadixTree
// This version does 3 new things:
// - Instantiates a RadixTree on every core.
// - "Learns" a route at startup (mapping a "System Prompt" to a specific Backend ID).
// - Performs a Lookup on incoming requests to see if they match.
//
// Testing:
// Once you build (ninja) and run (./ranvier_server):
// 1. Test a Cache Miss (Random Input)
// curl -X POST -d "Hello world" http://localhost:8080/v1/chat/completions
// Result: 🐢 Cache Miss. (Because "Hello world" doesn't match the "Finance Bot" prefix we hardcoded).
//
// 2. Test a Cache Hit (Matching Prefix)
// # We send the EXACT string we pre-warmed: "You are a Finance Bot. Answer strictly in JSON."
// curl -X POST -d "You are a Finance Bot. Answer strictly in JSON. What is the stock price?" http://localhost:8080/v1/chat/completions
// Result: ⚡ CACHE HIT! Routed to GPU-1
// Why: The Radix Tree matched the prefix, jumped over the "What is the stock price?" part (which wasn't in the tree), and returned the Backend ID associated with the prefix.

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sstring.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/net/inet_address.hh>
#include <iostream>
#include <memory>
#include <tokenizers_cpp.h>

#include <seastar/core/smp.hh> // For submit_to (Inter-core communication)
#include <seastar/core/loop.hh> // For parallel_for_each
#include <boost/range/irange.hpp> // To iterate over shards

#include "radix_tree.hpp"

using namespace seastar;
using namespace seastar::httpd;

// Global Tokenizer (Shared pointer is fine as it's read-only after load)
std::unique_ptr<tokenizers::Tokenizer> global_tokenizer;

// 2. The Radix Tree (Thread Local = One distinct copy per CPU Core)
// This mimics the "Replicated State" architecture of Seastar.
// Architecture note: Since Seastar is shared-nothing (threads don't share memory), we need to ensure our Radix Tree is safe. The easiest way for this MVP is to make the tree thread_local. This effectively gives every CPU core its own copy of the routing table (which is exactly how your final architecture will work anyway).
thread_local ranvier::RadixTree local_tree;

// Helper to replicate state across all CPU cores.
future<> broadcast_route(std::vector<int32_t> tokens, int backend_id) {
    // parallel_for_each executes the lambda on every item in the range (0..num_cpus)
    // boost::irange creates a range [0, 1, 2, ... smp::count-1]
    return do_with(std::move(tokens), [backend_id](std::vector<int32_t>& shared_tokens) {
        return parallel_for_each(boost::irange(0u, smp::count), [backend_id, &shared_tokens] (unsigned shard_id) {
            // smp::submit_to moves execution to the target shard
            return smp::submit_to(shard_id, [backend_id, tokens = shared_tokens] {
                // This code runs INSIDE the specific core
                local_tree.insert(tokens, backend_id);
                return make_ready_future<>();
            });
        });
    });
}

// Helper: Allows using async lambdas (futures) in routes
// function_handler is sync-only, so we need this for the Control Plane.
template <typename Func>
struct async_handler : public handler_base {
    Func _func;
    async_handler(Func&& f) : _func(std::move(f)) {}
    future<std::unique_ptr<reply>> handle(const sstring& path, std::unique_ptr<request> req, std::unique_ptr<reply> rep) override {
        return _func(std::move(req), std::move(rep));
    }
};

void setup_routes(routes& r) {
    // -------------------------------------------------------
    // DATA PLANE: High Performance Routing
    // DATA PLANE (Sync & Fast) -> function_handler
    // -------------------------------------------------------
    r.add(operation_type::POST, url("/v1/chat/completions"), new function_handler([](const_req req) {
        std::string body = req.content;
        if (!global_tokenizer) return std::string("{\"error\": \"Tokenizer not loaded\"}");

        std::vector<int32_t> tokens = global_tokenizer->Encode(body);
        auto backend_id = local_tree.lookup(tokens);

        std::string message;
        if (backend_id.has_value()) {
            message = "⚡ CACHE HIT! Routed to GPU-" + std::to_string(backend_id.value());
        } else {
            message = "🐢 Cache Miss. Routing to random GPU.";
        }
        return "{\"choices\":[{\"message\":{\"content\":\"" + message + "\"}}]}";
    }));

    // -------------------------------------------------------
    // CONTROL PLANE: Dynamic Configuration
    // CONTROL PLANE (Async & Distributed) -> async_handler
    // Usage: curl -X POST "http://localhost:8080/admin/routes?backend_id=5" -d "Prefix content..."
    // -------------------------------------------------------
    r.add(operation_type::POST, url("/admin/routes"), new async_handler([](std::unique_ptr<request> req, std::unique_ptr<reply> rep) {
        // 1. Parse Query Param (backend_id)
        // Seastar HTTP parsers return sstring, need to convert to int
        sstring id_str = req->get_query_param("backend_id");

        if (id_str.empty()) {
            // Wrap synchronous errors in make_ready_future.
            rep->write_body("json", "{\"error\": \"Missing backend_id query param\"}");
            return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
        }

        int backend_id = std::stoi(std::string(id_str));
        std::string prefix_text = req->content; // Body contains the prompt text to cache

        if (!global_tokenizer) {
            // Wrap tokenizer error in make_ready_future.
            rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
            return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
        }

        // 2. Tokenize (On the current core)
        std::vector<int32_t> tokens = global_tokenizer->Encode(prefix_text);

        // 4. Broadcast to ALL cores
        // We must wait for the broadcast to finish before replying
        return broadcast_route(tokens, backend_id).then([backend_id, rep = std::move(rep)]() mutable {
             std::cout << "[Control Plane] Route added for GPU " << backend_id << " on all cores.\n";

             rep->write_body("json", "{\"status\": \"ok\", \"backend\": " + std::to_string(backend_id) + "}");
             return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
        });
    }));
}

#if 0
void setup_routes(routes& r) {
    // Mimic the "Finance Bot" learning (Pre-warm cache)
    if (global_tokenizer) {
        // We only do this once per core setup, so it's safe here
        static bool warmed = false;
        if (!warmed) {
            // Pre-warm the cache on this core
            try {
                std::string system_prompt = "You are a Finance Bot. Answer strictly in JSON.";
                std::vector<int32_t> prompt_tokens = global_tokenizer->Encode(system_prompt);
                local_tree.insert(prompt_tokens, 1);
                std::cout << "[Core " << this_shard_id() << "] Learned route: 'Finance Bot' -> GPU 1\n";
                warmed = true;
            } catch (...) {}
        }
    }

    r.add(operation_type::POST, url("/v1/chat/completions"), new function_handler([](const_req req) {
        std::string body = req.content;

        if (!global_tokenizer) return std::string("{\"error\": \"Tokenizer not loaded\"}");

        std::vector<int32_t> tokens = global_tokenizer->Encode(body);
        auto backend_id = local_tree.lookup(tokens);

        std::string message;
        if (backend_id.has_value()) {
            message = "⚡ CACHE HIT! Routed to GPU-" + std::to_string(backend_id.value());
        } else {
            message = "🐢 Cache Miss. Routing to random GPU.";
        }

        return "{\"choices\":[{\"message\":{\"content\":\"" + message + "\"}}]}";
    }));
}
#endif

future<> run() {
    try {
        // Note: This MUST conform to the Rust BPE parser requirements
        // (e.g., "merges" key must exist, even if empty); otherwise,
        // the Rust parser will panic and the program will abort.
        std::string dummy_tokenizer_json = R"({
            "version":"1.0", "truncation": null, "padding": null, "added_tokens": [], "normalizer": null, "pre_tokenizer": null,
            "model": {
                "type": "BPE",
                "dropout": null,
                "unk_token": null,
                "continuing_subword_prefix": null,
                "end_of_word_suffix": null,
                "fuse_unk": false,
                "merges": [],
                "vocab": {
                    "You": 0, "are": 1, "a": 2, "Finance": 3, "Bot": 4, ".": 5,
                    "Answer": 6, "strictly": 7, "in": 8, "JSON": 9, "Hello": 10
                }
            }
        })";

        global_tokenizer = tokenizers::Tokenizer::FromBlobJSON(dummy_tokenizer_json);
        std::cout << "✅ Tokenizer Engine Loaded.\n";
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to load tokenizer: " << e.what() << "\n";
    }

    // 2. Manage Server Lifecycle with do_with
    // do_with ensures 'server' stays alive until the lambda chain finishes
    return do_with(http_server_control(), [](auto& server) {
        return server.start().then([&server] {
            return server.set_routes(setup_routes);
        }).then([&server] {
            return server.listen(socket_address(ipv4_addr("0.0.0.0", 8080)));
        }).then([] {
            std::cout << "⚡ Ranvier listening on port 8080... (Press Ctrl+C to stop)\n";

            // 3. The Wait Loop
            // Create a promise that we fulfill only when a signal is received
            auto stop_signal = std::make_shared<promise<>>();

            // We handle SIGINT/SIGTERM manually to ensure clean shutdown
            engine().handle_signal(SIGINT, [stop_signal] { stop_signal->set_value(); });
            engine().handle_signal(SIGTERM, [stop_signal] { stop_signal->set_value(); });

            return stop_signal->get_future();
        }).then([&server] {
            // 4. Graceful Shutdown
            std::cout << "\n🛑 Stopping Ranvier...\n";
            return server.stop();
        });
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
#endif

// Demo 3 - Refactor into 3 layers:
// 1. Infrastructure Layer (TokenizerService): Handles the raw AI logic.
// 2. Domain Layer (RouterService): Handles the business logic (Radix Tree, Broadcasting).
// 3. Presentation Layer (HttpController): Handles HTTP, JSON, and Routing.
#include "tokenizer_service.hpp"
#include "router_service.hpp"
#include "http_controller.hpp"

#include <fstream>
#include <iostream>
#include <streambuf>

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/httpd.hh>
#include <seastar/net/inet_address.hh>

using namespace seastar;
using namespace seastar::httpd;

// Services (Global/Static Scope for MVP)
ranvier::TokenizerService tokenizer;
ranvier::RouterService router;
ranvier::HttpController controller{tokenizer, router};

future<> run() {
    try {
        // Note: This MUST conform to the Rust BPE parser requirements
        // (e.g., "merges" key must exist, even if empty); otherwise,
        // the Rust parser will panic and the program will abort.
/*
        std::string dummy_tokenizer_json = R"({
            "version":"1.0", "truncation": null, "padding": null, "added_tokens": [], "normalizer": null, "pre_tokenizer": null,
            "model": {
                "type": "BPE",
                "dropout": null,
                "unk_token": null,
                "continuing_subword_prefix": null,
                "end_of_word_suffix": null,
                "fuse_unk": false,
                "merges": [],
                "vocab": {
                    "You": 0, "are": 1, "a": 2, "Finance": 3, "Bot": 4, ".": 5,
                    "Answer": 6, "strictly": 7, "in": 8, "JSON": 9, "Hello": 10
                }
            }
        })";
        std::string dummy_tokenizer_json = R"({
            "version":"1.0", "truncation": null, "padding": null, "added_tokens": [], "normalizer": null,
            "pre_tokenizer": {
                "type": "Whitespace"
            },
            "model": {
                "type": "BPE", "dropout": null, "unk_token": null, "continuing_subword_prefix": null, "end_of_word_suffix": null, "fuse_unk": false,
                "merges": [],
                "vocab": {
                    "You": 0, "are": 1, "a": 2, "Finance": 3, "Bot": 4, ".": 5, "Answer": 6, "strictly": 7, "in": 8, "JSON": 9, "Hello": 10,
                    "Help": 11, "me": 12, "write": 13, "C++": 14, "code": 15, "some": 16, "foo": 17, "bar": 18
                }
            }
        })";
*/
        // Look in the auto-copied assets folder
        std::ifstream tokenizer_stream("assets/gpt2.json");
        if (!tokenizer_stream.is_open()) {
            throw std::runtime_error("Could not find assets/gpt2.json (https://huggingface.co/gpt2/raw/main/tokenizer.json)");
        }

        std::string tokenizer_json_str((std::istreambuf_iterator<char>(tokenizer_stream)),
                              std::istreambuf_iterator<char>());
        //tokenizer.load_from_json(dummy_tokenizer_json);
        tokenizer.load_from_json(tokenizer_json_str);
        std::cout << "✅ Tokenizer Engine Loaded.\n";
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to load tokenizer: " << e.what() << "\n";
    }

    // 2. Manage Server Lifecycle with do_with
    // do_with ensures 'server' stays alive until the lambda chain finishes
    return do_with(http_server_control(), [](auto& server) {
        return server.start().then([&server] {
            return server.set_routes([](routes& r) {
                // Route setup is delegated to the Controller
                controller.register_routes(r);
            });
        }).then([&server] {
            return server.listen(socket_address(ipv4_addr("0.0.0.0", 8080)));
        }).then([] {
            std::cout << "⚡ Ranvier listening on port 8080... (Press Ctrl+C to stop)\n";

            // 3. The Wait Loop
            // Create a promise that we fulfill only when a signal is received
            auto stop_signal = std::make_shared<promise<>>();

            // We handle SIGINT/SIGTERM manually to ensure clean shutdown
            engine().handle_signal(SIGINT, [stop_signal] { stop_signal->set_value(); });
            engine().handle_signal(SIGTERM, [stop_signal] { stop_signal->set_value(); });

            return stop_signal->get_future();
        }).then([&server] {
            // 4. Graceful Shutdown
            std::cout << "\n🛑 Stopping Ranvier...\n";
            return server.stop();
        });
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
