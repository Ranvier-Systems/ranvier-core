// Ranvier Core - GIE Endpoint-Picker decision plan (pure)
//
// The content-free, dependency-free core of the EPP response decision: given
// whether routing produced a backend and that backend's resolved "<ip:port>"
// endpoint, decide what the ext_proc response must say. Kept out of
// gie_epp_server.{hpp,cpp} so it carries NO gRPC / protobuf / Seastar / config
// dependency and can be unit-tested without any of them (the gRPC server itself
// is integration-verified).
//
// This mirrors the GIE Endpoint-Picker protocol's two outcomes:
//   - a backend was picked  -> set the x-gateway-destination-endpoint header
//   - no ready endpoint      -> ImmediateResponse 503 (Service Unavailable)
// (429 request-shedding and multi-endpoint fallback are PR-2.)

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ranvier {

// The HTTP header the data plane reads to learn the chosen backend(s).
// Spec: gateway-api-inference-extension docs/proposals/004-endpoint-picker-protocol.
inline constexpr const char* kGieDestinationEndpointHeader =
    "x-gateway-destination-endpoint";

// What the ext_proc handler should emit for one request-headers phase.
struct EppResponsePlan {
    enum class Kind {
        SetEndpoint,           // add kGieDestinationEndpointHeader = endpoint
        ImmediateUnavailable,  // ImmediateResponse with http_status
    };
    Kind kind = Kind::ImmediateUnavailable;
    std::string endpoint;       // "<ip:port>" — valid when kind == SetEndpoint
    int http_status = 503;      // valid when kind == ImmediateUnavailable
};

// Format the destination-endpoint header value from a resolved address.
// IPv6 literals are bracketed so "<ip>:<port>" parses unambiguously, matching
// the data plane's expectation. Returns nullopt if the inputs are unusable.
inline std::optional<std::string> epp_format_endpoint(const std::string& ip, uint16_t port) {
    if (ip.empty()) {
        return std::nullopt;
    }
    const bool is_ipv6 = ip.find(':') != std::string::npos;
    if (is_ipv6) {
        return "[" + ip + "]:" + std::to_string(port);
    }
    return ip + ":" + std::to_string(port);
}

// Decide the EPP response. `endpoint` is the resolved "<ip:port>" string for the
// routed backend, or nullopt when routing found no backend / the address could
// not be resolved. No backend -> 503 (the GIE "no ready endpoints" outcome).
inline EppResponsePlan epp_plan(bool have_backend, const std::optional<std::string>& endpoint) {
    EppResponsePlan plan;
    if (have_backend && endpoint.has_value() && !endpoint->empty()) {
        plan.kind = EppResponsePlan::Kind::SetEndpoint;
        plan.endpoint = *endpoint;
        return plan;
    }
    plan.kind = EppResponsePlan::Kind::ImmediateUnavailable;
    plan.http_status = 503;  // Service Unavailable — no ready endpoint
    return plan;
}

}  // namespace ranvier
