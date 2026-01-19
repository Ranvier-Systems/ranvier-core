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
// - No locks: Each context is owned by exactly one shard at a time
//
// Memory safety:
// - Move-only semantics prevent accidental copies
// - Buffer size limits prevent memory exhaustion
// - Validation helpers check invariants

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <optional>

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>

namespace ranvier {

// Configuration limits for request context
struct CrossShardRequestLimits {
    // Maximum request body size (prevents OOM on malicious large requests)
    static constexpr size_t max_body_size = 128 * 1024 * 1024;  // 128 MB

    // Maximum number of client-provided tokens
    static constexpr size_t max_client_tokens = 128 * 1024;  // 128K tokens

    // Maximum length for string fields (request_id, client_ip, etc.)
    static constexpr size_t max_string_field_length = 4096;

    // Maximum URL path length
    static constexpr size_t max_path_length = 8192;
};

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

    // Validate the context's internal state
    // Returns true if all fields are within acceptable limits
    bool is_valid() const noexcept {
        if (body.size() > CrossShardRequestLimits::max_body_size) return false;
        if (request_id.size() > CrossShardRequestLimits::max_string_field_length) return false;
        if (client_ip.size() > CrossShardRequestLimits::max_string_field_length) return false;
        if (traceparent.size() > CrossShardRequestLimits::max_string_field_length) return false;
        if (path.size() > CrossShardRequestLimits::max_path_length) return false;
        if (client_tokens.size() > CrossShardRequestLimits::max_client_tokens) return false;
        return true;
    }

    // Get estimated memory footprint for this context (useful for backpressure)
    size_t estimated_memory_usage() const noexcept {
        return body.size() +
               request_id.capacity() +
               client_ip.capacity() +
               traceparent.capacity() +
               method.capacity() +
               path.capacity() +
               (client_tokens.capacity() * sizeof(int32_t));
    }

