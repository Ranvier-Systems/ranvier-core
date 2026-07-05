// Ranvier Core - Usage-Ledger Event Schema
//
// Data structure for the pluggable usage-ledger sink: one per-request usage
// event, handed to the sink at request completion.
//
// SCOPE: per-API-key usage metering only. The event carries the attribution
// identifiers (api_key_id, request_id) that ARE the point of a usage ledger —
// this is the deliberate difference from telemetry_schema.hpp, which is a
// content-free *aggregate* with no identifiers. Even so, a usage event carries
// NO prompt or response content and NO token IDs: only the identifiers plus the
// structural usage figures (token counts, cost, status, latency) needed to
// meter, bill, or attribute. Anything richer belongs outside core.
//
// NOTE ON TOKEN COUNTS: input_tokens / output_tokens / cost_units are the
// engine's authoritative response `usage` when it was available (snooped from
// the response during streaming), and otherwise the pre-flight estimates the
// per-API-key attribution path computes (CostEstimate: input from
// tokenized/char heuristics, output from the request's max_tokens or a
// multiplier). The `tokens_estimated` flag below says which — a streaming
// response without stream_options.include_usage carries no usage, so those fall
// back to estimates. Billing consumers should branch on `tokens_estimated`
// rather than assume. See docs/architecture/response-usage-accounting.md.
//
// =============================================================================
// FORWARD-COMPATIBILITY CONTRACT
// =============================================================================
// `UsageEvent` follows the same forward-compatibility discipline as
// `WindowReport`/`CacheStatePacket`. A sink built against format version N MUST
// remain correct when fed an event at format version N+M:
//
//   1. `UsageEvent::format_version` is recorded but is NOT a rejection boundary
//      — consumers read the fields they know and ignore the rest.
//   2. New fields are appended only. Never re-purpose an existing field, never
//      shrink a field's semantic range, never renumber BackendType ordinals
//      (see types.hpp).
//   3. A consumer reading an older event sees zero-valued / empty defaults for
//      fields it expected but that were not set — the correct "not reported"
//      semantic.
//   4. Bump `kUsageEventFormatVersion` only when readers ALREADY DEPLOYED in
//      the wild may misinterpret the event — essentially never under the
//      append-only rule. It is an escape hatch, not a "what's new" knob.
//
// This header is the contract; downstream sink implementations are bound by it.

#pragma once

#include "intent_classifier.hpp"   // RequestIntent
#include "types.hpp"               // BackendId, BackendType

#include <cstdint>
#include <optional>
#include <string>

namespace ranvier {

// Current schema version of UsageEvent. See the FORWARD-COMPATIBILITY CONTRACT
// above before bumping.
//
//   v1: initial usage-event schema.
inline constexpr uint16_t kUsageEventFormatVersion = 1;

// Request-intent attribution for a usage event. Lets ledger sinks bucket usage
// by request type (autocomplete vs chat vs edit) without any content.
//
// WIRE INVARIANT: ordinals are pinned, append-only, and NEVER renumbered — same
// discipline as BackendType (see types.hpp). They are DELIBERATELY independent
// of RequestIntent's ordinals: 0 is reserved for UNSPECIFIED, the "not reported"
// value an older event (or one built before intent classification ran) decodes
// to. Append new classes at the end; never reuse or renumber an ordinal.
enum class UsageIntentClass : uint8_t {
    UNSPECIFIED  = 0,  // not reported — the append-only decode default
    AUTOCOMPLETE = 1,
    CHAT         = 2,
    EDIT         = 3,
};

// Map the request's classified intent to the ledger's pinned wire ordinal. Sole
// source of truth for the RequestIntent -> UsageIntentClass correspondence; a
// future RequestIntent with no case here decodes to UNSPECIFIED rather than a
// wrong class (and the switch keeps -Wreturn-type honest).
inline UsageIntentClass to_usage_intent_class(RequestIntent intent) {
    switch (intent) {
        case RequestIntent::AUTOCOMPLETE: return UsageIntentClass::AUTOCOMPLETE;
        case RequestIntent::CHAT:         return UsageIntentClass::CHAT;
        case RequestIntent::EDIT:         return UsageIntentClass::EDIT;
    }
    return UsageIntentClass::UNSPECIFIED;
}

// =============================================================================
// UsageEvent
// =============================================================================
//
// One per completed request, built at the http_controller terminal phase next
// to the existing request_attribution enqueue. A flat POD of identifiers +
// usage figures so the hot-path build is a handful of string moves and integer
// copies. See the scope and honesty notes at the top of this file.
struct UsageEvent {
    // Forward-compat: recorded, NOT a rejection boundary. See header notes.
    uint16_t format_version = kUsageEventFormatVersion;

    // ---- Attribution identifiers (the point of the ledger) ----
    // ApiKey.name, or "" for an unauthenticated/invalid data-plane request
    // (see docs/architecture/per-api-key-attribution.md §5). Empty is a valid,
    // meaningful value — "usage that arrived without a key".
    std::string api_key_id;
    // Correlation id for this request (also on the trace span / X-Request-ID).
    std::string request_id;

    // ---- Request shape (all content-free) ----
    std::string endpoint;   // "/v1/chat/completions" | "/v1/completions"
    std::string model;      // client-requested model string ("" if absent)
    // Selected backend. Unset on early failures (no backend was chosen).
    std::optional<BackendId> backend_id;

    // ---- Timing / outcome ----
    int64_t timestamp_ms = 0;   // wall-clock ms at request start
    int32_t status_code  = 0;   // backend HTTP status, or synthesized proxy code
    int32_t latency_ms   = 0;   // end-to-end request latency

    // ---- Usage figures ----
    int64_t input_tokens  = 0;
    int64_t output_tokens = 0;
    double  cost_units    = 0.0;

    // True when the figures above are pre-flight ESTIMATES, false when they are
    // the engine's authoritative response `usage` (forward-compat append; see
    // the HONESTY NOTE at the top of this file). A billing consumer should check
    // this rather than assume — estimates and actuals can both appear depending
    // on whether the response carried usage.
    bool tokens_estimated = true;

    // Classified request intent (forward-compat append; see the CONTRACT above).
    // UNSPECIFIED when intent classification was disabled for the request or the
    // event predates this field — a sink attributing by intent should treat
    // UNSPECIFIED as "not reported", not as a fourth class.
    UsageIntentClass intent_class = UsageIntentClass::UNSPECIFIED;
};

}  // namespace ranvier
