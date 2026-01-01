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
// Exporter selection:
// - Zipkin: Default, works without protobuf, sends to Zipkin/Jaeger collectors
// - OTLP: Requires protobuf, preferred for OTEL Collector/Tempo/modern backends
// Define RANVIER_USE_OTLP_EXPORTER before including to use OTLP (requires protobuf)

#pragma once

#include "config.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <opentelemetry/nostd/span.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/batch_span_processor.h>
#include <opentelemetry/sdk/trace/samplers/always_on.h>
#include <opentelemetry/sdk/trace/samplers/always_off.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#include <opentelemetry/sdk/resource/resource.h>

// Include the appropriate exporter
#ifdef RANVIER_USE_OTLP_EXPORTER
#include <opentelemetry/exporters/otlp/otlp_http_exporter.h>
#else
#include <opentelemetry/exporters/zipkin/zipkin_exporter.h>
#endif

namespace ranvier {

namespace trace = opentelemetry::trace;
namespace sdk_trace = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;
namespace propagation = opentelemetry::context::propagation;

#ifdef RANVIER_USE_OTLP_EXPORTER
namespace otlp = opentelemetry::exporter::otlp;
#else
namespace zipkin = opentelemetry::exporter::zipkin;
#endif

// W3C Trace Context parsed from traceparent header
// Format: {version}-{trace-id}-{parent-span-id}-{trace-flags}
struct TraceContext {
    std::string trace_id;      // 32 hex chars (16 bytes)
    std::string parent_span_id; // 16 hex chars (8 bytes)
    bool sampled = false;       // Trace flags (01 = sampled)
    bool valid = false;         // Whether parsing succeeded

    // Parse W3C traceparent header
    // Format: "00-{trace_id}-{parent_span_id}-{flags}"
    static TraceContext parse(std::string_view traceparent);

    // Generate traceparent header for outgoing requests
    std::string to_traceparent(std::string_view span_id) const;
};

// RAII span wrapper for automatic scope management
// Usage:
//   auto span = tracer().start_span("ranvier.request", trace_ctx);
//   span.set_attribute("ranvier.backend_id", backend_id);
//   // span automatically ends when it goes out of scope
class ScopedSpan {
public:
    ScopedSpan(trace::Tracer& tracer, const std::string& name,
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

    // Set span status
    void set_status(trace::StatusCode code, const std::string& description = "");

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

    // Access the underlying span
    trace::Span& span() { return *_span; }

private:
    opentelemetry::nostd::shared_ptr<trace::Span> _span;
    std::unique_ptr<trace::Scope> _scope;
};

// Global tracing service
// Initialize once at startup, then use tracer() to get the tracer
class TracingService {
public:
    // Initialize OpenTelemetry with the given configuration
    // Call once at application startup before serving requests
    static void init(const TelemetryConfig& config);

    // Shutdown OpenTelemetry and flush pending spans
    // Call at application shutdown after all requests complete
    static void shutdown();

    // Check if tracing is enabled
    static bool is_enabled();

    // Get the tracer instance
    // Returns a no-op tracer if tracing is disabled
    static trace::Tracer& tracer();

    // Start a new span with optional parent context
    // Caller takes ownership of the returned ScopedSpan
    static ScopedSpan start_span(const std::string& name,
                                 const std::optional<TraceContext>& parent = std::nullopt);

    // Convenience method to start a child span from current context
    static ScopedSpan start_child_span(const std::string& name);

private:
    static opentelemetry::nostd::shared_ptr<trace::Tracer> _tracer;
    static bool _enabled;
    static std::string _service_name;
};

// ============================================================================
// Implementation
// ============================================================================

inline TraceContext TraceContext::parse(std::string_view traceparent) {
    TraceContext ctx;

    // Minimum length: "00-" + 32 + "-" + 16 + "-" + 2 = 55
    if (traceparent.length() < 55) {
        return ctx;
    }

    // Version check (only support version 00)
    if (traceparent[0] != '0' || traceparent[1] != '0' || traceparent[2] != '-') {
        return ctx;
    }

    // Extract trace-id (32 hex chars)
    ctx.trace_id = std::string(traceparent.substr(3, 32));
    if (traceparent[35] != '-') {
        return ctx;
    }

    // Extract parent-span-id (16 hex chars)
    ctx.parent_span_id = std::string(traceparent.substr(36, 16));
    if (traceparent[52] != '-') {
        return ctx;
    }

    // Extract trace-flags (2 hex chars)
    if (traceparent.length() >= 55) {
        std::string_view flags = traceparent.substr(53, 2);
        ctx.sampled = (flags == "01");
    }

    // Validate hex characters
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };

