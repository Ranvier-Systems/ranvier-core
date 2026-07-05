# GIE Endpoint-Picker (EPP) ext_proc Compatibility Mode

**Status:** implemented (bridge + prefix-aware routing + `dynamic_metadata`).
429 request-shedding and the inline-vs-sidecar benchmark remain as follow-ups.
**Build-gated:** `WITH_GIE_EPP` (default OFF). **Runtime:** `gie_epp.enabled`
(default false).

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
pipeline (extract → tokenize → `route_request` → `get_backend_address`) instead
of reimplementing it off-reactor.

The reactor-side work is a **named coroutine** (`route_on_reactor`, taking the
body by value — Rules #16/#21) invoked by a *plain* `submit_to` lambda that
copies the body reactor-side from a `string_view` into the gRPC thread's buffer
(alive across the bridge wait). That sidesteps both a cross-thread free of the
body and the coroutine-lambda lifetime trap. The decision crosses back as a
trivially-copyable POD (a `socket_address` + an "ok" flag), so no Seastar-shard
heap is ever freed on a gRPC thread (Rules #14/#15); the `<ip:port>` string is
formatted on the gRPC thread from that POD.

### Ingress backpressure (cap + deadline)

gRPC's sync API grows its handler-thread pool with concurrent RPCs, so an
unbounded `Process` flood (or a slow client opening many streams) would park an
unbounded number of handler threads on `fut.get()`, each blocked awaiting shard
0 — flooding shard 0's task queue. The inline HTTP ingress has a concurrency
cap; this alternate ingress bounds itself two ways (both `gie_epp.*` config):

- **Ingress admission cap** (`max_inflight_requests`, default 1024): a plain
  atomic counter on the gRPC side (never the reactor, so no Seastar primitive is
  needed) admits at most N concurrent bridges onto shard 0. Over-cap requests
  shed immediately down the same `ImmediateResponse` **503** path as "no ready
  endpoint" — they never reach the reactor. `0` disables the cap.
- **Bridge deadline** (`bridge_deadline_ms`, default 2000): the handler waits on
  the routing future with `wait_for(deadline)` rather than an unbounded
  `get()`. A wedged or overloaded shard 0 sheds load (503) instead of parking
  the handler thread forever. The abandoned reactor task still completes and
  sets the now-unobserved shared state; `std::future`'s destructor does not
  block on it, so no handler thread leaks. `0` disables the deadline.

Both shed reasons increment a cumulative counter logged at drain. This is the
503 load-shedding backstop; the GIE-native **429** shed (below) is a separate,
still-open follow-up.

```
GIE gateway ──gRPC──► ExternalProcessor::Process       (gRPC sync thread pool)
                        request_headers → CONTINUE (defer; body follows)
                        request_body  → accumulate, then on end_of_stream:
                        alien::submit_to(shard 0) ─► route_on_reactor()  (reactor)
                                                       extract_text + chat template
                                                       encode_threaded_async()
                                                       route_request(tokens)
                                                       get_backend_address()
                        ◄──── EppDecision (POD) ──────────────────────────┘
                        set x-gateway-destination-endpoint (+ dynamic_metadata),
                        or ImmediateResponse 503
```

## Prefix-aware routing & tokenization alignment

The picker routes on the actual prompt: it captures the request body (the
gateway must send it — `request_body: BUFFERED` in the GIE picker config; a
bodyless request, headers `end_of_stream`, routes on load/hash instead),
extracts the prompt, and tokenizes it with **the same chat template the inline
path uses** (`assets.chat_template_format`). That alignment is load-bearing:
vLLM tokenizes the chat-template-formatted prompt, the native KV-event stream
(P0.1) keys the residency index on *those* token hashes, so the EPP's tokens
must match for an ART/residency hit. With a misaligned (e.g. template-less)
tokenization the prefix hashes diverge and routing silently falls back to
load/hash. In a **pure-EPP deployment** (no inline proxy learning routes), the
prefix/residency state that makes this meaningful is populated by the KV-event
stream and gossip — so prefix-aware EPP routing composes with, and depends on,
P0.1. Without any residency source it degrades to load/hash, the same as the
bodyless path — no regression. Caveat (shared with the inline path): one
configured template won't byte-match a heterogeneous fleet of model families;
per-model template selection is a future refinement.

The chosen endpoint is returned both as the `x-gateway-destination-endpoint`
header mutation and in `dynamic_metadata` under the `envoy.lb` namespace (same
value, per the GIE spec). `clear_route_cache` forces the data plane to re-run
selection against it.

## Lifecycle

The server is created and started on shard 0 after `RouterService` is up (it
answers 503 until backends register — the correct GIE behaviour), and stopped in
`stop_services()` **before** `RouterService` teardown, since in-flight handlers
call into the router. Shutdown drains gRPC on a **dedicated OS thread**
(`Shutdown()` + `Wait()` off the reactor) and resolves the returned future back
on shard 0 via `alien::run_on`, so the reactor stays free to service in-flight
handlers' bridge calls while they finish (freezing it in `Wait()` would deadlock
them); the thread is joined in `~Impl` (Rule #13).

## Configuration

```yaml
gie_epp:
  enabled: false          # RANVIER_GIE_EPP_ENABLED; toggling requires restart
  port: 9002              # RANVIER_GIE_EPP_PORT
  listen_address: "0.0.0.0"  # RANVIER_GIE_EPP_LISTEN_ADDRESS
  max_inflight_requests: 1024  # RANVIER_GIE_EPP_MAX_INFLIGHT_REQUESTS; 0 disables the cap
  bridge_deadline_ms: 2000     # RANVIER_GIE_EPP_BRIDGE_DEADLINE_MS; 0 disables the deadline
```

Requires a binary built with `-DWITH_GIE_EPP=ON` (which adds gRPC + protobuf and
generates the ext_proc stubs from `proto/ext_proc_min.proto` at build time).
`enabled: true` on a binary without `WITH_GIE_EPP` logs a warning and stays
inert.

## Building & testing

The EPP is **off by default** (`WITH_GIE_EPP=OFF`), so the production image and
the standard unit-test lane never compile the gRPC server. Three layers of
coverage close the gap without flipping the default:

- **Decision logic — always covered.** `tests/unit/gie_epp_test.cpp` is pure
  (no gRPC/Seastar deps) and is part of the `unit_tests` aggregate, so it runs
  in the normal **Unit Tests** CI lane and any local `make test-unit` — the
  endpoint formatting, the set-endpoint-vs-503 branch, and the Rule #4 body cap
  are guarded on every push regardless of the flag.
- **gRPC path — dedicated lane.** `Dockerfile.gie-epp` builds
  `WITH_GIE_EPP=ON` (compiling + linking `gie_epp_server.cpp` and running the
  ext_proc proto codegen), and `.github/workflows/gie-epp-tests.yml` runs
  `ctest` over that build after each Docker Publish. This is the compile/link
  regression guard for the gRPC server, mirroring the sanitizer/fuzz lanes.
- **End-to-end behaviour.** `make test-epp`
  (`tests/integration/test_gie_epp.py` + `docker-compose.epp-test.yml`) drives a
  real running EPP via a gRPC `ext_proc` client (`epp_client.py`) and asserts the
  503 / header / `dynamic_metadata` / bodyless paths. The same client backs the
  overhead microbenchmark (`make bench-epp`, see
  [`../benchmarks/epp-overhead-microbenchmark.md`](../benchmarks/epp-overhead-microbenchmark.md)).

The build toolchain (`grpc-devel`, `grpc-plugins`, `protobuf-compiler`) lives in
`Dockerfile.base`, so it is *available* for an EPP build without being linked
into the default/production binaries.

**Local one-liner** (inside the `ranvier-base` / `ranvier-gie-epp` dev
container, which carries the gRPC toolchain and the pre-built `/deps`):

```sh
make gie-epp-test     # configure WITH_GIE_EPP=ON, build ranvier_server + tests, run ctest
```

The local target builds `WITH_KV_EVENTS=OFF` so it needs only the gRPC
toolchain, not libzmq — KV events is orthogonal and has its own coverage. The
CI lane (`Dockerfile.gie-epp`) builds the full EPP + KV combination. Pass
`make gie-epp-test GIE_EPP_KV_EVENTS=ON` to build both locally (needs
`zeromq-devel`).

Or by hand: `cmake -B build -DWITH_GIE_EPP=ON && cmake --build build && (cd build && ctest)`
— note `WITH_GIE_EPP` is a CMake configure option, so it must be set at the
`cmake` step, not passed to `ninja`/`make`. If the gRPC packages are missing,
the configure step fails fast with a `FATAL_ERROR` naming what to install.

## Remaining follow-ups

- **429 request-shedding.** GIE allows an `ImmediateResponse` 429 under load.
  Deferred pending a real overload signal — there is no global "shed now" today
  (only per-backend `get_composite_backend_load`); a meaningful policy is its
  own design rather than a blunt all-backends-over-threshold heuristic. Note the
  ingress cap + bridge deadline above already shed genuine overload/stall as
  **503**; a 429 would be the GIE-preferred "retry me" signal for the same
  condition once a policy exists.
- **Inline-vs-sidecar overhead benchmark.** The published proof point comparing
  inline routing to a sidecar ext_proc EPP hop (needs real cluster/hardware).
- **Per-model chat-template selection** for heterogeneous fleets (see the
  tokenization-alignment caveat above), and `mode_override` to request the
  request body from gateways not pre-configured to send it.
- **Multi-endpoint fallback lists** in the destination header.
