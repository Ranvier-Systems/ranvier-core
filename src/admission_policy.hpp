// Ranvier Core - Request-Admission Policy Seam
//
// Pluggable decision point for embedder-owned admission policies — quota
// systems, tenant-tier products, priority rewriters — consulted once per
// proxied request. This continues the embeddability series (telemetry sink,
// usage-ledger sink, downstream-service lifecycle) with a per-request decision
// hook, so embedders can gate or re-prioritise traffic without forking the
// controller.
//
// =============================================================================
// CONTRACT (binding on all implementations)
// =============================================================================
//
//   - decide() is consulted once per proxied request, on the shard handling it,
//     AFTER priority classification and BEFORE backpressure / scheduler
//     admission — so a Reject short-circuits before any queue slot is consumed
//     and a priority override participates in tier queueing.
//
//   - decide() MUST be synchronous, cheap, and lock-free on the caller's shard.
//     It runs inline on the request hot path; a stall here stalls that shard.
//     The instance is per-shard (the factory is invoked once per shard, see
//     below), so a policy needing process-wide state (e.g. a shared quota
//     counter) must keep a shard-local view and reconcile it OFF the hot path
//     rather than taking a cross-shard lock here.
//
//   - decide() MUST NOT throw (Hard Rule #22). It is called inline, not behind
//     a future, so an escaping exception bypasses the request's future-based
//     error handling. Swallow internal errors and fail open (return Allow).
//
//   - CRITICAL requests are passed to decide() too. The policy sees the
//     priority and decides how to treat it; core does not hard-code that a
//     CRITICAL request is always admitted.

#pragma once

#include "request_scheduler.hpp"  // PriorityLevel

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ranvier {

// Decision verb. Allow is the pass-through default.
enum class AdmissionAction : uint8_t {
    Allow  = 0,
    Reject = 1,
};

// Response header carrying an Allow-path advisory string back to the client.
// Named in the X-Ranvier-* space to match the existing proxy response headers
// (X-Request-ID, X-Backend-ID, X-Routing-Mode). Emitted only when a decision
// supplies a non-empty warning value — e.g. a quota system nearing its limit.
inline constexpr const char* kAdmissionWarningHeader = "X-Ranvier-Admission-Warning";

// What the policy sees. Every field is resolved before the call:
//   - api_key_id: the request's resolved ApiKey::name. Empty is a valid,
//     meaningful value ("arrived without authentication on the data plane" —
//     see docs/architecture/per-api-key-attribution.md); it is not a sentinel.
//   - priority: the PriorityLevel classified by extract_priority (CRITICAL
//     included — see the contract).
//   - estimated_cost_units: the pre-route cost heuristic (the same value that
//     feeds cost-based priority classification), before any routing decision.
struct AdmissionRequest {
    std::string   api_key_id;
    PriorityLevel priority = PriorityLevel::NORMAL;
    double        estimated_cost_units = 0.0;
};

// What the policy returns. Allow with all optionals unset is a pure
// pass-through; the optional fields are inert unless populated.
struct AdmissionDecision {
    AdmissionAction action = AdmissionAction::Allow;

    // Reject only: surfaced by the controller as the Retry-After header on the
    // 429 response. Ignored on Allow. A quota system sets this to the seconds
    // until the tenant's window refills.
    uint32_t retry_after_seconds = 1;

    // Allow only: overrides the enqueue priority so the request queues in a
    // different tier (e.g. a tenant-tier product promoting a paid key to HIGH,
    // or a priority rewriter demoting a batch key to LOW). Applied before the
    // scheduler sees the request. nullopt leaves the classified priority intact.
    std::optional<PriorityLevel> priority_override;

    // Allow only: value for the kAdmissionWarningHeader response header (e.g.
    // "approaching quota"). Empty => no header emitted.
    std::string warning_header_value;
};

// Abstract policy. See the contract above.
class AdmissionPolicy {
public:
    virtual ~AdmissionPolicy() = default;

    // Consulted once per proxied request. Synchronous, non-throwing, cheap.
    virtual AdmissionDecision decide(const AdmissionRequest& request) = 0;
};

// Process-wide seam for choosing which policy each shard installs at start-up.
// Mirrors the downstream-service factory (and unlike the sink factories, there
// is NO default policy): OSS leaves it unset, so no policy object is created and
// the per-request path is a single null check — behaviour bit-for-bit unchanged.
// A downstream build installs a policy via set_admission_policy_factory() during
// its own start-up, BEFORE Application::init_admission_policy() invokes the
// factory on each shard. An empty factory resets to "none".
//
// The factory is invoked once PER SHARD (admission decisions happen on every
// shard; a shard-0-only policy would force a cross-shard hop per request); each
// invocation yields that shard's own instance so decide() is always a
// shard-local call. Implementations needing a single shared backend should hide
// it behind the per-shard front-ends (see contract).
//
// Not racy runtime state (cf. Hard Rule #11, mirroring the telemetry/usage-ledger
// factory seams): the slot is written once on shard 0 during single-threaded
// start-up and only read afterwards (once per shard, during the start-up
// broadcast); it is never touched on the hot path. The function-local static
// supplies the standard thread-safe one-time init of the slot itself.
using AdmissionPolicyFactory = std::function<std::unique_ptr<AdmissionPolicy>()>;

// Shared storage for the factory seam; prefer the set/get accessors below.
inline AdmissionPolicyFactory& admission_policy_factory_slot() {
    static AdmissionPolicyFactory slot;  // empty until a build installs one
    return slot;
}

inline void set_admission_policy_factory(AdmissionPolicyFactory factory) {
    admission_policy_factory_slot() = std::move(factory);
}

// The installed factory, or an empty std::function when unset. Like the
// downstream-service seam (and unlike the sink factories) there is no default
// policy: an empty result means "no policy", the unchanged-behaviour path.
inline AdmissionPolicyFactory get_admission_policy_factory() {
    return admission_policy_factory_slot();
}

}  // namespace ranvier
