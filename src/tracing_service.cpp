// Ranvier Core - OpenTelemetry Distributed Tracing Implementation
//
// This file contains all OpenTelemetry includes and implementation.
// By isolating OTEL here, we avoid static initialization issues that can
// block before main() when OTEL headers are included at the top level.
//
// Thread-safety: Uses std::call_once for one-time initialization and
// std::atomic<bool> for the enabled flag. This ensures safe concurrent
// access from multiple Seastar shards without blocking the hot path.

#include "tracing_service.hpp"

#include <atomic>
#include <mutex>  // for std::call_once, std::once_flag

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

// Use Zipkin exporter (OTLP exporter disabled due to protobuf static init issues)
#include <opentelemetry/exporters/zipkin/zipkin_exporter.h>

namespace ranvier {

namespace trace = opentelemetry::trace;
namespace sdk_trace = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;
namespace zipkin = opentelemetry::exporter::zipkin;

// ============================================================================
// W3C Trace Context Format Constants
// Spec: https://www.w3.org/TR/trace-context/
//
// Format: {version}-{trace-id}-{parent-span-id}-{trace-flags}
// Example: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01
// ============================================================================
namespace w3c {

// Field lengths (in hex characters)
constexpr size_t VERSION_HEX_LEN = 2;       // "00"
constexpr size_t TRACE_ID_HEX_LEN = 32;     // 16 bytes = 32 hex chars
constexpr size_t SPAN_ID_HEX_LEN = 16;      // 8 bytes = 16 hex chars
constexpr size_t TRACE_FLAGS_HEX_LEN = 2;   // "00" or "01"
constexpr size_t SEPARATOR_LEN = 1;         // "-"

// Field lengths (in bytes, for binary representation)
constexpr size_t TRACE_ID_BYTE_LEN = 16;
constexpr size_t SPAN_ID_BYTE_LEN = 8;

// Field offsets in traceparent string
// Layout: VV-TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT-SSSSSSSSSSSSSSSS-FF
//         0  3                                36               53
constexpr size_t VERSION_OFFSET = 0;
constexpr size_t TRACE_ID_OFFSET = VERSION_HEX_LEN + SEPARATOR_LEN;  // 3
constexpr size_t SPAN_ID_OFFSET = TRACE_ID_OFFSET + TRACE_ID_HEX_LEN + SEPARATOR_LEN;  // 36
constexpr size_t FLAGS_OFFSET = SPAN_ID_OFFSET + SPAN_ID_HEX_LEN + SEPARATOR_LEN;  // 53

// Separator positions (indices where '-' should appear)
constexpr size_t SEP_AFTER_VERSION = VERSION_HEX_LEN;  // 2
constexpr size_t SEP_AFTER_TRACE_ID = TRACE_ID_OFFSET + TRACE_ID_HEX_LEN;  // 35
constexpr size_t SEP_AFTER_SPAN_ID = SPAN_ID_OFFSET + SPAN_ID_HEX_LEN;  // 52

// Minimum valid traceparent length
constexpr size_t MIN_TRACEPARENT_LEN = FLAGS_OFFSET + TRACE_FLAGS_HEX_LEN;  // 55

// Known values
constexpr char SEPARATOR = '-';
constexpr std::string_view SUPPORTED_VERSION = "00";
constexpr std::string_view SAMPLED_FLAG = "01";
constexpr std::string_view UNSAMPLED_FLAG = "00";

// Buffer sizes for hex string output (includes null terminator)
constexpr size_t TRACE_ID_BUF_SIZE = TRACE_ID_HEX_LEN + 1;  // 33
constexpr size_t SPAN_ID_BUF_SIZE = SPAN_ID_HEX_LEN + 1;    // 17

// Helper: Check if character is valid hexadecimal
constexpr bool is_hex_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Helper: Convert hex character pair to byte
constexpr uint8_t hex_pair_to_byte(char hi, char lo) noexcept {
    auto char_to_nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    };
    return static_cast<uint8_t>((char_to_nibble(hi) << 4) | char_to_nibble(lo));
}

// Helper: Validate that a string contains only hex characters
inline bool is_valid_hex_string(std::string_view s) noexcept {
    for (char c : s) {
        if (!is_hex_char(c)) return false;
    }
    return true;
}

// Helper: Check if hex string is all zeros (invalid per W3C spec)
inline bool is_all_zeros(std::string_view s) noexcept {
    for (char c : s) {
        if (c != '0') return false;
    }
    return true;
}

}  // namespace w3c

// ============================================================================
// Static storage (file-local to avoid header pollution)
//
// Thread-safety model:
// - g_init_flag: Ensures init() runs exactly once across all shards
// - g_enabled: Atomic flag for lock-free reads in hot path (start_span)
// - g_shutting_down: Atomic flag set during shutdown(); provides a barrier
//   to prevent new span creation while shutdown is in progress
// - g_tracer/g_provider: Written once during init (protected by call_once),
//   then only read. Never reset — they are process-lifetime objects to
//   avoid a TOCTOU race between the g_shutting_down check and StartSpan.
//   The acquire fence on g_enabled ensures visibility after init.
// - g_service_name: Written once during init, immutable thereafter
// ============================================================================