    for (char c : ctx.trace_id) {
        if (!is_hex(c)) return ctx;
    }
    for (char c : ctx.parent_span_id) {
        if (!is_hex(c)) return ctx;
    }

    // Check for all-zeros (invalid)
    bool all_zeros = true;
    for (char c : ctx.trace_id) {
        if (c != '0') { all_zeros = false; break; }
    }
    if (all_zeros) return ctx;

    ctx.valid = true;
    return ctx;
}

inline std::string TraceContext::to_traceparent(std::string_view span_id) const {
    if (!valid || span_id.empty()) {
        return "";
    }
    return "00-" + trace_id + "-" + std::string(span_id) + (sampled ? "-01" : "-00");
}

// ScopedSpan implementation
inline ScopedSpan::ScopedSpan(trace::Tracer& tracer, const std::string& name,
                              const std::optional<TraceContext>& parent) {
    trace::StartSpanOptions options;

    if (parent && parent->valid) {
        // Create span context from parent
        uint8_t trace_id_bytes[16];
        uint8_t span_id_bytes[8];

        // Parse hex strings to bytes
        auto hex_to_byte = [](char hi, char lo) -> uint8_t {
            auto char_to_nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            return (char_to_nibble(hi) << 4) | char_to_nibble(lo);
        };

        for (size_t i = 0; i < 16; ++i) {
            trace_id_bytes[i] = hex_to_byte(parent->trace_id[i*2], parent->trace_id[i*2+1]);
        }
        for (size_t i = 0; i < 8; ++i) {
            span_id_bytes[i] = hex_to_byte(parent->parent_span_id[i*2], parent->parent_span_id[i*2+1]);
        }

        trace::TraceId tid(opentelemetry::nostd::span<const uint8_t, 16>(trace_id_bytes, 16));
        trace::SpanId sid(opentelemetry::nostd::span<const uint8_t, 8>(span_id_bytes, 8));
        trace::TraceFlags flags(parent->sampled ? trace::TraceFlags::kIsSampled : 0);

        trace::SpanContext parent_ctx(tid, sid, flags, true);
        options.parent = parent_ctx;
    }

    _span = tracer.StartSpan(name, options);
    _scope = std::make_unique<trace::Scope>(_span);
}

inline ScopedSpan::~ScopedSpan() {
    if (_span) {
        _span->End();
    }
}

inline ScopedSpan::ScopedSpan(ScopedSpan&& other) noexcept
    : _span(std::move(other._span)), _scope(std::move(other._scope)) {
    other._span = nullptr;
}

inline ScopedSpan& ScopedSpan::operator=(ScopedSpan&& other) noexcept {
    if (this != &other) {
        if (_span) {
            _span->End();
        }
        _span = std::move(other._span);
        _scope = std::move(other._scope);
        other._span = nullptr;
    }
    return *this;
}

inline void ScopedSpan::set_attribute(const std::string& key, const std::string& value) {
    if (_span && _span->IsRecording()) {
        _span->SetAttribute(key, value);
    }
}

inline void ScopedSpan::set_attribute(const std::string& key, int64_t value) {
    if (_span && _span->IsRecording()) {
        _span->SetAttribute(key, value);
    }
}

inline void ScopedSpan::set_attribute(const std::string& key, double value) {
    if (_span && _span->IsRecording()) {
        _span->SetAttribute(key, value);
    }
}

inline void ScopedSpan::set_attribute(const std::string& key, bool value) {
    if (_span && _span->IsRecording()) {
        _span->SetAttribute(key, value);
    }
}

inline void ScopedSpan::set_status(trace::StatusCode code, const std::string& description) {
    if (_span) {
        _span->SetStatus(code, description);
    }
}

inline void ScopedSpan::set_error(const std::string& description) {
    set_status(trace::StatusCode::kError, description);
}

inline void ScopedSpan::set_ok() {
    set_status(trace::StatusCode::kOk, "");
}

inline void ScopedSpan::record_exception(const std::exception& e) {
    if (_span && _span->IsRecording()) {
        _span->AddEvent("exception", {
            {"exception.type", typeid(e).name()},
            {"exception.message", e.what()}
        });
        _span->SetStatus(trace::StatusCode::kError, e.what());
    }
}

inline std::string ScopedSpan::span_id() const {
    if (!_span) return "";

    char buf[17];
    auto span_ctx = _span->GetContext();
    span_ctx.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>{buf, 16});
    buf[16] = '\0';
    return std::string(buf, 16);
}

inline std::string ScopedSpan::trace_id() const {
    if (!_span) return "";

    char buf[33];
    auto span_ctx = _span->GetContext();
    span_ctx.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>{buf, 32});
    buf[32] = '\0';
    return std::string(buf, 32);
}

