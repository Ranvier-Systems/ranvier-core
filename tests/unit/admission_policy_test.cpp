// Ranvier Core - Request-Admission Policy Seam Unit Tests
//
// Reactor-free tests over the admission-policy seam: the AdmissionDecision /
// AdmissionRequest defaults, the process-wide factory slot semantics (unset =>
// none, install, reset), the pass-through (noop) Allow path, and the decision
// plumbing the controller acts on — a Reject carries the Retry-After seconds, an
// Allow carries the warning-header value, and a priority override lands the
// request in the overridden scheduler tier. The override test drives a real
// BasicRequestScheduler (dependency-free, no reactor) to prove the override is
// honoured at enqueue.
//
// What this test does NOT cover (out of scope here; needs a reactor):
//   - The http_controller consult site building the 429 reply and stamping the
//     Retry-After / X-Ranvier-Admission-Warning headers on seastar::http::reply
//   - Per-shard policy installation (Application::init_admission_policy)
// Those verify in the dev container / integration build.

#include "admission_policy.hpp"
#include "request_scheduler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

using namespace ranvier;

namespace {

// Minimal Context satisfying BasicRequestScheduler's requirements
// (priority / user_agent / agent_id / enqueue_time). Mirrors the fields the
// production ProxyContext exposes to the scheduler, without pulling in Seastar.
struct StubContext {
    PriorityLevel priority = PriorityLevel::NORMAL;
    std::string user_agent;
    std::string agent_id;
    std::chrono::steady_clock::time_point enqueue_time;
};

// Test double: returns a preset decision and records the request it saw, so a
// caller can prove decide() received the resolved inputs verbatim.
class ScriptedPolicy final : public AdmissionPolicy {
public:
    explicit ScriptedPolicy(AdmissionDecision decision) : _decision(std::move(decision)) {}
    AdmissionDecision decide(const AdmissionRequest& request) override {
        last_request = request;
        ++call_count;
        return _decision;
    }
    AdmissionRequest last_request;
    int call_count = 0;

private:
    AdmissionDecision _decision;
};

}  // namespace

// =============================================================================
// Struct defaults: Allow is the pass-through default
// =============================================================================

TEST(AdmissionDecision, DefaultsToAllowPassThrough) {
    AdmissionDecision d;
    EXPECT_EQ(d.action, AdmissionAction::Allow);
    EXPECT_EQ(d.retry_after_seconds, 1u);
    EXPECT_FALSE(d.priority_override.has_value());
    EXPECT_TRUE(d.warning_header_value.empty());
}

TEST(AdmissionRequest, DefaultsAreZeroAndUnauthenticated) {
    AdmissionRequest r;
    // Empty api_key_id is a valid, meaningful value ("no key"), not a sentinel.
    EXPECT_TRUE(r.api_key_id.empty());
    EXPECT_EQ(r.priority, PriorityLevel::NORMAL);
    EXPECT_DOUBLE_EQ(r.estimated_cost_units, 0.0);
}

// The warning header is named in the X-Ranvier-* space, matching the existing
// proxy response headers.
TEST(AdmissionPolicy, WarningHeaderNameFollowsRanvierConvention) {
    EXPECT_STREQ(kAdmissionWarningHeader, "X-Ranvier-Admission-Warning");
}

// =============================================================================
// Factory slot: unset => none; installed => that factory; reset => none again
// =============================================================================

TEST(AdmissionPolicyFactory, DefaultsToNoneWhenUnset) {
    // Ensure a clean slate (another test may have installed one).
    set_admission_policy_factory(nullptr);
    // Unset means "no policy" — an empty std::function, not a default policy.
    EXPECT_FALSE(static_cast<bool>(get_admission_policy_factory()));
}

TEST(AdmissionPolicyFactory, InstalledFactoryProducesPolicy) {
    set_admission_policy_factory([]() -> std::unique_ptr<AdmissionPolicy> {
        return std::make_unique<ScriptedPolicy>(AdmissionDecision{});
    });

    auto factory = get_admission_policy_factory();
    ASSERT_TRUE(static_cast<bool>(factory));
    auto policy = factory();
    ASSERT_NE(policy.get(), nullptr);
    EXPECT_NE(dynamic_cast<ScriptedPolicy*>(policy.get()), nullptr);

    // Reset so later tests / the process see "none" again.
    set_admission_policy_factory(nullptr);
    EXPECT_FALSE(static_cast<bool>(get_admission_policy_factory()));
}

TEST(AdmissionPolicyFactory, ResetToNullClearsInstalledFactory) {
    set_admission_policy_factory([]() -> std::unique_ptr<AdmissionPolicy> {
        return std::make_unique<ScriptedPolicy>(AdmissionDecision{});
    });
    ASSERT_TRUE(static_cast<bool>(get_admission_policy_factory()));

    set_admission_policy_factory(nullptr);
    EXPECT_FALSE(static_cast<bool>(get_admission_policy_factory()));
}