static std::once_flag g_init_flag;
static std::atomic<bool> g_enabled{false};
static std::atomic<bool> g_shutting_down{false};
static opentelemetry::nostd::shared_ptr<trace::Tracer> g_tracer;
static std::shared_ptr<sdk_trace::TracerProvider> g_provider;
static std::string g_service_name;

// ============================================================================
// TraceContext implementation
// ============================================================================

TraceContext TraceContext::parse(std::string_view traceparent) {
    TraceContext ctx;

    // Validate minimum length
    if (traceparent.length() < w3c::MIN_TRACEPARENT_LEN) {
        return ctx;
    }

    // Validate version field (only support version "00")
    std::string_view version = traceparent.substr(w3c::VERSION_OFFSET, w3c::VERSION_HEX_LEN);
    if (version != w3c::SUPPORTED_VERSION) {
        return ctx;
    }

    // Validate all separator positions
    if (traceparent[w3c::SEP_AFTER_VERSION] != w3c::SEPARATOR ||
        traceparent[w3c::SEP_AFTER_TRACE_ID] != w3c::SEPARATOR ||
        traceparent[w3c::SEP_AFTER_SPAN_ID] != w3c::SEPARATOR) {
        return ctx;
    }

    // Extract trace-id and validate
    std::string_view trace_id_view = traceparent.substr(w3c::TRACE_ID_OFFSET, w3c::TRACE_ID_HEX_LEN);
    if (!w3c::is_valid_hex_string(trace_id_view)) {
        return ctx;
    }

    // Extract parent-span-id and validate
    std::string_view span_id_view = traceparent.substr(w3c::SPAN_ID_OFFSET, w3c::SPAN_ID_HEX_LEN);
    if (!w3c::is_valid_hex_string(span_id_view)) {
        return ctx;
    }

    // Trace-id must not be all zeros (per W3C spec)
    if (w3c::is_all_zeros(trace_id_view)) {
        return ctx;
    }

    // Extract trace-flags
    std::string_view flags = traceparent.substr(w3c::FLAGS_OFFSET, w3c::TRACE_FLAGS_HEX_LEN);
    ctx.sampled = (flags == w3c::SAMPLED_FLAG);

    // All validations passed - populate the context
    ctx.trace_id = std::string(trace_id_view);
    ctx.parent_span_id = std::string(span_id_view);
    ctx.valid = true;
    return ctx;
}

