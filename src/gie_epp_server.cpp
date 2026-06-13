// Ranvier Core - GIE Endpoint-Picker (EPP) ext_proc gRPC server implementation
//
// See gie_epp_server.hpp for the threading model and scope. All gRPC and
// generated-protobuf types are confined to this translation unit; it is compiled
// only WITH_GIE_EPP=ON.

#include "gie_epp_server.hpp"

#include "gie_epp_plan.hpp"
#include "logging.hpp"
#include "router_service.hpp"

// Generated from proto/ext_proc_min.proto (build-time protoc + grpc_cpp_plugin;
// include dir wired in CMakeLists.txt under WITH_GIE_EPP).
#include "ext_proc_min.pb.h"
#include "ext_proc_min.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <fmt/format.h>

#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <seastar/util/log.hh>

namespace ranvier {

namespace epp = envoy::service::ext_proc::v3;

static seastar::logger log_epp("gie_epp");

namespace {

// Routing decision crossed back from the reactor to a gRPC handler thread.
// Trivially copyable on purpose: a socket_address is a sockaddr_storage union
// with a trivial destructor, so nothing heap-allocated on a Seastar shard is
// ever freed on a gRPC thread (Rules #14/#15). The endpoint string is formatted
// on the gRPC thread from this POD.
struct EppDecision {
    bool ok = false;                 // true -> addr is a routed backend
    seastar::socket_address addr;    // valid when ok
};

// The ext_proc ExternalProcessor service. One Process() bidi stream per request;
// the gateway sends request phases and we return the chosen endpoint (or 503).
class ExtProcServiceImpl final : public epp::ExternalProcessor::Service {
public:
    ExtProcServiceImpl(RouterService* router, seastar::alien::instance* alien)
        : _router(router), _alien(alien) {}

    grpc::Status Process(
        grpc::ServerContext* /*context*/,
        grpc::ServerReaderWriter<epp::ProcessingResponse, epp::ProcessingRequest>* stream)
        override {
        epp::ProcessingRequest request;
        while (stream->Read(&request)) {
            epp::ProcessingResponse response;
            if (request.has_request_headers()) {
                build_headers_response(response);
            } else if (request.has_request_body()) {
                // PR-1 routes at the header phase; just CONTINUE the body so a
                // gateway configured to stream the body to the picker is not
                // stalled. PR-2 will tokenize the body here for prefix routing.
                response.mutable_request_body()->mutable_response()->set_status(
                    epp::CommonResponse::CONTINUE);
            } else {
                // A phase we don't act on in PR-1 (response_* should not reach a
                // request-path picker). Close the stream cleanly.
                break;
            }
            if (!stream->Write(response)) {
                break;  // gateway hung up
            }
            request.Clear();
        }
        return grpc::Status::OK;
    }

private:
    // Build the request_headers response: either set the destination-endpoint
    // header or emit an ImmediateResponse 503 when no backend is ready.
    void build_headers_response(epp::ProcessingResponse& response) {
        const EppDecision decision = decide();
        std::optional<std::string> endpoint;
        if (decision.ok) {
            endpoint = epp_format_endpoint(
                fmt::format("{}", decision.addr.addr()), decision.addr.port());
        }
        const EppResponsePlan plan = epp_plan(decision.ok, endpoint);

        if (plan.kind == EppResponsePlan::Kind::SetEndpoint) {
            auto* common = response.mutable_request_headers()->mutable_response();
            common->set_status(epp::CommonResponse::CONTINUE);
            // Force the data plane to re-run endpoint selection against the
            // header we just set (otherwise the original route stands).
            common->set_clear_route_cache(true);
            auto* opt = common->mutable_header_mutation()->add_set_headers();
            opt->set_append_action(epp::HeaderValueOption::OVERWRITE_IF_EXISTS_OR_ADD);
            auto* hv = opt->mutable_header();
            hv->set_key(kGieDestinationEndpointHeader);
            hv->set_value(plan.endpoint);
        } else {
            auto* imm = response.mutable_immediate_response();
            imm->mutable_status()->set_code(epp::ServiceUnavailable);
            imm->set_details("ranvier: no ready endpoint");
        }
    }