// =============================================================================
// decide(): inputs delivered verbatim, and the noop pass-through path
// =============================================================================

TEST(AdmissionPolicy, DecideReceivesResolvedInputsVerbatim) {
    ScriptedPolicy policy{AdmissionDecision{}};
    policy.decide(AdmissionRequest{"tenant-a", PriorityLevel::HIGH, 42.5});

    ASSERT_EQ(policy.call_count, 1);
    EXPECT_EQ(policy.last_request.api_key_id, "tenant-a");
    EXPECT_EQ(policy.last_request.priority, PriorityLevel::HIGH);
    EXPECT_DOUBLE_EQ(policy.last_request.estimated_cost_units, 42.5);
}

// The noop path: a default Allow decision leaves priority untouched and emits no
// warning header — the controller applies neither override nor header, so
// behaviour matches the no-policy case.
TEST(AdmissionPolicy, DefaultAllowIsPurePassThrough) {
    ScriptedPolicy policy{AdmissionDecision{}};
    const AdmissionDecision d = policy.decide(
        AdmissionRequest{"", PriorityLevel::NORMAL, 0.0});
    EXPECT_EQ(d.action, AdmissionAction::Allow);
    EXPECT_FALSE(d.priority_override.has_value());
    EXPECT_TRUE(d.warning_header_value.empty());
}

// =============================================================================
// Decision plumbing: reject -> Retry-After, warn header, priority override
// =============================================================================

// A Reject carries the retry_after_seconds the controller stamps into the
// Retry-After header on the 429.
TEST(AdmissionPolicy, RejectCarriesRetryAfterSeconds) {
    AdmissionDecision reject;
    reject.action = AdmissionAction::Reject;
    reject.retry_after_seconds = 30;

    ScriptedPolicy policy{reject};
    const AdmissionDecision d = policy.decide(
        AdmissionRequest{"quota-exhausted", PriorityLevel::LOW, 5.0});
    EXPECT_EQ(d.action, AdmissionAction::Reject);
    EXPECT_EQ(d.retry_after_seconds, 30u);
}

// An Allow may carry a warning-header value the controller emits as
// kAdmissionWarningHeader.
TEST(AdmissionPolicy, AllowCarriesWarningHeaderValue) {
    AdmissionDecision warn;
    warn.action = AdmissionAction::Allow;
    warn.warning_header_value = "approaching quota";

    ScriptedPolicy policy{warn};
    const AdmissionDecision d = policy.decide(
        AdmissionRequest{"tenant-b", PriorityLevel::NORMAL, 10.0});
    EXPECT_EQ(d.action, AdmissionAction::Allow);
    EXPECT_EQ(d.warning_header_value, "approaching quota");
}

// A priority override is honoured by the scheduler enqueue: applying it the way
// the controller does (priority = *override) lands the request in the overridden
// tier, not the classified one. CRITICAL is deliberately not special-cased.
TEST(AdmissionPolicy, PriorityOverrideHonouredBySchedulerEnqueue) {
    AdmissionDecision bump;
    bump.action = AdmissionAction::Allow;
    bump.priority_override = PriorityLevel::CRITICAL;

    ScriptedPolicy policy{bump};

    // Classified NORMAL; policy overrides to CRITICAL.
    PriorityLevel priority = PriorityLevel::NORMAL;
    const AdmissionDecision d = policy.decide(
        AdmissionRequest{"paid-tenant", priority, 3.0});
    ASSERT_EQ(d.action, AdmissionAction::Allow);
    ASSERT_TRUE(d.priority_override.has_value());
    priority = *d.priority_override;  // exactly what the controller does

    BasicRequestScheduler<StubContext> scheduler;
    auto ctx = std::make_unique<StubContext>();
    ctx->priority = priority;
    ASSERT_TRUE(scheduler.enqueue(std::move(ctx)));

    const auto depths = scheduler.queue_depths();
    EXPECT_EQ(depths[static_cast<size_t>(PriorityLevel::CRITICAL)], 1u);
    EXPECT_EQ(depths[static_cast<size_t>(PriorityLevel::NORMAL)], 0u);
}

// A decision with no override leaves the classified tier intact at enqueue.
TEST(AdmissionPolicy, NoOverrideLeavesClassifiedTierIntact) {
    PriorityLevel priority = PriorityLevel::LOW;
    AdmissionDecision d;  // default Allow, no override
    if (d.priority_override) {
        priority = *d.priority_override;
    }

    BasicRequestScheduler<StubContext> scheduler;
    auto ctx = std::make_unique<StubContext>();
    ctx->priority = priority;
    ASSERT_TRUE(scheduler.enqueue(std::move(ctx)));

    const auto depths = scheduler.queue_depths();
    EXPECT_EQ(depths[static_cast<size_t>(PriorityLevel::LOW)], 1u);
    EXPECT_EQ(depths[static_cast<size_t>(PriorityLevel::CRITICAL)], 0u);
}