std::string TraceContext::to_traceparent(std::string_view span_id) const {
    if (!valid || span_id.empty()) {
        return "";
    }
    // Format: {version}-{trace-id}-{span-id}-{flags}
    std::string result;
    result.reserve(w3c::MIN_TRACEPARENT_LEN);
    result.append(w3c::SUPPORTED_VERSION);
    result.push_back(w3c::SEPARATOR);
    result.append(trace_id);
    result.push_back(w3c::SEPARATOR);
    result.append(span_id);
    result.push_back(w3c::SEPARATOR);
    result.append(sampled ? w3c::SAMPLED_FLAG : w3c::UNSAMPLED_FLAG);
    return result;
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

    // Acquire fence ensures we see all initialization that happened before
    // g_enabled was set to true. This is the hot path - atomic load is lock-free.
    if (!g_enabled.load(std::memory_order_acquire) || !g_tracer) {
        return;
    }

    // Check if we're shutting down to avoid creating spans during cleanup
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    trace::StartSpanOptions options;

    if (parent && parent->valid) {
        // Create span context from parent by converting hex strings to bytes
        uint8_t trace_id_bytes[w3c::TRACE_ID_BYTE_LEN];
        uint8_t span_id_bytes[w3c::SPAN_ID_BYTE_LEN];

        // Convert trace-id hex string to bytes
        for (size_t i = 0; i < w3c::TRACE_ID_BYTE_LEN; ++i) {
            trace_id_bytes[i] = w3c::hex_pair_to_byte(
                parent->trace_id[i * 2], parent->trace_id[i * 2 + 1]);
        }

        // Convert span-id hex string to bytes
        for (size_t i = 0; i < w3c::SPAN_ID_BYTE_LEN; ++i) {
            span_id_bytes[i] = w3c::hex_pair_to_byte(
                parent->parent_span_id[i * 2], parent->parent_span_id[i * 2 + 1]);
        }

        trace::TraceId tid(opentelemetry::nostd::span<const uint8_t, w3c::TRACE_ID_BYTE_LEN>(
            trace_id_bytes, w3c::TRACE_ID_BYTE_LEN));
        trace::SpanId sid(opentelemetry::nostd::span<const uint8_t, w3c::SPAN_ID_BYTE_LEN>(
            span_id_bytes, w3c::SPAN_ID_BYTE_LEN));
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

    char buf[w3c::SPAN_ID_BUF_SIZE];
    auto span_ctx = _impl->span->GetContext();
    span_ctx.span_id().ToLowerBase16(
        opentelemetry::nostd::span<char, w3c::SPAN_ID_HEX_LEN>{buf, w3c::SPAN_ID_HEX_LEN});
    buf[w3c::SPAN_ID_HEX_LEN] = '\0';
    return std::string(buf, w3c::SPAN_ID_HEX_LEN);
}

std::string ScopedSpan::trace_id() const {
    if (!_impl || !_impl->span) return "";

    char buf[w3c::TRACE_ID_BUF_SIZE];
    auto span_ctx = _impl->span->GetContext();
    span_ctx.trace_id().ToLowerBase16(
        opentelemetry::nostd::span<char, w3c::TRACE_ID_HEX_LEN>{buf, w3c::TRACE_ID_HEX_LEN});
    buf[w3c::TRACE_ID_HEX_LEN] = '\0';
    return std::string(buf, w3c::TRACE_ID_HEX_LEN);
}

bool ScopedSpan::is_recording() const {
    return _impl && _impl->span && _impl->span->IsRecording();
}

// ============================================================================
// TracingService implementation
// ============================================================================

void TracingService::init(const TelemetryConfig& config) {
    // std::call_once ensures this initialization runs exactly once,
    // even if multiple shards call init() concurrently at startup.
    // The lambda captures config by reference - safe because call_once
    // blocks until the initialization completes.
    std::call_once(g_init_flag, [&config]() {
        g_service_name = config.service_name;

        if (!config.enabled) {
            // Tracing disabled - leave g_enabled as false
            return;
        }

        // Create Zipkin exporter
        // Note: OTLP exporter is disabled due to protobuf static initialization issues
        // Zipkin format works with Jaeger, Tempo, and other collectors
        zipkin::ZipkinExporterOptions exporter_opts;
        std::string endpoint = config.otlp_endpoint;
        // Convert OTLP-style endpoint to Zipkin format if needed
        if (endpoint.find(":4318") != std::string::npos) {
            size_t pos = endpoint.find(":4318");
            endpoint = endpoint.substr(0, pos) + ":9411/api/v2/spans";
        } else if (endpoint.find("/v1/traces") == std::string::npos &&
                   endpoint.find("/api/v2/spans") == std::string::npos) {
            endpoint += "/api/v2/spans";
        }
        //exporter_opts.url = endpoint; // TODO: Disabled for testing
        exporter_opts.service_name = config.service_name;
        auto exporter = std::make_unique<zipkin::ZipkinExporter>(exporter_opts);

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

        // Release fence: ensure all writes above are visible to other threads
        // before they observe g_enabled == true
        g_enabled.store(true, std::memory_order_release);
    });
}

void TracingService::shutdown() {
    // First, signal that we're shutting down to prevent new spans from starting.
    // This must happen before we disable tracing so that the ScopedSpan
    // constructor's g_shutting_down check (line ~234) acts as a barrier
    // against new span creation during the shutdown sequence.
    g_shutting_down.store(true, std::memory_order_release);

    // Now atomically disable tracing. Use exchange to get the previous value
    // and only flush if tracing was actually enabled.
    bool was_enabled = g_enabled.exchange(false, std::memory_order_acq_rel);

    if (was_enabled && g_provider) {
        // Flush any pending spans before shutdown.
        g_provider->ForceFlush();

        // INTENTIONALLY do NOT call g_provider.reset() or g_tracer.reset().
        //
        // There is a TOCTOU race between the g_shutting_down check in
        // ScopedSpan's constructor (line ~234) and the actual use of g_tracer
        // at StartSpan (line ~267). A thread can observe g_shutting_down==false,
        // get preempted, then dereference g_tracer after it has been reset here.
        //
        // Letting these leak is safe:
        // 1. g_enabled is false — no new spans will be created after this point
        // 2. g_shutting_down is true — second barrier for late arrivals
        // 3. The provider/tracer remain valid so any in-flight StartSpan call
        //    that slipped past the checks still dereferences a live object
        // 4. These are process-lifetime statics; the OS reclaims them at exit
    }
}

bool TracingService::is_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

ScopedSpan TracingService::start_span(const std::string& name,
                                       const std::optional<TraceContext>& parent) {
    return ScopedSpan(name, parent);
}

ScopedSpan TracingService::start_child_span(const std::string& name) {
    return ScopedSpan(name, std::nullopt);
}

}  // namespace ranvier

#else  // RANVIER_NO_TELEMETRY not defined - use real OpenTelemetry

namespace ranvier {

// No-op stubs when OpenTelemetry is disabled at build time
TraceContext TraceContext::parse(std::string_view traceparent) {
    TraceContext ctx;
    return ctx;
}

}  // namespace ranvier

#endif  // RANVIER_NO_TELEMETRY
