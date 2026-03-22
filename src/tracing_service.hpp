// Ranvier Core - OpenTelemetry Distributed Tracing Service
//
// Provides distributed tracing with:
// - W3C Trace Context propagation (traceparent header)
// - Zipkin HTTP export (default) or OTLP HTTP export (if protobuf is available)
// - Automatic span creation for request lifecycle
// - Backend ID attributes for debugging
//
// Thread-safety: Uses OpenTelemetry's thread-safe tracer provider
// Seastar compatibility: Span operations are synchronous and non-blocking
//
// Architecture: Uses PIMPL pattern to isolate OpenTelemetry includes in the .cpp
// file, avoiding static initialization issues that can block before main().
//
// Build without telemetry: cmake -DWITH_TELEMETRY=OFF to disable OpenTelemetry

#pragma once

#include "chassis_config_schema.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ranvier {

// W3C Trace Context parsed from traceparent header
// Format: {version}-{trace-id}-{parent-span-id}-{trace-flags}
struct TraceContext {
    std::string trace_id;       // 32 hex chars (16 bytes)
    std::string parent_span_id; // 16 hex chars (8 bytes)
    bool sampled = false;       // Trace flags (01 = sampled)
    bool valid = false;         // Whether parsing succeeded

    // Parse W3C traceparent header
    // Format: "00-{trace_id}-{parent_span_id}-{flags}"
    static TraceContext parse(std::string_view traceparent);

    // Generate traceparent header for outgoing requests
    std::string to_traceparent(std::string_view span_id) const;
};

#ifdef RANVIER_NO_TELEMETRY
// ============================================================================
// No-op stubs when OpenTelemetry is disabled at build time
// ============================================================================

class ScopedSpan {
public:
    ScopedSpan() = default;
    ScopedSpan(const std::string&, const std::optional<TraceContext>& = std::nullopt) {}
    ~ScopedSpan() = default;

    ScopedSpan(ScopedSpan&&) noexcept = default;
    ScopedSpan& operator=(ScopedSpan&&) noexcept = default;
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;

    void set_attribute(const std::string&, const std::string&) {}
    void set_attribute(const std::string&, int64_t) {}
    void set_attribute(const std::string&, double) {}
    void set_attribute(const std::string&, bool) {}
    void set_error(const std::string&) {}
    void set_ok() {}
    void record_exception(const std::exception&) {}
    std::string span_id() const { return ""; }
    std::string trace_id() const { return ""; }
    bool is_recording() const { return false; }
};

class TracingService {
public:
    static void init(const TelemetryConfig&) {}
    static void shutdown() {}
    static bool is_enabled() { return false; }
    static ScopedSpan start_span(const std::string&,
                                  const std::optional<TraceContext>& = std::nullopt) {
        return ScopedSpan();
    }
    static ScopedSpan start_child_span(const std::string&) { return ScopedSpan(); }
};

#else  // RANVIER_NO_TELEMETRY not defined - use real OpenTelemetry
// ============================================================================
// Real implementation using PIMPL to isolate OpenTelemetry includes
// All OpenTelemetry headers are in tracing_service.cpp only
// ============================================================================

// RAII span wrapper for automatic scope management
// Uses PIMPL to avoid including OpenTelemetry headers here
class ScopedSpan {
public:
    ScopedSpan();
    ScopedSpan(const std::string& name,
               const std::optional<TraceContext>& parent = std::nullopt);
    ~ScopedSpan();

    // Non-copyable, movable
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;
    ScopedSpan(ScopedSpan&& other) noexcept;
    ScopedSpan& operator=(ScopedSpan&& other) noexcept;

    // Set attributes on the span
    void set_attribute(const std::string& key, const std::string& value);
    void set_attribute(const std::string& key, int64_t value);
    void set_attribute(const std::string& key, double value);
    void set_attribute(const std::string& key, bool value);

    // Set span as error (convenience method)
    void set_error(const std::string& description);

    // Set span as ok (convenience method)
    void set_ok();

    // Record an exception
    void record_exception(const std::exception& e);

    // Get the span ID for propagation
    std::string span_id() const;

    // Get the trace ID for logging
    std::string trace_id() const;

    // Check if this span is recording (sampled)
    bool is_recording() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Global tracing service
// Initialize once at startup, then use start_span() to create spans
class TracingService {
public:
    // Initialize OpenTelemetry with the given configuration
    // Call once at application startup before serving requests
    // This is where OTEL is actually initialized - not at static init time
    static void init(const TelemetryConfig& config);

    // Shutdown OpenTelemetry and flush pending spans
    // Call at application shutdown after all requests complete
    static void shutdown();

    // Check if tracing is enabled
    static bool is_enabled();

    // Start a new span with optional parent context
    static ScopedSpan start_span(const std::string& name,
                                  const std::optional<TraceContext>& parent = std::nullopt);

    // Convenience method to start a child span from current context
    static ScopedSpan start_child_span(const std::string& name);

private:
    // Implementation details are in tracing_service.cpp
    friend class ScopedSpan;
    static void* get_tracer();  // Returns opaque pointer to tracer
};

#endif  // RANVIER_NO_TELEMETRY

}  // namespace ranvier
