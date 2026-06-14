// Ranvier Core - GIE Endpoint-Picker plan unit tests
//
// Reactor-free, dependency-free tests over the pure EPP decision helper
// (src/gie_epp_plan.hpp): the endpoint-header formatting and the
// set-endpoint-vs-503 branch. The gRPC server itself (gie_epp_server.cpp) is
// integration-verified — these cover the content-free decision core.

#include "gie_epp_plan.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using namespace ranvier;

// =============================================================================
// epp_format_endpoint
// =============================================================================

TEST(EppFormatEndpoint, IPv4HostPort) {
    auto ep = epp_format_endpoint("10.0.0.7", 8000);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(*ep, "10.0.0.7:8000");
}

TEST(EppFormatEndpoint, IPv6IsBracketed) {
    // "<ip>:<port>" is ambiguous for IPv6; the literal must be bracketed so the
    // data plane parses host and port unambiguously.
    auto ep = epp_format_endpoint("2001:db8::1", 8000);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(*ep, "[2001:db8::1]:8000");
}

TEST(EppFormatEndpoint, EmptyIpIsNullopt) {
    EXPECT_FALSE(epp_format_endpoint("", 8000).has_value());
}

// =============================================================================
// epp_plan: set-endpoint vs ImmediateResponse 503
// =============================================================================

TEST(EppPlan, BackendWithEndpointSetsHeader) {
    auto plan = epp_plan(/*have_backend=*/true, std::optional<std::string>("10.0.0.7:8000"));
    EXPECT_EQ(plan.kind, EppResponsePlan::Kind::SetEndpoint);
    EXPECT_EQ(plan.endpoint, "10.0.0.7:8000");
}

TEST(EppPlan, NoBackendIs503) {
    auto plan = epp_plan(/*have_backend=*/false, std::nullopt);
    EXPECT_EQ(plan.kind, EppResponsePlan::Kind::ImmediateUnavailable);
    EXPECT_EQ(plan.http_status, 503);
}

TEST(EppPlan, BackendButUnresolvedEndpointIs503) {
    // Routing picked a backend but its address could not be resolved — the
    // gateway must not be handed an empty endpoint; fall to 503.
    auto plan = epp_plan(/*have_backend=*/true, std::nullopt);
    EXPECT_EQ(plan.kind, EppResponsePlan::Kind::ImmediateUnavailable);
    EXPECT_EQ(plan.http_status, 503);
}

TEST(EppPlan, BackendWithEmptyEndpointIs503) {
    auto plan = epp_plan(/*have_backend=*/true, std::optional<std::string>(""));
    EXPECT_EQ(plan.kind, EppResponsePlan::Kind::ImmediateUnavailable);
}

TEST(EppPlan, DestinationHeaderNameMatchesGieSpec) {
    // The data plane keys on this exact header; a typo silently breaks routing.
    EXPECT_STREQ(kGieDestinationEndpointHeader, "x-gateway-destination-endpoint");
}
