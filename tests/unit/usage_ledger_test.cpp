// Ranvier Core - Usage-Ledger Sink Unit Tests
//
// Reactor-free tests over the usage-ledger seam: UsageEvent defaults +
// forward-compat readability (the wire contract — see
// src/usage_ledger_schema.hpp §"FORWARD-COMPATIBILITY CONTRACT"), the
// NoopUsageLedgerSink default, the process-wide factory seam, and that a real
// sink double receives the event verbatim through record(). Uses the seastar
// stubs in tests/stubs so this test does NOT need a running reactor.
//
// What this test does NOT cover (out of scope here; needs a reactor):
//   - The http_controller terminal-path build + record() call site
//   - Per-shard sink installation (Application::init_usage_ledger)
//   - flush() draining a real buffering sink at shutdown
// Those live in the integration build and the operator's Docker test loop.

#include "usage_ledger_schema.hpp"
#include "usage_ledger_sink.hpp"
#include "types.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace ranvier;

// =============================================================================
// UsageEvent defaults + forward-compatibility
// =============================================================================

TEST(UsageEvent, DefaultsCarryCurrentFormatVersion) {
    UsageEvent ev;
    EXPECT_EQ(ev.format_version, kUsageEventFormatVersion);
    // Empty/zero defaults — the "not reported" semantic.
    EXPECT_TRUE(ev.api_key_id.empty());
    EXPECT_TRUE(ev.request_id.empty());
    EXPECT_TRUE(ev.endpoint.empty());
    EXPECT_TRUE(ev.model.empty());
    EXPECT_FALSE(ev.backend_id.has_value());
    EXPECT_EQ(ev.timestamp_ms, 0);
    EXPECT_EQ(ev.status_code, 0);
    EXPECT_EQ(ev.latency_ms, 0);
    EXPECT_EQ(ev.input_tokens, 0);
    EXPECT_EQ(ev.output_tokens, 0);
    EXPECT_DOUBLE_EQ(ev.cost_units, 0.0);
}

// The forward-compatibility contract: a consumer reading an event whose
// format_version is newer than it knows MUST still read the fields it knows.
// version is recorded, never a rejection boundary.
TEST(UsageEvent, FieldsRemainReadableWhenVersionBumped) {
    UsageEvent ev;
    ev.format_version = 999;  // pretend a future producer
    ev.api_key_id = "production-deploy";
    ev.request_id = "req-abc";
    ev.endpoint = "/v1/chat/completions";
    ev.model = "meta-llama/Llama-3.1-8B-Instruct";
    ev.backend_id = static_cast<BackendId>(7);
    ev.input_tokens = 128;
    ev.output_tokens = 256;
    ev.cost_units = 51.5;

    // A v1 reader still sees every known field unchanged.
    EXPECT_EQ(ev.api_key_id, "production-deploy");
    EXPECT_EQ(ev.request_id, "req-abc");
    EXPECT_EQ(ev.endpoint, "/v1/chat/completions");
    EXPECT_EQ(ev.model, "meta-llama/Llama-3.1-8B-Instruct");
    ASSERT_TRUE(ev.backend_id.has_value());
    EXPECT_EQ(*ev.backend_id, static_cast<BackendId>(7));
    EXPECT_EQ(ev.input_tokens, 128);
    EXPECT_EQ(ev.output_tokens, 256);
    EXPECT_DOUBLE_EQ(ev.cost_units, 51.5);
}

TEST(UsageEvent, UnauthenticatedKeyIsEmptyNotSentinel) {
    // "" is a valid, meaningful api_key_id — "usage that arrived without a key".
    // The seam carries it verbatim; it does not substitute a sentinel.
    UsageEvent ev;
    EXPECT_TRUE(ev.api_key_id.empty());
}

// =============================================================================
// NoopUsageLedgerSink: default sink discards every event
// =============================================================================

TEST(NoopUsageLedgerSink, IsConstructibleAndPolymorphic) {
    std::unique_ptr<UsageLedgerSink> sink = std::make_unique<NoopUsageLedgerSink>();
    EXPECT_NE(sink.get(), nullptr);
}

