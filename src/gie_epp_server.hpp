// Ranvier Core - GIE Endpoint-Picker (EPP) ext_proc gRPC server
//
// Exposes the routing core as a Gateway API Inference Extension (GIE)
// Endpoint-Picker: a gRPC `envoy.service.ext_proc.v3.ExternalProcessor` server
// that a GIE-conformant gateway delegates endpoint selection to. The picker
// returns the chosen backend via the `x-gateway-destination-endpoint` response
// header (GIE docs/proposals/004-endpoint-picker-protocol). A compatibility
// mode alongside — not a replacement for — the standalone inline data plane.
//
// Compiled only WITH_GIE_EPP=ON (default OFF; gRPC is a heavy dependency). All
// gRPC / generated-protobuf types are confined to gie_epp_server.cpp behind a
// PIMPL, so this header — included by application.cpp — pulls in NO gRPC.
//
// Threading model (Rule #12 — kv_event_subscriber / tokenizer_thread_pool are
// the templates):
//
//   GIE gateway ──gRPC──► ExternalProcessor::Process   (gRPC sync thread pool)
//                           read request_headers
//                           alien::submit_to(shard 0) ─► route_request()  (reactor)
//                                                        get_backend_address()
//                           ◄──── EppDecision (trivially copyable, no heap) ──┘
//                           write x-gateway-destination-endpoint  (or 503)
//
// The gRPC server runs on its OWN threads — never the reactor. The handler
// thread blocks on the std::future returned by seastar::alien::submit_to to get
// the routing decision; that is the request/response inverse of the KV
// subscriber's fire-and-forget alien::run_on. The decision crosses the boundary
// as a trivially-copyable POD (a socket_address + status), so no Seastar-shard
// heap is ever freed on a gRPC thread (Rules #14/#15).
//
// PR-1 scope (BACKLOG §20.2 P1.4): the bridge + a conformant picker that routes
// with empty tokens (live load/hash selection) and returns the endpoint, or 503
// when no backend is ready. PR-2 adds body capture + tokenization for full
// prefix-aware routing, dynamic_metadata, and 429 shedding.

#pragma once

#include "config.hpp"        // GieEppConfig
#include "chat_template.hpp"  // ChatTemplate (value member; needs the full type)

#include <memory>

#include <seastar/core/alien.hh>
#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>

namespace ranvier {

class RouterService;
class TokenizerService;

class GieEppServer {
public:
    // `router` and `tokenizer` are borrowed (not owned); both must outlive the
    // server (the Application stops this service before tearing them down).
    // `chat_template` is copied — the EPP tokenizes prompts with the same
    // template the inline path uses so token sequences align with the backend
    // (and thus with the KV-event-fed residency index). PR-2 (BACKLOG §20.2 P1.4).
    GieEppServer(const GieEppConfig& cfg, RouterService* router,
                 seastar::sharded<TokenizerService>* tokenizer,
                 ChatTemplate chat_template);
    ~GieEppServer();

    GieEppServer(const GieEppServer&) = delete;
    GieEppServer& operator=(const GieEppServer&) = delete;

    // Build and start the gRPC server (binds the port, spawns gRPC's own
    // threads). Call once from shard 0 after RouterService is up. `alien` is the
    // bridge handlers use to run routing decisions on the reactor.
    void start(seastar::alien::instance& alien);

    // Graceful shutdown: stop accepting, drain in-flight RPCs (bounded), join.
    // Must complete before RouterService teardown (handlers call into it).
    seastar::future<> stop();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace ranvier
