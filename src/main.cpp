#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <iostream>
#include <tokenizers_cpp.h> // Prove we linked the Rust library correctly

// Demo 1 - Run seastar
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

/*
// Demo 2 - Run tokenizer
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

using namespace seastar;
using namespace seastar::httpd; // <--- FIXED: This is where 'routes' lives

// Global pointer for the demo
std::unique_ptr<tokenizers::Tokenizer> global_tokenizer;

void setup_routes(http_server_control& server) {
    // POST /v1/chat/completions
    // This mimics the OpenAI API entry point
    server.set_routes([](routes& r) {
        r.add(operation_type::POST, url("/v1/chat/completions"), new function_handler([](const_req req) {
            // 1. "Snoop" the request content
            // content is an sstring in Seastar, acts like std::string
            std::string prompt = req.content;

            // 2. Tokenize (The "Heavy" Logic)
            int token_count = 0;
            if (global_tokenizer) {
                // Encode logic
                std::vector<int32_t> tokens = global_tokenizer->Encode(prompt);
                token_count = tokens.size();

                // Safe to print from a reactor thread (no locks needed)
                std::cout << "[Core " << this_shard_id() << "] Routed " << token_count << " tokens.\n";
            }

            // 3. Return a dummy response
            // We return a simple JSON string. Seastar handles the HTTP 200 OK wrapper.
            return "{\"id\":\"chatcmpl-mock\",\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"[Ranvier] Route calculated.\"}}]}";
        }));
    });
}

future<> run() {
    // 1. Load Dummy Tokenizer
    // We use a tiny JSON blob to initialize a basic tokenizer without needing a file on disk yet.
    try {
        std::string dummy_tokenizer_json = R"({
            "version":"1.0",
            "truncation": null,
            "padding": null,
            "added_tokens": [],
            "normalizer": null,
            "pre_tokenizer": null,
            "model": {
                "type": "BPE",
                "dropout": null,
                "unk_token": null,
                "continuing_subword_prefix": null,
                "end_of_word_suffix": null,
                "fuse_unk": false,
                "vocab": {"a": 0, "b": 1, "c": 2}
            }
        })";

        global_tokenizer = tokenizers::Tokenizer::FromBlobJSON(dummy_tokenizer_json);
        std::cout << "✅ Tokenizer Engine Loaded.\n";
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to load tokenizer: " << e.what() << "\n";
    }

    // 2. Start HTTP Server on ALL cores
    // 'static' keeps the server object alive for the duration of the program
    static http_server_control server;

    return server.start().then([&server] {
        return server.set_routes(setup_routes);
    }).then([&server] {
        // Listen on 0.0.0.0:8080
        return server.listen(socket_address(make_ipv4_address({0, 0, 0, 0}), 8080));
    }).then([] {
        std::cout << "⚡ Ranvier listening on port 8080... (Try: curl -X POST localhost:8080/v1/chat/completions -d 'abc')\n";
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
*/

/*
// Demo 3 - Integrate with RadixTree
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

// 1. Include your new header
#include "radix_tree.hh"

using namespace seastar;
using namespace seastar::httpd;

// Global Tokenizer (Shared pointer is fine as it's read-only after load)
std::unique_ptr<tokenizers::Tokenizer> global_tokenizer;

// 2. The Radix Tree (Thread Local = One distinct copy per CPU Core)
// This mimics the "Replicated State" architecture of Seastar.
// Architecture note: Since Seastar is shared-nothing (threads don't share memory), we need to ensure our Radix Tree is safe. The easiest way for this MVP is to make the tree thread_local. This effectively gives every CPU core its own copy of the routing table (which is exactly how your final architecture will work anyway).
thread_local ranvier::RadixTree local_tree;

void setup_routes(http_server_control& server) {
    // 3. "Pre-warm" the Cache (Simulation)
    // Let's pretend GPU-1 (ID: 1) already has the "Finance Bot" system prompt loaded.
    // We insert this into the tree on THIS core.
    if (global_tokenizer) {
        std::string system_prompt = "You are a Finance Bot. Answer strictly in JSON.";
        std::vector<int32_t> prompt_tokens = global_tokenizer->Encode(system_prompt);

        // Map this prefix -> Backend ID 1
        local_tree.insert(prompt_tokens, 1);
        std::cout << "[Core " << this_shard_id() << "] Learned route: 'Finance Bot' -> GPU 1\n";
    }

    server.set_routes([](routes& r) {
        r.add(operation_type::POST, url("/v1/chat/completions"), new function_handler([](const_req req) {
            std::string body = req.content;

            if (!global_tokenizer) return "{\"error\": \"Tokenizer not loaded\"}";

            // A. Tokenize the incoming request
            std::vector<int32_t> tokens = global_tokenizer->Encode(body);

            // B. Consult the Oracle (Radix Tree)
            // We verify if this request starts with a known prefix
            auto backend_id = local_tree.lookup(tokens);

            // C. Build Response
            std::string message;
            if (backend_id.has_value()) {
                // CACHE HIT
                message = "⚡ CACHE HIT! Routed to GPU-" + std::to_string(backend_id.value());
            } else {
                // CACHE MISS
                message = "🐢 Cache Miss. Routing to random GPU.";
            }

            return "{\"choices\":[{\"message\":{\"content\":\"" + message + "\"}}]}";
        }));
    });
}

future<> run() {
    // 4. Load Dummy Tokenizer (Same as before)
    try {
        std::string dummy_tokenizer_json = R"({
            "version":"1.0", "truncation": null, "padding": null, "added_tokens": [], "normalizer": null, "pre_tokenizer": null,
            "model": {
                "type": "BPE", "dropout": null, "unk_token": null, "continuing_subword_prefix": null, "end_of_word_suffix": null, "fuse_unk": false,
                "vocab": {"You": 0, "are": 1, "a": 2, "Finance": 3, "Bot": 4, ".": 5, "Answer": 6, "strictly": 7, "in": 8, "JSON": 9, "Hello": 10}
            }
        })";
        global_tokenizer = tokenizers::Tokenizer::FromBlobJSON(dummy_tokenizer_json);
        std::cout << "✅ Tokenizer Engine Loaded.\n";
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to load tokenizer: " << e.what() << "\n";
    }

    static http_server_control server;

    return server.start().then([&server] {
        return server.set_routes(setup_routes);
    }).then([&server] {
        return server.listen(socket_address(make_ipv4_address({0, 0, 0, 0}), 8080));
    }).then([] {
        std::cout << "⚡ Ranvier listening on port 8080...\n";
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
*/
