// Ranvier Core - GIE Endpoint-Picker plan unit tests
//
// Reactor-free, dependency-free tests over the pure EPP helpers
// (src/gie_epp_plan.hpp): endpoint-header formatting, the set-endpoint-vs-503
// branch, and the Rule #4 bounded body-append cap. The gRPC server itself
// (gie_epp_server.cpp) is integration-verified — these cover the pure logic.

#include "gie_epp_plan.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

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

// =============================================================================
// epp_append_bounded: Rule #4 cap on the buffered request body
// =============================================================================

TEST(EppAppendBounded, UnderCapAppendsFully) {
    std::string buf = "ab";
    size_t n = epp_append_bounded(buf, "cde", /*cap=*/10);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(buf, "abcde");
}

TEST(EppAppendBounded, CrossingCapTruncatesToRoom) {
    std::string buf = "abcd";          // 4 bytes, cap 6 -> room 2
    size_t n = epp_append_bounded(buf, "xyz", /*cap=*/6);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(buf, "abcdxy");
    EXPECT_EQ(buf.size(), 6u);
}

TEST(EppAppendBounded, ExactlyAtCapAppendsNothing) {
    std::string buf = "abcdef";        // already at cap
    size_t n = epp_append_bounded(buf, "more", /*cap=*/6);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(buf, "abcdef");
}

TEST(EppAppendBounded, FillsExactlyToCap) {
    std::string buf = "abcd";
    size_t n = epp_append_bounded(buf, "ef", /*cap=*/6);  // room 2, chunk 2
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(buf, "abcdef");
}

TEST(EppAppendBounded, MultipleChunksRespectCapAcrossCalls) {
    std::string buf;
    const size_t cap = 5;
    EXPECT_EQ(epp_append_bounded(buf, "abc", cap), 3u);   // buf=abc
    EXPECT_EQ(epp_append_bounded(buf, "defg", cap), 2u);  // buf=abcde (room 2)
    EXPECT_EQ(epp_append_bounded(buf, "hij", cap), 0u);   // already full
    EXPECT_EQ(buf, "abcde");
    EXPECT_EQ(buf.size(), cap);
}

TEST(EppAppendBounded, EmptyChunkIsNoOp) {
    std::string buf = "abc";
    EXPECT_EQ(epp_append_bounded(buf, "", /*cap=*/10), 0u);
    EXPECT_EQ(buf, "abc");
}

TEST(EppAppendBounded, BinarySafeWithEmbeddedNuls) {
    std::string buf;
    std::string chunk("a\0b", 3);  // contains an embedded NUL
    size_t n = epp_append_bounded(buf, std::string_view(chunk.data(), chunk.size()), 10);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf, chunk);
}
