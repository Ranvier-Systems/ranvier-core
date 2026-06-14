// Ranvier Core - GIE Endpoint-Picker (EPP) ext_proc gRPC server implementation
//
// See gie_epp_server.hpp for the threading model and scope. All gRPC and
// generated-protobuf types are confined to this translation unit; it is compiled
// only WITH_GIE_EPP=ON.

#include "gie_epp_server.hpp"

#include "gie_epp_plan.hpp"
#include "logging.hpp"
#include "request_rewriter.hpp"
#include "router_service.hpp"
#include "tokenizer_service.hpp"

// Generated from proto/ext_proc_min.proto (build-time protoc + grpc_cpp_plugin;
// include dir wired in CMakeLists.txt under WITH_GIE_EPP).
#include "ext_proc_min.pb.h"
#include "ext_proc_min.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <fmt/format.h>

#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <seastar/core/coroutine.hh>
#include <seastar/util/log.hh>

namespace ranvier {

namespace epp = envoy::service::ext_proc::v3;

static seastar::logger log_epp("gie_epp");

namespace {

// Rule #4: bound the request body we buffer for a routing decision. Routing
// needs only the prompt prefix, so truncating a very large or streamed body is
// harmless — the inline path likewise tokenizes only a bounded prefix. Caps the
// per-request accumulation regardless of how much the gateway streams.
constexpr size_t kMaxRoutingBodyBytes = 256 * 1024;

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
    ExtProcServiceImpl(RouterService* router,
                       seastar::sharded<TokenizerService>* tokenizer,
                       ChatTemplate chat_template,
                       seastar::alien::instance* alien)
        : _router(router),
          _tokenizer(tokenizer),
          _chat_template(std::move(chat_template)),
          _alien(alien) {}

    grpc::Status Process(
        grpc::ServerContext* /*context*/,
        grpc::ServerReaderWriter<epp::ProcessingResponse, epp::ProcessingRequest>* stream)
        override {
        // Per-request body accumulation (this RPC, this gRPC thread). BUFFERED
        // delivery is one frame with end_of_stream; STREAMED is several.
        std::string body_buf;
        epp::ProcessingRequest request;
        while (stream->Read(&request)) {
            epp::ProcessingResponse response;
            bool write = true;

            if (request.has_request_headers()) {
                if (request.request_headers().end_of_stream()) {
                    // No body will follow (e.g. GET / empty body): nothing to
                    // tokenize, so decide now on the headers phase (load/hash).
                    fill_decision(response, /*on_body=*/false, decide(std::string()));
                } else {
                    // A body follows; defer the decision to the body phase so we
                    // can tokenize the prompt for prefix-aware routing.
                    response.mutable_request_headers()->mutable_response()->set_status(
                        epp::CommonResponse::CONTINUE);
                }
            } else if (request.has_request_body()) {
                const auto& hb = request.request_body();
                // Rule #4: accumulate only up to the routing-prefix cap; extra
                // body bytes beyond it don't change the prefix decision.
                epp_append_bounded(body_buf, hb.body(), kMaxRoutingBodyBytes);
                if (hb.end_of_stream()) {
                    // Full body in hand: prefix-aware decision on the body phase.
                    fill_decision(response, /*on_body=*/true, decide(body_buf));
                } else {
                    // More body frames coming: keep accumulating, CONTINUE.
                    response.mutable_request_body()->mutable_response()->set_status(
                        epp::CommonResponse::CONTINUE);
                }
            } else {
                // A phase we don't act on for a request-path picker
                // (response_* / trailers). Close the stream cleanly.
                write = false;
                break;
            }

            if (write && !stream->Write(response)) {
                break;  // gateway hung up
            }
            request.Clear();
        }
        return grpc::Status::OK;
    }

private:
    // Fill `response` for a completed decision: set the destination-endpoint
    // header (+ dynamic_metadata) on the matching phase, or an ImmediateResponse
    // 503 when no backend is ready. `on_body` selects which oneof phase carries
    // the CommonResponse (request_body vs request_headers).
    void fill_decision(epp::ProcessingResponse& response, bool on_body,
                       const EppDecision& decision) {
        std::optional<std::string> endpoint;
        if (decision.ok) {
            endpoint = epp_format_endpoint(
                fmt::format("{}", decision.addr.addr()), decision.addr.port());
        }
        const EppResponsePlan plan = epp_plan(decision.ok, endpoint);

        if (plan.kind == EppResponsePlan::Kind::SetEndpoint) {
            auto* common = on_body
                ? response.mutable_request_body()->mutable_response()
                : response.mutable_request_headers()->mutable_response();
            common->set_status(epp::CommonResponse::CONTINUE);
            // Force the data plane to re-run endpoint selection against the
            // header we just set (otherwise the original route stands).
            common->set_clear_route_cache(true);
            auto* opt = common->mutable_header_mutation()->add_set_headers();
            opt->set_append_action(epp::HeaderValueOption::OVERWRITE_IF_EXISTS_OR_ADD);
            auto* hv = opt->mutable_header();
            hv->set_key(kGieDestinationEndpointHeader);
            hv->set_value(plan.endpoint);
            // Also surface the same endpoint via dynamic_metadata for gateways
            // that read it from there. The GIE spec requires the header and
            // metadata values not disagree — both come from `plan.endpoint`.
            set_dynamic_metadata(response, plan.endpoint);
        } else {
            auto* imm = response.mutable_immediate_response();
            imm->mutable_status()->set_code(epp::ServiceUnavailable);
            imm->set_details("ranvier: no ready endpoint");
        }
    }

