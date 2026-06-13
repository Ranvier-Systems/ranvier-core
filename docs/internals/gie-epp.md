# GIE Endpoint-Picker (EPP) ext_proc Compatibility Mode

**Status:** PR-1 (bridge + header-level routing) implemented; prefix-aware
routing, `dynamic_metadata`, and 429 shedding are PR-2. **Build-gated:**
`WITH_GIE_EPP` (default OFF). **Runtime:** `gie_epp.enabled` (default false).

## What this is

The Kubernetes [Gateway API Inference Extension](https://gateway-api-inference-extension.sigs.k8s.io/)
(GIE, GA 2026) standardized endpoint selection on an **ext_proc Endpoint-Picker
(EPP)** protocol: a GIE-conformant gateway delegates "which model-server endpoint
should serve this request?" to an external gRPC service. This mode exposes
Ranvier's routing core as that picker, so any conformant gateway can delegate to
it — riding the standard's distribution **without giving up the standalone
inline data plane**, which remains the primary, lower-latency path. (The
inline-vs-sidecar overhead benchmark that motivates that framing is a separate
follow-up.)

## Protocol

The server implements Envoy's `envoy.service.ext_proc.v3.ExternalProcessor`
(`Process`, a bidirectional stream). Per the
[EPP protocol](https://github.com/kubernetes-sigs/gateway-api-inference-extension/tree/main/docs/proposals/004-endpoint-picker-protocol),
the picker returns the chosen backend by setting the
`x-gateway-destination-endpoint` response header to `<ip:port>` (with
`clear_route_cache` so the data plane re-evaluates against it); when no backend
is ready it returns an `ImmediateResponse` with HTTP **503**.

Rather than vendor Envoy's full proto tree, `proto/ext_proc_min.proto` declares a
**minimal, wire-compatible subset**: protobuf encodes by field number and gRPC
dispatches by the `envoy.service.ext_proc.v3.ExternalProcessor/Process` path, so
matching the package/service/method names and the exact upstream field numbers
is sufficient for interop. Unknown fields in the gateway's full messages are
ignored. The referenced Envoy types (`HeaderMap`/`HeaderValue`/
`HeaderValueOption`/`HttpStatus`) are inlined with their upstream field numbers
(verified 2026-06 against `envoyproxy/envoy@main`).

## Threading: gRPC ↔ Seastar bridge

gRPC owns its own completion-queue threads, which cannot run on the Seastar
reactor. So the gRPC server lives entirely on gRPC's own threads (Rule #12 — the
same dedicated-OS-thread discipline as `tokenizer_thread_pool` /
`kv_event_subscriber`). The difference is direction: a `Process` handler needs a
**response**, so it bridges into the reactor with `seastar::alien::submit_to`
(shard 0) and blocks the gRPC thread on the returned `std::future` for the
routing decision — the request/response inverse of the KV subscriber's
fire-and-forget `alien::run_on`. The handler thus reuses the entire reactor-side
pipeline (`RouterService::route_request` → `get_backend_address`) instead of
reimplementing routing off-reactor.

The decision crosses the boundary as a trivially-copyable POD (a
`socket_address` + an "ok" flag), so no Seastar-shard heap is ever freed on a
gRPC thread (Rules #14/#15). The `<ip:port>` string is formatted on the gRPC
thread from that POD.

```
GIE gateway ──gRPC──► ExternalProcessor::Process       (gRPC sync thread pool)
                        read request_headers
                        alien::submit_to(shard 0) ─► route_request()   (reactor)
                                                     get_backend_address()
                        ◄──── EppDecision (POD) ──────────────────────────┘
                        set x-gateway-destination-endpoint   (or 503)
```

## Lifecycle

The server is created and started on shard 0 after `RouterService` is up (it
answers 503 until backends register — the correct GIE behaviour), and stopped in
`stop_services()` **before** `RouterService` teardown, since in-flight handlers
call into the router. Shutdown is a bounded `grpc::Server::Shutdown(deadline)` +
`Wait()` (a brief teardown-time reactor stall, matching the kv-subscriber join
precedent).

## Configuration

```yaml
gie_epp:
  enabled: false          # RANVIER_GIE_EPP_ENABLED; toggling requires restart
  port: 9002              # RANVIER_GIE_EPP_PORT
  listen_address: "0.0.0.0"  # RANVIER_GIE_EPP_LISTEN_ADDRESS
```

Requires a binary built with `-DWITH_GIE_EPP=ON` (which adds gRPC + protobuf and
generates the ext_proc stubs from `proto/ext_proc_min.proto` at build time).
`enabled: true` on a binary without `WITH_GIE_EPP` logs a warning and stays
inert.

## Deferred to PR-2

- Capture the request body (`request_body` phase) and tokenize it for full
  prefix-aware routing via the existing pipeline, instead of header-level
  selection with empty tokens.
- Mirror the endpoint into `dynamic_metadata` (`envoy.lb` namespace) alongside
  the header.
- 429 request-shedding and multi-endpoint fallback lists.
- The published inline-vs-sidecar ext_proc overhead benchmark.