TEST(NoopUsageLedgerSink, RecordIsCallableAndDiscards) {
    NoopUsageLedgerSink sink;
    UsageEvent ev;
    ev.api_key_id = "alice";
    // Must accept and return without throwing; nothing observable happens.
    EXPECT_NO_THROW(sink.record(std::move(ev)));
}

TEST(NoopUsageLedgerSink, FlushReturnsFuture) {
    NoopUsageLedgerSink sink;
    // Just verify the call compiles and returns. The stubbed seastar::future
    // is not driven by a reactor here, so we do not .get() it.
    auto fut = sink.flush();
    (void)fut;
}

TEST(NoopUsageLedgerSink, FactoryReturnsNoop) {
    auto sink = make_default_usage_ledger_sink();
    ASSERT_NE(sink.get(), nullptr);
    EXPECT_NE(dynamic_cast<NoopUsageLedgerSink*>(sink.get()), nullptr);
}

// =============================================================================
// Factory seam: OSS leaves it unset (-> Noop); a downstream build installs one
// =============================================================================

// Test double, distinguishable from Noop, that captures what it receives — this
// is the behavioural proof that record() delivers the event to the installed
// sink with every field intact.
class CapturingSink final : public UsageLedgerSink {
public:
    void record(UsageEvent event) override { events.push_back(std::move(event)); }
    std::vector<UsageEvent> events;
};

TEST(UsageLedgerSinkFactory, DefaultsToNoopWhenUnset) {
    // Ensure a clean slate (another test may have installed one).
    set_usage_ledger_sink_factory(nullptr);
    auto sink = get_usage_ledger_sink_factory()();
    ASSERT_NE(sink.get(), nullptr);
    EXPECT_NE(dynamic_cast<NoopUsageLedgerSink*>(sink.get()), nullptr);
}

TEST(UsageLedgerSinkFactory, InstalledFactoryOverridesDefault) {
    set_usage_ledger_sink_factory([]() -> std::unique_ptr<UsageLedgerSink> {
        return std::make_unique<CapturingSink>();
    });
    auto sink = get_usage_ledger_sink_factory()();
    EXPECT_NE(dynamic_cast<CapturingSink*>(sink.get()), nullptr);
    EXPECT_EQ(dynamic_cast<NoopUsageLedgerSink*>(sink.get()), nullptr);

    // Reset so later tests / the process see the default again.
    set_usage_ledger_sink_factory(nullptr);
    EXPECT_NE(dynamic_cast<NoopUsageLedgerSink*>(get_usage_ledger_sink_factory()().get()),
              nullptr);
}

TEST(UsageLedgerSink, RecordDeliversEventVerbatim) {
    CapturingSink sink;
    UsageEvent ev;
    ev.api_key_id = "bob";
    ev.request_id = "req-42";
    ev.endpoint = "/v1/completions";
    ev.model = "qwen2";
    ev.backend_id = static_cast<BackendId>(3);
    ev.timestamp_ms = 1714608000000;
    ev.status_code = 200;
    ev.latency_ms = 240;
    ev.input_tokens = 100;
    ev.output_tokens = 200;
    ev.cost_units = 12.5;

    sink.record(std::move(ev));

    ASSERT_EQ(sink.events.size(), 1u);
    const auto& got = sink.events.front();
    EXPECT_EQ(got.api_key_id, "bob");
    EXPECT_EQ(got.request_id, "req-42");
    EXPECT_EQ(got.endpoint, "/v1/completions");
    EXPECT_EQ(got.model, "qwen2");
    ASSERT_TRUE(got.backend_id.has_value());
    EXPECT_EQ(*got.backend_id, static_cast<BackendId>(3));
    EXPECT_EQ(got.timestamp_ms, 1714608000000);
    EXPECT_EQ(got.status_code, 200);
    EXPECT_EQ(got.latency_ms, 240);
    EXPECT_EQ(got.input_tokens, 100);
    EXPECT_EQ(got.output_tokens, 200);
    EXPECT_DOUBLE_EQ(got.cost_units, 12.5);
}