    // dynamic_metadata = { "envoy.lb": { "x-gateway-destination-endpoint": ep } }
    static void set_dynamic_metadata(epp::ProcessingResponse& response,
                                     const std::string& endpoint) {
        auto* md = response.mutable_dynamic_metadata();                 // Struct
        auto* inner = (*md->mutable_fields())["envoy.lb"]              // Value
                          .mutable_struct_value();                      // Struct
        (*inner->mutable_fields())[kGieDestinationEndpointHeader]       // Value
            .set_string_value(endpoint);
    }

    // Run the routing decision on the reactor (shard 0) and block this gRPC
    // handler thread for the result. `body` is the raw request body (empty for
    // bodyless requests). Reuses the entire reactor-side tokenize+route pipeline
    // rather than reimplementing routing off-reactor.
    EppDecision decide(const std::string& body) {
        try {
            // The submit_to callable is a PLAIN lambda (not a coroutine): it
            // copies the body reactor-side from a view into `body` (alive across
            // fut.get() below) and hands it to the named coroutine by value. This
            // avoids both a cross-thread free of the body and the Rule #16
            // coroutine-lambda lifetime trap.
            std::future<EppDecision> fut = seastar::alien::submit_to(
                *_alien, 0u,
                [this, bv = std::string_view(body)]() -> seastar::future<EppDecision> {
                    return route_on_reactor(std::string(bv));
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

    // Reactor-side (shard 0) routing coroutine. Named (not a lambda) and takes
    // `body` by value (Rule #21) so its frame owns the body across the tokenize
    // suspension. Extracts the prompt with the SAME chat template the inline path
    // uses, so the EPP's tokens align with the backend's (and thus with the
    // KV-event-fed residency index); tokenizes; routes.
    seastar::future<EppDecision> route_on_reactor(std::string body) {
        std::vector<int32_t> tokens;
        auto extracted = RequestRewriter::extract_text_with_boundary_info(
            body, /*need_formatted_messages=*/false, _chat_template);
        std::string text = extracted.has_value() ? std::move(extracted->text) : std::move(body);
        if (!text.empty() && _tokenizer->local().is_loaded()) {
            auto tok = co_await _tokenizer->local().encode_threaded_async(text);
            tokens = std::move(tok.tokens);
        }
        // Empty tokens (bodyless / unloaded tokenizer) -> load/hash fallback,
        // exactly as the headerless path; non-empty -> ART/residency-aware.
        RouteResult rr = _router->route_request(tokens, std::string(), 0, 0.0);
        EppDecision d;
        if (rr.backend_id.has_value()) {
            auto addr = _router->get_backend_address(*rr.backend_id);
            if (addr.has_value()) {
                d.ok = true;
                d.addr = *addr;
            }
        }
        co_return d;
    }

    RouterService* _router;
    seastar::sharded<TokenizerService>* _tokenizer;
    const ChatTemplate _chat_template;
    seastar::alien::instance* _alien;
};

}  // namespace

// ---------------------------------------------------------------------------
// PIMPL
// ---------------------------------------------------------------------------

struct GieEppServer::Impl {
    GieEppConfig cfg;
    RouterService* router = nullptr;
    seastar::sharded<TokenizerService>* tokenizer = nullptr;
    ChatTemplate chat_template;
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

GieEppServer::GieEppServer(const GieEppConfig& cfg, RouterService* router,
                           seastar::sharded<TokenizerService>* tokenizer,
                           ChatTemplate chat_template)
    : _impl(std::make_unique<Impl>()) {
    _impl->cfg = cfg;
    _impl->router = router;
    _impl->tokenizer = tokenizer;
    _impl->chat_template = std::move(chat_template);
}

GieEppServer::~GieEppServer() = default;

void GieEppServer::start(seastar::alien::instance& alien) {
    if (_impl->server) {
        return;  // already started
    }
    _impl->alien = &alien;
    _impl->service = std::make_unique<ExtProcServiceImpl>(
        _impl->router, _impl->tokenizer, _impl->chat_template, _impl->alien);

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
