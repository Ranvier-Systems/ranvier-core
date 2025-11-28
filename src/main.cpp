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

#include "radix_tree.hpp"

using namespace seastar;
using namespace seastar::httpd;

// Global Tokenizer (Shared pointer is fine as it's read-only after load)
std::unique_ptr<tokenizers::Tokenizer> global_tokenizer;

// 2. The Radix Tree (Thread Local = One distinct copy per CPU Core)
// This mimics the "Replicated State" architecture of Seastar.
// Architecture note: Since Seastar is shared-nothing (threads don't share memory), we need to ensure our Radix Tree is safe. The easiest way for this MVP is to make the tree thread_local. This effectively gives every CPU core its own copy of the routing table (which is exactly how your final architecture will work anyway).
thread_local ranvier::RadixTree local_tree;

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