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

/*
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