    // Force local heap allocation for all heap-owning members.
    // CRITICAL: When receiving a context via foreign_ptr from another shard,
    // the heap data for std::string and std::vector members is still allocated
    // on the source shard. Moving the context only transfers metadata (pointers),
    // not the actual heap memory. Freeing cross-shard memory causes SIGSEGV.
    //
    // This method creates a new context with all heap-owning members freshly
    // allocated on the CURRENT shard, making it safe to store and eventually free.
    //
    // Note: seastar::temporary_buffer is Seastar-aware and handles cross-shard
    // transfer correctly, so we can move it directly.
    CrossShardRequestContext force_local_allocation() && {
        CrossShardRequestContext local;
        // temporary_buffer is Seastar-aware, safe to move
        local.body = std::move(body);
        // Force fresh allocations for all std::string members
        local.request_id = std::string(request_id.begin(), request_id.end());
        local.client_ip = std::string(client_ip.begin(), client_ip.end());
        local.traceparent = std::string(traceparent.begin(), traceparent.end());
        local.method = std::string(method.begin(), method.end());
        local.path = std::string(path.begin(), path.end());
        // Force fresh allocation for vector
        local.client_tokens = std::vector<int32_t>(client_tokens.begin(), client_tokens.end());
        local.has_client_tokens = has_client_tokens;
        local.origin_shard = origin_shard;
        return local;
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

// Result of creating a CrossShardRequestContext with validation
// Used by the try_create_* factory functions
struct CrossShardRequestCreateResult {
    std::optional<CrossShardRequestContext> context;
    bool success = false;
    std::string error;

    static CrossShardRequestCreateResult ok(CrossShardRequestContext ctx) {
        CrossShardRequestCreateResult r;
        r.context = std::move(ctx);
        r.success = true;
        return r;
    }

    static CrossShardRequestCreateResult fail(std::string msg) {
        CrossShardRequestCreateResult r;
        r.error = std::move(msg);
        return r;
    }
};

// Validated factory functions for CrossShardRequestContext
// These perform size limit checks before allocation
namespace cross_shard {

// Create from Seastar HTTP request with validation
// Returns: Result with context if valid, or error message if validation fails
inline CrossShardRequestCreateResult try_create_from_request(
    seastar::http::request& req,
    const std::string& request_id,
    const std::string& client_ip,
    const std::string& traceparent = "") {

    // Validate body size before copying
    if (req.content.size() > CrossShardRequestLimits::max_body_size) {
        return CrossShardRequestCreateResult::fail("Request body exceeds maximum size");
    }

    // Validate string field lengths
    if (request_id.size() > CrossShardRequestLimits::max_string_field_length) {
        return CrossShardRequestCreateResult::fail("Request ID exceeds maximum length");
    }
    if (client_ip.size() > CrossShardRequestLimits::max_string_field_length) {
        return CrossShardRequestCreateResult::fail("Client IP exceeds maximum length");
    }
    if (req._url.size() > CrossShardRequestLimits::max_path_length) {
        return CrossShardRequestCreateResult::fail("URL path exceeds maximum length");
    }

    return CrossShardRequestCreateResult::ok(
        CrossShardRequestContext::from_request(req, request_id, client_ip, traceparent));
}

// Create from temporary_buffer with validation (true zero-copy path)
inline CrossShardRequestCreateResult try_create_from_buffer(
    seastar::temporary_buffer<char> body_buf,
    const std::string& request_id,
    const std::string& client_ip,
    std::string method,
    std::string path,
    const std::string& traceparent = "") {

    // Validate sizes before storing
    if (body_buf.size() > CrossShardRequestLimits::max_body_size) {
        return CrossShardRequestCreateResult::fail("Request body exceeds maximum size");
    }
    if (request_id.size() > CrossShardRequestLimits::max_string_field_length) {
        return CrossShardRequestCreateResult::fail("Request ID exceeds maximum length");
    }
    if (path.size() > CrossShardRequestLimits::max_path_length) {
        return CrossShardRequestCreateResult::fail("URL path exceeds maximum length");
    }

    return CrossShardRequestCreateResult::ok(
        CrossShardRequestContext::from_buffer(std::move(body_buf), request_id, client_ip,
                                              std::move(method), std::move(path), traceparent));
}

// Create with pre-tokenized tokens (validated)
inline CrossShardRequestCreateResult try_create_with_tokens(
    seastar::http::request& req,
    const std::string& request_id,
    const std::string& client_ip,
    std::vector<int32_t> tokens,
    const std::string& traceparent = "") {

    // Validate token count
    if (tokens.size() > CrossShardRequestLimits::max_client_tokens) {
        return CrossShardRequestCreateResult::fail("Token count exceeds maximum");
    }

    auto result = try_create_from_request(req, request_id, client_ip, traceparent);
    if (!result.success) {
        return result;
    }

    result.context->client_tokens = std::move(tokens);
    result.context->has_client_tokens = true;
    return result;
}

}  // namespace cross_shard

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
    // CRITICAL: We call force_local_allocation() on the target shard to ensure all
    // heap-owning members (std::string, std::vector) are allocated locally before
    // passing to the handler. Without this, freeing cross-shard memory causes SIGSEGV.
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
                // CRITICAL: Force local allocation for all heap-owning members.
                // The context's std::string and std::vector members have heap data
                // on the source shard. We must allocate fresh copies on THIS shard
                // before the foreign_ptr is destroyed, otherwise we'd free cross-shard
                // memory which causes SIGSEGV.
                CrossShardRequestContext local_ctx = std::move(*foreign).force_local_allocation();
                return handler(std::move(local_ctx));
            });
    }

    // Dispatch with explicit unique_ptr ownership
    // Provided for API compatibility; internally uses foreign_ptr
    // CRITICAL: Same as dispatch(), we call force_local_allocation() to ensure
    // all heap-owning members are allocated on the target shard.
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
                // CRITICAL: Force local allocation (see dispatch() comment for details)
                CrossShardRequestContext local_ctx = std::move(*foreign).force_local_allocation();
                return handler(std::move(local_ctx));
            });
    }
};

}  // namespace ranvier