inline bool ScopedSpan::is_recording() const {
    return _span && _span->IsRecording();
}

// Static member initialization
inline opentelemetry::nostd::shared_ptr<trace::Tracer> TracingService::_tracer;
inline bool TracingService::_enabled = false;
inline std::string TracingService::_service_name;

inline void TracingService::init(const TelemetryConfig& config) {
    _enabled = config.enabled;
    _service_name = config.service_name;

    if (!_enabled) {
        // Use no-op tracer
        _tracer = trace::Provider::GetTracerProvider()->GetTracer("ranvier-noop", OPENTELEMETRY_SDK_VERSION);
        return;
    }

    // Create exporter based on build configuration
#ifdef RANVIER_USE_OTLP_EXPORTER
    // OTLP HTTP exporter (requires protobuf)
    otlp::OtlpHttpExporterOptions exporter_opts;
    exporter_opts.url = config.otlp_endpoint + "/v1/traces";
    auto exporter = std::make_unique<otlp::OtlpHttpExporter>(exporter_opts);
#else
    // Zipkin exporter (no protobuf required)
    // Zipkin endpoint format: http://host:9411/api/v2/spans
    // For Jaeger with Zipkin receiver: http://host:9411/api/v2/spans
    zipkin::ZipkinExporterOptions exporter_opts;
    // Convert OTLP endpoint to Zipkin format if needed
    // OTLP: http://localhost:4318 -> Zipkin: http://localhost:9411/api/v2/spans
    std::string endpoint = config.otlp_endpoint;
    if (endpoint.find(":4318") != std::string::npos) {
        // Convert OTLP default port to Zipkin default port
        size_t pos = endpoint.find(":4318");
        endpoint = endpoint.substr(0, pos) + ":9411/api/v2/spans";
    } else if (endpoint.find("/v1/traces") == std::string::npos &&
               endpoint.find("/api/v2/spans") == std::string::npos) {
        // Append Zipkin path if not already present
        endpoint += "/api/v2/spans";
    }
    exporter_opts.url = endpoint;
    exporter_opts.service_name = config.service_name;
    auto exporter = std::make_unique<zipkin::ZipkinExporter>(exporter_opts);
#endif

    // Create batch processor with configured settings
    sdk_trace::BatchSpanProcessorOptions processor_opts;
    processor_opts.max_queue_size = config.max_queue_size;
    processor_opts.max_export_batch_size = config.max_export_batch_size;
    processor_opts.schedule_delay_millis = std::chrono::duration_cast<std::chrono::milliseconds>(config.export_interval);
    auto processor = std::make_unique<sdk_trace::BatchSpanProcessor>(std::move(exporter), processor_opts);

    // Create sampler with configured rate
    std::unique_ptr<sdk_trace::Sampler> sampler;
    if (config.sample_rate >= 1.0) {
        sampler = std::make_unique<sdk_trace::AlwaysOnSampler>();
    } else if (config.sample_rate <= 0.0) {
        sampler = std::make_unique<sdk_trace::AlwaysOffSampler>();
    } else {
        sampler = std::make_unique<sdk_trace::TraceIdRatioBasedSampler>(config.sample_rate);
    }

    // Create resource with service name
    auto resource_attrs = resource::ResourceAttributes{
        {"service.name", config.service_name},
        {"service.version", "1.0.0"},
        {"deployment.environment", "production"}
    };
    auto resource_obj = resource::Resource::Create(resource_attrs);

    // Create tracer provider
    auto provider = std::make_shared<sdk_trace::TracerProvider>(
        std::move(processor),
        resource_obj,
        std::move(sampler)
    );

    // Set as global provider
    trace::Provider::SetTracerProvider(provider);

    // Get our tracer
    _tracer = provider->GetTracer("ranvier", "1.0.0");
}

inline void TracingService::shutdown() {
    if (_enabled) {
        // Get provider and force flush
        auto provider = trace::Provider::GetTracerProvider();
        if (auto sdk_provider = std::dynamic_pointer_cast<sdk_trace::TracerProvider>(provider)) {
            sdk_provider->ForceFlush();
        }
    }
    _enabled = false;
}

inline bool TracingService::is_enabled() {
    return _enabled;
}

inline trace::Tracer& TracingService::tracer() {
    return *_tracer;
}

inline ScopedSpan TracingService::start_span(const std::string& name,
                                             const std::optional<TraceContext>& parent) {
    return ScopedSpan(tracer(), name, parent);
}

inline ScopedSpan TracingService::start_child_span(const std::string& name) {
    return ScopedSpan(tracer(), name, std::nullopt);
}

}  // namespace ranvier
