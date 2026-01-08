// Ranvier Core - Cross-Shard Request Dispatch
//
// Provides zero-copy transfer of HTTP request context between shards.
// Used by the shard-aware load balancer to dispatch requests to less-loaded
// shards for processing.
//
// Key design decisions:
// - Request body is moved (not copied) using std::move semantics
// - Headers are moved as a map (relatively small, acceptable to copy if needed)
// - Reply object stays on originating shard; results stream back via futures

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>

namespace ranvier {

// Lightweight request context that can be efficiently moved between shards
// Contains only the data needed for tokenization and routing
struct CrossShardRequestContext {
    // Request body - moved, not copied (zero-copy)
    std::string body;

    // Request ID for tracing
    std::string request_id;

    // Client IP for rate limiting context
    std::string client_ip;

    // Traceparent header for distributed tracing
    std::string traceparent;

    // Pre-extracted token IDs (if client provided them)
    std::vector<int32_t> client_tokens;
    bool has_client_tokens = false;

    // HTTP method and path (for logging/metrics)
    std::string method;
    std::string path;

    // Originating shard (for metrics and debugging)
    uint32_t origin_shard;

    // Move constructor and assignment
    CrossShardRequestContext() = default;
    CrossShardRequestContext(CrossShardRequestContext&&) noexcept = default;
    CrossShardRequestContext& operator=(CrossShardRequestContext&&) noexcept = default;

    // Disable copying to enforce zero-copy semantics
    CrossShardRequestContext(const CrossShardRequestContext&) = delete;
    CrossShardRequestContext& operator=(const CrossShardRequestContext&) = delete;

    // Create from Seastar HTTP request (moves body)
    static CrossShardRequestContext from_request(
        seastar::http::request& req,
        const std::string& request_id,
        const std::string& client_ip,
        const std::string& traceparent = "") {

        CrossShardRequestContext ctx;
        ctx.body = std::move(req.content);  // Zero-copy move
        ctx.request_id = request_id;
        ctx.client_ip = client_ip;
        ctx.traceparent = traceparent;
        ctx.method = req._method;
        ctx.path = req._url;
        ctx.origin_shard = seastar::this_shard_id();
        return ctx;
    }

    // Create with pre-tokenized tokens
    static CrossShardRequestContext from_request_with_tokens(
        seastar::http::request& req,
        const std::string& request_id,
        const std::string& client_ip,
        std::vector<int32_t> tokens,
        const std::string& traceparent = "") {

        auto ctx = from_request(req, request_id, client_ip, traceparent);
        ctx.client_tokens = std::move(tokens);
        ctx.has_client_tokens = true;
        return ctx;
    }
};

// Result of cross-shard request processing
// Returned from the target shard to the originating shard
struct CrossShardResult {
    // HTTP status code
    uint16_t status_code = 200;

    // Response body (for non-streaming responses)
    std::string body;

    // Response headers to add
    std::unordered_map<std::string, std::string> headers;

    // For streaming responses: the stream is handled separately
    bool is_streaming = false;

    // Error information
    bool has_error = false;
    std::string error_message;

    // Backend that handled the request (for metrics)
    uint32_t backend_id = 0;

    // Processing time on target shard (nanoseconds)
    uint64_t processing_time_ns = 0;

    // Factory methods for common responses
    static CrossShardResult success(std::string body) {
        CrossShardResult r;
        r.body = std::move(body);
        return r;
    }

    static CrossShardResult error(uint16_t code, std::string message) {
        CrossShardResult r;
        r.status_code = code;
        r.has_error = true;
        r.error_message = std::move(message);
        return r;
    }

    static CrossShardResult streaming() {
        CrossShardResult r;
        r.is_streaming = true;
        return r;
    }
};

// Cross-shard dispatcher
// Handles moving request context to target shard and coordinating response
class CrossShardDispatcher {
public:
    // Dispatch a request to a specific shard for processing
    // The handler function is invoked on the target shard with moved context
    template<typename Handler>
    static seastar::future<CrossShardResult> dispatch(
        uint32_t target_shard,
        CrossShardRequestContext ctx,
        Handler&& handler) {

        if (target_shard == seastar::this_shard_id()) {
            // Local processing - no cross-shard needed
            return handler(std::move(ctx));
        }

        // Cross-shard dispatch using submit_to
        // Note: We use foreign_ptr for safe cross-shard pointer transfer
        auto ctx_ptr = seastar::make_lw_shared<CrossShardRequestContext>(std::move(ctx));

        return seastar::smp::submit_to(target_shard,
            [ctx_ptr, handler = std::forward<Handler>(handler)]() mutable {
                // Move context out of shared_ptr on target shard
                CrossShardRequestContext local_ctx = std::move(*ctx_ptr);
                return handler(std::move(local_ctx));
            });
    }

    // Dispatch with foreign_ptr for explicit lifetime management
    // Use this when the handler needs to access the context multiple times
    template<typename Handler>
    static seastar::future<CrossShardResult> dispatch_with_foreign(
        uint32_t target_shard,
        std::unique_ptr<CrossShardRequestContext> ctx,
        Handler&& handler) {

        if (target_shard == seastar::this_shard_id()) {
            return handler(std::move(*ctx));
        }

        // Use foreign_ptr for safe cross-shard unique_ptr transfer
        auto foreign = seastar::make_foreign(std::move(ctx));

        return seastar::smp::submit_to(target_shard,
            [foreign = std::move(foreign), handler = std::forward<Handler>(handler)]() mutable {
                // Get the local pointer on target shard
                auto& ctx = *foreign;
                // Create a local copy for processing (foreign_ptr content can't be moved)
                CrossShardRequestContext local_ctx;
                local_ctx.body = std::move(ctx.body);
                local_ctx.request_id = std::move(ctx.request_id);
                local_ctx.client_ip = std::move(ctx.client_ip);
                local_ctx.traceparent = std::move(ctx.traceparent);
                local_ctx.client_tokens = std::move(ctx.client_tokens);
                local_ctx.has_client_tokens = ctx.has_client_tokens;
                local_ctx.method = std::move(ctx.method);
                local_ctx.path = std::move(ctx.path);
                local_ctx.origin_shard = ctx.origin_shard;
                return handler(std::move(local_ctx));
            });
    }
};

}  // namespace ranvier
