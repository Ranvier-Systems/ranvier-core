// Ranvier Core - Cross-Shard Request Dispatch
//
// Provides zero-copy transfer of HTTP request context between shards.
// Used by the shard-aware load balancer to dispatch requests to less-loaded
// shards for processing.
//
// Key design decisions:
// - Request body uses seastar::temporary_buffer for true zero-copy from NIC
// - Headers are extracted on-demand using string_view (no map copy)
// - Reply object stays on originating shard; results stream back via futures

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>

namespace ranvier {

// Lightweight request context that can be efficiently moved between shards
// Contains only the data needed for tokenization and routing
struct CrossShardRequestContext {
    // Request body - uses temporary_buffer for true zero-copy from NIC/TCP stack
    // The buffer owns the memory and can be moved without any memcpy
    seastar::temporary_buffer<char> body;

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
    uint32_t origin_shard = 0;

    // Move constructor and assignment
    CrossShardRequestContext() = default;
    CrossShardRequestContext(CrossShardRequestContext&&) noexcept = default;
    CrossShardRequestContext& operator=(CrossShardRequestContext&&) noexcept = default;

    // Disable copying to enforce zero-copy semantics
    CrossShardRequestContext(const CrossShardRequestContext&) = delete;
    CrossShardRequestContext& operator=(const CrossShardRequestContext&) = delete;

    // Zero-copy view of the body as string_view
    // Use this for tokenization and parsing to avoid copies
    std::string_view body_view() const noexcept {
        return std::string_view(body.get(), body.size());
    }

    // Body size in bytes
    size_t body_size() const noexcept {
        return body.size();
    }

    // Check if body is empty
    bool body_empty() const noexcept {
        return body.empty();
    }

    // Create from Seastar HTTP request
    // Note: This copies from sstring to temporary_buffer since sstring doesn't
    // provide a way to release its internal buffer. For high-throughput paths,
    // prefer using string_view directly from req.content when possible.
    // The temporary_buffer enables efficient cross-shard transfer via foreign_ptr.
    static CrossShardRequestContext from_request(
        seastar::http::request& req,
        const std::string& request_id,
        const std::string& client_ip,
        const std::string& traceparent = "") {

        CrossShardRequestContext ctx;
        // Copy from sstring to temporary_buffer for cross-shard transfer
        // This is a single copy; subsequent moves are zero-copy
        ctx.body = seastar::temporary_buffer<char>(req.content.data(), req.content.size());
        ctx.request_id = request_id;
        ctx.client_ip = client_ip;
        ctx.traceparent = traceparent;
        ctx.method = req._method;
        ctx.path = req._url;
        ctx.origin_shard = seastar::this_shard_id();
        return ctx;
    }

    // Create from temporary_buffer directly (true zero-copy path)
    // Use this when you already have a temporary_buffer from the network stack
    static CrossShardRequestContext from_buffer(
        seastar::temporary_buffer<char> body_buf,
        const std::string& request_id,
        const std::string& client_ip,
        std::string method,
        std::string path,
        const std::string& traceparent = "") {

        CrossShardRequestContext ctx;
        ctx.body = std::move(body_buf);  // True zero-copy move
        ctx.request_id = request_id;
        ctx.client_ip = client_ip;
        ctx.traceparent = traceparent;
        ctx.method = std::move(method);
        ctx.path = std::move(path);
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
    //
    // Note: This uses foreign_ptr to safely transfer ownership across shards.
    // The context is wrapped in a unique_ptr, then foreign_ptr for safe cross-shard access.
    template<typename Handler>
    static seastar::future<CrossShardResult> dispatch(
        uint32_t target_shard,
        CrossShardRequestContext ctx,
        Handler&& handler) {

        if (target_shard == seastar::this_shard_id()) {
            // Local processing - no cross-shard needed
            return handler(std::move(ctx));
        }

        // Wrap in unique_ptr then foreign_ptr for safe cross-shard transfer
        auto ctx_ptr = std::make_unique<CrossShardRequestContext>(std::move(ctx));
        auto foreign = seastar::make_foreign(std::move(ctx_ptr));

        return seastar::smp::submit_to(target_shard,
            [foreign = std::move(foreign), handler = std::forward<Handler>(handler)]() mutable {
                // Move context out of foreign_ptr on target shard
                // foreign_ptr allows the pointee to be moved out
                CrossShardRequestContext local_ctx = std::move(*foreign);
                return handler(std::move(local_ctx));
            });
    }

    // Dispatch with explicit unique_ptr ownership
    // Provided for API compatibility; internally uses foreign_ptr
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
                // Move context out on target shard
                CrossShardRequestContext local_ctx = std::move(*foreign);
                return handler(std::move(local_ctx));
            });
    }
};

}  // namespace ranvier
