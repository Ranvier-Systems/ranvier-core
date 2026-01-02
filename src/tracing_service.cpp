// Ranvier Core - OpenTelemetry Distributed Tracing Implementation
//
// This file contains all OpenTelemetry includes and implementation.
// By isolating OTEL here, we avoid static initialization issues that can
// block before main() when OTEL headers are included at the top level.

#include "tracing_service.hpp"

#ifndef RANVIER_NO_TELEMETRY

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
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/samplers/always_on.h>
#include <opentelemetry/sdk/trace/samplers/always_off.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#include <opentelemetry/sdk/resource/resource.h>

#ifdef RANVIER_USE_OTLP_EXPORTER
#include <opentelemetry/exporters/otlp/otlp_http_exporter.h>
#else
#include <opentelemetry/exporters/zipkin/zipkin_exporter.h>
#endif

namespace ranvier {

namespace trace = opentelemetry::trace;
namespace sdk_trace = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;

#ifdef RANVIER_USE_OTLP_EXPORTER
namespace otlp = opentelemetry::exporter::otlp;
#else
namespace zipkin = opentelemetry::exporter::zipkin;
#endif

// ============================================================================
// Static storage (file-local to avoid header pollution)
// ============================================================================

static opentelemetry::nostd::shared_ptr<trace::Tracer> g_tracer;
static std::shared_ptr<sdk_trace::TracerProvider> g_provider;
static bool g_enabled = false;
static std::string g_service_name;

// ============================================================================
// TraceContext implementation
// ============================================================================

TraceContext TraceContext::parse(std::string_view traceparent) {
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

std::string TraceContext::to_traceparent(std::string_view span_id) const {
    if (!valid || span_id.empty()) {
        return "";
    }
    return "00-" + trace_id + "-" + std::string(span_id) + (sampled ? "-01" : "-00");
}

// ============================================================================
// ScopedSpan implementation
// ============================================================================

// Internal implementation struct (PIMPL)
struct ScopedSpan::Impl {
    opentelemetry::nostd::shared_ptr<trace::Span> span;
    std::unique_ptr<trace::Scope> scope;
};

ScopedSpan::ScopedSpan() : _impl(std::make_unique<Impl>()) {}

ScopedSpan::ScopedSpan(const std::string& name, const std::optional<TraceContext>& parent)
    : _impl(std::make_unique<Impl>()) {

    if (!g_enabled || !g_tracer) {
        return;
    }

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

    _impl->span = g_tracer->StartSpan(name, options);
    _impl->scope = std::make_unique<trace::Scope>(_impl->span);
}

ScopedSpan::~ScopedSpan() {
    if (_impl && _impl->span) {
        _impl->span->End();
    }
}

ScopedSpan::ScopedSpan(ScopedSpan&& other) noexcept = default;
ScopedSpan& ScopedSpan::operator=(ScopedSpan&& other) noexcept = default;

void ScopedSpan::set_attribute(const std::string& key, const std::string& value) {
    if (_impl && _impl->span && _impl->span->IsRecording()) {
        _impl->span->SetAttribute(key, value);
    }
}

void ScopedSpan::set_attribute(const std::string& key, int64_t value) {
    if (_impl && _impl->span && _impl->span->IsRecording()) {
        _impl->span->SetAttribute(key, value);
    }
}

void ScopedSpan::set_attribute(const std::string& key, double value) {
    if (_impl && _impl->span && _impl->span->IsRecording()) {
        _impl->span->SetAttribute(key, value);
    }
}

void ScopedSpan::set_attribute(const std::string& key, bool value) {
    if (_impl && _impl->span && _impl->span->IsRecording()) {
        _impl->span->SetAttribute(key, value);
    }
}

void ScopedSpan::set_error(const std::string& description) {
    if (_impl && _impl->span) {
        _impl->span->SetStatus(trace::StatusCode::kError, description);
    }
}

void ScopedSpan::set_ok() {
    if (_impl && _impl->span) {
        _impl->span->SetStatus(trace::StatusCode::kOk, "");
    }
}

void ScopedSpan::record_exception(const std::exception& e) {
    if (_impl && _impl->span && _impl->span->IsRecording()) {
        _impl->span->AddEvent("exception", {
            {"exception.type", typeid(e).name()},
            {"exception.message", e.what()}
        });
        _impl->span->SetStatus(trace::StatusCode::kError, e.what());
    }
}

std::string ScopedSpan::span_id() const {
    if (!_impl || !_impl->span) return "";

    char buf[17];
    auto span_ctx = _impl->span->GetContext();
    span_ctx.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>{buf, 16});
    buf[16] = '\0';
    return std::string(buf, 16);
}

std::string ScopedSpan::trace_id() const {
    if (!_impl || !_impl->span) return "";

    char buf[33];
    auto span_ctx = _impl->span->GetContext();
    span_ctx.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>{buf, 32});
    buf[32] = '\0';
    return std::string(buf, 32);
}

bool ScopedSpan::is_recording() const {
    return _impl && _impl->span && _impl->span->IsRecording();
}

// ============================================================================
// TracingService implementation
// ============================================================================

void TracingService::init(const TelemetryConfig& config) {
    g_enabled = config.enabled;
    g_service_name = config.service_name;

    if (!g_enabled) {
        // Use no-op tracer
        g_tracer = trace::Provider::GetTracerProvider()->GetTracer("ranvier-noop", OPENTELEMETRY_SDK_VERSION);
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
    zipkin::ZipkinExporterOptions exporter_opts;
    std::string endpoint = config.otlp_endpoint;
    if (endpoint.find(":4318") != std::string::npos) {
        size_t pos = endpoint.find(":4318");
        endpoint = endpoint.substr(0, pos) + ":9411/api/v2/spans";
    } else if (endpoint.find("/v1/traces") == std::string::npos &&
               endpoint.find("/api/v2/spans") == std::string::npos) {
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
    g_provider = std::make_shared<sdk_trace::TracerProvider>(
        std::move(processor),
        resource_obj,
        std::move(sampler)
    );

    // Set as global provider
    trace::Provider::SetTracerProvider(
        opentelemetry::nostd::shared_ptr<trace::TracerProvider>(g_provider));

    // Get our tracer
    g_tracer = g_provider->GetTracer("ranvier", "1.0.0");
}

void TracingService::shutdown() {
    if (g_enabled && g_provider) {
        g_provider->ForceFlush();
        g_provider.reset();
    }
    g_enabled = false;
}

bool TracingService::is_enabled() {
    return g_enabled;
}

ScopedSpan TracingService::start_span(const std::string& name,
                                       const std::optional<TraceContext>& parent) {
    return ScopedSpan(name, parent);
}

ScopedSpan TracingService::start_child_span(const std::string& name) {
    return ScopedSpan(name, std::nullopt);
}

}  // namespace ranvier

#endif  // RANVIER_NO_TELEMETRY