    // Run the routing decision on the reactor (shard 0) and block this gRPC
    // handler thread for the result. PR-1 routes with empty tokens — live
    // load/hash selection over registered backends; PR-2 adds body tokenization
    // for prefix-aware routing. Reuses the entire reactor-side pipeline rather
    // than reimplementing routing off-reactor.
    EppDecision decide() {
        try {
            std::future<EppDecision> fut = seastar::alien::submit_to(
                *_alien, 0u, [router = _router]() -> seastar::future<EppDecision> {
                    EppDecision d;
                    const std::vector<int32_t> no_tokens;
                    RouteResult rr = router->route_request(no_tokens, std::string(), 0, 0.0);
                    if (rr.backend_id.has_value()) {
                        auto addr = router->get_backend_address(*rr.backend_id);
                        if (addr.has_value()) {
                            d.ok = true;
                            d.addr = *addr;
                        }
                    }
                    return seastar::make_ready_future<EppDecision>(d);
                });
            return fut.get();
        } catch (const std::exception& e) {
            log_epp.warn("EPP routing bridge failed: {}", e.what());
            return EppDecision{};  // ok=false -> 503
        } catch (...) {
            log_epp.warn("EPP routing bridge failed: unknown error");
            return EppDecision{};
        }
    }

    RouterService* _router;
    seastar::alien::instance* _alien;
};

}  // namespace

// ---------------------------------------------------------------------------
// PIMPL
// ---------------------------------------------------------------------------

struct GieEppServer::Impl {
    GieEppConfig cfg;
    RouterService* router = nullptr;
    seastar::alien::instance* alien = nullptr;
    std::unique_ptr<ExtProcServiceImpl> service;
    std::unique_ptr<grpc::Server> server;
    // gRPC drain runs here, off the reactor (Rule #12). Resolved back onto
    // shard 0 via alien::run_on (see stop()).
    seastar::promise<> shutdown_promise;
    std::thread shutdown_thread;

    ~Impl() {
        // Rule #13: explicit thread teardown. The ~Impl body runs before member
        // destruction, so the thread (which uses `server`) is joined before
        // `server` is destroyed. By normal stop() the thread is already done.
        if (shutdown_thread.joinable()) {
            shutdown_thread.join();
        }
    }
};

GieEppServer::GieEppServer(const GieEppConfig& cfg, RouterService* router)
    : _impl(std::make_unique<Impl>()) {
    _impl->cfg = cfg;
    _impl->router = router;
}

GieEppServer::~GieEppServer() = default;

void GieEppServer::start(seastar::alien::instance& alien) {
    if (_impl->server) {
        return;  // already started
    }
    _impl->alien = &alien;
    _impl->service = std::make_unique<ExtProcServiceImpl>(_impl->router, _impl->alien);

    const std::string addr =
        fmt::format("{}:{}", _impl->cfg.listen_address, _impl->cfg.port);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(_impl->service.get());
    _impl->server = builder.BuildAndStart();
    if (!_impl->server) {
        log_epp.error("GIE EPP ext_proc server failed to bind {}", addr);
        return;
    }
    log_epp.info("GIE EPP ext_proc server listening on {} "
                 "(envoy.service.ext_proc.v3.ExternalProcessor)", addr);
}

seastar::future<> GieEppServer::stop() {
    if (!_impl->server || _impl->shutdown_thread.joinable()) {
        return seastar::make_ready_future<>();  // not started, or already stopping
    }
    // Drain gRPC on a dedicated OS thread (Rule #12): in-flight Process handlers
    // are blocked on the reactor via alien::submit_to, so the reactor MUST stay
    // free for them to finish — freezing it in Wait() here would deadlock those
    // handlers until they were force-cancelled. Shutdown() stops accepting and
    // lets in-flight RPCs complete; once Wait() returns, hop back onto shard 0
    // to resolve the future (mirrors async_persistence's worker-done signal).
    auto fut = _impl->shutdown_promise.get_future();
    grpc::Server* server = _impl->server.get();
    seastar::alien::instance* alien = _impl->alien;
    Impl* impl = _impl.get();  // owned by this; ~Impl joins the thread first
    _impl->shutdown_thread = std::thread([server, alien, impl]() {
        server->Shutdown();
        server->Wait();
        seastar::alien::run_on(*alien, 0, [impl]() noexcept {
            impl->shutdown_promise.set_value();
        });
    });
    log_epp.debug("GIE EPP ext_proc server draining");
    return fut;
}

}  // namespace ranvier
