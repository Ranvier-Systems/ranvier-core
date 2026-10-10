# Changelog

All notable changes to Ranvier Core will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- `POST /admin/backends?...&kv_events_port=P[&kv_events_replay_port=R]`: opt an
  admin-registered backend into the native KV-event stream (vLLM `--kv-events-config`
  ZMQ publisher at `tcp://<ip>:P`), the admin-API form of the static-YAML `kv_events_port`
  and the `ranvier.io/kv-events-port` annotation; `kv_events_port=0` drops the stream. The
  response carries `"kv_events": subscribed | unsubscribed | unavailable | queue_full`
  (`unavailable` when `kv_events.enabled` is false or the build lacks `WITH_KV_EVENTS`; the
  backend still registers). Not persisted with the backend row. Exists because the
  benchmark fleet registers backends through this endpoint and so had no way to turn the
  subscriber on: every GPU campaign to date ran with `router_native_*` at zero.
- `bench.sh --kv-events` (and `--kv-events-port-start N`, default 5557): launches each vLLM
  with `--kv-events-config` (ZMQ publisher on 5557+i, replay socket +100), the Ranvier nodes
  with `RANVIER_KV_EVENTS_ENABLED=true`, and has Locust register every backend with
  `kv_events_port`, for both `--compare` arms. The compare header states whether the stream
  was on and prints `router_native_kv_ops_total` per arm (bench.sh logs an error when the
  prefix arm's is 0: the stream never connected and the arm ran on probabilistic residency);
  the manifest records `kv_events_enabled`; `results_parser.py compare` prints the native
  counters (kv_ops, verified_hits, verified_evictions, routes_materialized, stream_resets)
  for both arms whenever any is non-zero or a manifest says the run asked for the stream.
- `bench-runner.sh --suite kvevents` (fitted 13B 20u and 50-prefix 13B 20u vs `least_loaded`
  with `--kv-events`, ×3, ~2h40m; the pre-registered reading is in `--help`) and
  `--suite kvreset` (the 8B 20u row vs round-robin ×3 with the between-arm KV reset working,
  which that row has never had).
- `routing_mode: least_loaded` (`RANVIER_ROUTING_MODE=least_loaded`): route every request
  to the live backend with the lowest capacity-adjusted composite load (under the shipping
  defaults, the node's in-flight count summed across shards), ties broken uniformly at
  random; no ART lookup, no route learning, tokenization skipped as in `random`. Exists as
  the benchmark's strongest no-affinity baseline: `bench.sh --compare --baseline-mode
  least_loaded` runs it as the baseline arm, `bench-runner.sh --suite baseline` runs the
  fitted 13B 20u and 30u rows against it, and the compare header names the arm. Not a
  recommended production mode.

### Added
- `bench-runner.sh --suite saturation`: the fitted 13B 30u/30m row vs `least_loaded` at
  `--bounded-load-epsilon 0.5` and `0.25`, plus the 50-prefix 20u row vs `least_loaded`, three
  repeats each (~8 h). Asks whether a tighter divert cap recovers the saturation tail the
  baseline suite found prefix routing does not have over least-loaded, without giving back P50
  and KV; the pre-registered reading is in the suite's help text and the results doc.

### Documentation
- Baseline suite measured (2026-10-08): prefix routing vs the new `least_loaded` mode on the
  fitted 13B 20u and 30u rows, three repeats each. Least-loaded alone removes most of the P99
  tail against round-robin (−50% / −41%); prefix adds P50 −27…−29%, throughput +6…+9% and a
  3–4× KV hit rate at both loads, a further −14.6% P99 at 20 users and no reliable P99 change at
  30. README tagline, summary table and benchmark section, the results doc and the 2026-10-02
  strategic assessment restate the headline accordingly: affinity's win is prefill saved and
  capacity recovered; most of the tail win over round-robin is load balancing.
- Saturation suite measured (2026-10-09, first suite with the KV reset acknowledged `8/8` on every
  arm): the fitted 13B 30u row vs `least_loaded` with the prefix arm at `bounded_load_epsilon`
  0.5 (P99 −7.3/+6.0/+1.3%, mixed, KV 42–44%) and 0.25 (+11.1/+4.8/+25.4%, consistent
  regression, KV 32–37%): a tighter cap diverts more, gives back KV and moves the tail the wrong
  way, so the default stays 1.0 and the docs state the trade-off. The 50-prefix 13B 20u row vs
  `least_loaded`, three clean repeats: P99 +22.9/+24.9/+21.7% of completed requests with P50
  −9…−16%, KV 3–5×, fewer timeouts and flat throughput; the carry-over repeat's parity of
  2026-10-08 was the inherited cache. README, results doc, strategic assessment and BACKLOG §27
  (new item: holder-aware divert) updated.

### Fixed
- Native KV-event stream, first hardware contact (2026-10-10, vLLM 0.15.1): three defects, each
  of which alone kept `router_native_kv_ops_total` at zero. (1) `bench.sh --kv-events` passed
  `tcp://0.0.0.0:<port>` as the publisher endpoint; vLLM binds only when the endpoint contains
  `*` and connect()s otherwise, so the PUB socket never listened. The endpoint is now
  `tcp://*:<port>` and bench.sh checks that every publisher port is listening once vLLM is
  healthy. (2) The subscriber's replay request was the bare 8-byte sequence; vLLM's ROUTER
  handler expects the REQ-style envelope and logged every request as "Invalid replay request",
  so every connect-backfill and gap repair failed. The DEALER now sends the empty delimiter frame
  first. (3) The decoder accepted only integer block hashes; vLLM publishes 32-byte sha256 BIN
  hashes by default (ints only with `VLLM_KV_EVENTS_USE_INT_BLOCK_HASHES=1`). The decoder now
  accepts both and folds bytes to the same low-64-bit key vLLM's int conversion produces, so a
  stream with either encoding addresses the same ledger entries; bench.sh also sets the int
  variable on the vLLM it launches. The two kvevents-suite runs made before the fix are valid
  control repeats without the stream (fitted 13B 20u vs least-loaded, P99 −13.2 / −12.5%).
- `bench.sh` launches vLLM with `VLLM_SERVER_DEV_MODE=1`: vLLM serves `POST /reset_prefix_cache`
  only as a development endpoint behind that variable, so the between-arm KV reset added in
  the previous entry returned 404 on vLLM 0.15.1 and the arms still carried KV over (first
  seen on the 2026-10-08 baseline suite; its compare headers record `0/8` acknowledged). The
  404 warning now says what to set on externally managed backends.

### Changed
- `bench.sh --compare` resets every vLLM backend's prefix cache (`POST /reset_prefix_cache`)
  before each arm, so the arm that runs second no longer inherits the first arm's warm KV
  (on 8B/20u the prefix-first repeats read ~9 points weaker for that reason on both
  2026-10-01 and 2026-10-06). `--no-kv-reset` keeps the old carry-over; the compare header
  records which behaviour a run had and how many backends acknowledged the reset.
- `results_parser.py aggregate` reads each repeat's arm order from its manifest and prints
  rr-first and prefix-first medians beside the overall verdict (also in the JSON as
  `by_arm_order`), so an order effect shows in the aggregate instead of needing the compare
  files read by hand.
- `bench-runner.sh`: the `fitted` suite gains a 13B 30 users / 30 min row (16 prefixes ×
  2000–4000 tokens) so the high-load regime has a measurement without timeouts; the `epsilon`
  and `placement` suites are labelled historical (ε 1.0 and least-loaded placement ship since
  2.2.0).

### Documentation
- Standard 50-prefix matrix re-measured at three repeats on the 2.2.0 image (2026-10-06):
  P99 TTFT vs round-robin −26.7% (8B 20u), −14.1% (13B 30u, timeouts both arms), −21.0%
  (13B 20u, was +11.0%), −38.8% (13B 10u, was no reliable effect); every repeat improved.
  README and `docs/benchmarks/benchmark-results-current.md` cite it as the current table;
  the arm-order (warm-cache inheritance) and 30-user timeout caveats are recorded.
- Fitted suite at three repeats on the 2.2.0 image (2026-10-06/07), including the new 13B
  30 users / 30 min row: P99 TTFT −28.6% (10u), −56.6% (20u, reproducing the Oct 5 −57.5%
  on another instance), −42.0% (30u) with zero incomplete requests in all eighteen arms and
  +15% throughput at 30 users. The 50-prefix 30-user timeout excess is thereby attributed to
  the eviction regime, not the ε 1.0 cap; no ε sweep is planned.

## [2.2.0] - 2026-10-06

Routing release. The load-divert policy now reads the node's live in-flight count
instead of a 5-second-stale scraped GPU score, diverts only at twice the mean
instead of on nearly any load, and places new prefixes least-loaded with
cluster-wide convergence. On the configuration that had regressed in every earlier
campaign (CodeLlama-13B, 20 users, 8×A100, 3 Ranvier nodes) P99 time-to-first-token
went from +11% against round-robin to **−57.5% median across three repeats and both
arm orders**, with P50 −28% and vLLM's KV prefix-cache hit rate 12% → 70%. Nothing
measured regressed: 13B 10 users −34.5%, Llama-8B 20 users −22.6% (was −17%), 13B
30 users −16.4% in the eviction regime. Record and every intermediate leg:
`docs/benchmarks/benchmark-results-current.md`. Also in this release: the Gateway API
Inference Extension Endpoint-Picker mode, the native vLLM KV-event subscriber,
prefill/decode pool roles, the unified route scorer, and the embeddability seams
(admission policy, usage ledger, response-side usage, OpenTelemetry GenAI).

### Upgrade notes

Five routing defaults changed. Every multi-shard deployment gets the new divert
behaviour on upgrade; no config key was renamed or removed.

| Key | 2.1.0 | 2.2.0 | Env to restore 2.1.0 |
|-----|-------|-------|----------------------|
| `routing.cross_shard_load_sync` | `false` | `true` | `RANVIER_CROSS_SHARD_LOAD_SYNC=false` |
| `routing.gpu_load_weight` | `10` | `0` | `RANVIER_ROUTING_GPU_LOAD_WEIGHT=10` |
| `routing.capacity_headroom_weight` | `5` | `0` | `RANVIER_CAPACITY_HEADROOM_WEIGHT=5` |
| `routing.bounded_load_epsilon` | `0.25` | `1.0` | `RANVIER_BOUNDED_LOAD_EPSILON=0.25` |
| `routing.miss_placement` | `hash` | `least_loaded` | `RANVIER_MISS_PLACEMENT=hash` |

- Cross-shard load sync broadcasts each shard's in-flight counts every 100 ms:
  about 1,100 SMP messages per second on 8 shards. A single-shard process skips
  the timer. Lower `cross_shard_load_sync_interval` only with a reason.
- The vLLM metrics scrape still runs; the GPU score and KV usage now feed
  observability and residency routing only, not the divert decision. Set the two
  weights above zero to blend them back in.
- `miss_placement: hash` remains the right choice on a single node without gossip,
  where the convergence rules have nothing to do.
- Gossip peers on 2.1.0 and 2.2.0 interoperate: the route-announcement wire format
  is unchanged. A mixed cluster converges only once every node runs 2.2.0, since
  2.1.0 nodes keep the plain trust ladder.

### Measurement status

The 2026-09-05 release gate asked that each entry have a recorded GPU run or be
marked hardware-independent. As of this release:

- **Measured on 8×A100, October 2026:** the five routing defaults, least-loaded
  cache-miss placement with eager learn and both convergence rules, the
  bounded-load divert target, the unified route scorer (every run went through it).
  Three repeats for 13B/20 users; one confirmation repeat each for 13B/10u, 8B/20u
  and 13B/30u. The full matrix at three repeats under these defaults is the next
  benchmark session.
- **Hardware-independent:** request-admission policy seam, response-side usage
  accounting, usage-ledger sink, OpenTelemetry GenAI conventions, GIE EPP bridge,
  server, integration test and overhead microbenchmark, inline-vs-sidecar scope and
  Phase 1 harness.
- **Not yet exercised on GPU hardware, ship as experimental:** Kimi (Moonshot)
  templates; native KV-event mode parts 1 and 2 (the October campaigns ran with the
  subscriber off and the residency signal never crossed its threshold);
  disaggregated prefill/decode pool roles. Each is opt-in and off by default.

### Added

- **Kimi (Moonshot) model support** — `ChatTemplateFormat::kimi` (aliases
  `kimi`, `kimi-k2`, `kimi-k3`, `moonshot`) renders Kimi K2/K3's per-role
  `<|im_user|>`/`<|im_assistant|>`/`<|im_system|>` turns, `<|im_middle|>` and
  `<|im_end|>`, and injects Moonshot's default system turn for system-less
  conversations, so Ranvier's routing token sequence stays byte-aligned with the
  backend's `apply_chat_template`. Kimi ships no fast `tokenizer.json`, so
  `tests/tokenizer_parity/` adds a tiktoken→fast conversion helper, a parity
  harness that checks the converted tokenizer and the rendered template against
  the authoritative tokenizer, and a build-gated FFI parity test
  (`RANVIER_BUILD_KIMI_PARITY_TEST`, default OFF) driven by an emitted fixture.
  Non-string (multimodal) message content is documented as dropped from
  routing. Deploy contract, open GPU-dependent items and the K3 prefix-caching
  spike runbook are in BACKLOG §26. Not yet exercised on GPU hardware.

- **Request-admission policy seam** (embeddability series) — A pluggable,
  per-request decision hook so embedders can apply custom admission policies —
  quota systems, tenant-tier products, priority rewriters — without forking the
  controller. `src/admission_policy.hpp` defines the abstract `AdmissionPolicy`
  with a single synchronous, non-throwing `decide()` (Hard Rule #22) and a
  process-wide factory seam (`set_/get_admission_policy_factory`), instantiated
  once per shard alongside the other per-shard seam consumers so `decide()` is
  always a shard-local call. The policy sees the request's resolved `api_key_id`,
  its classified `PriorityLevel` (CRITICAL included — the hook does not hard-code
  that judgment), and the pre-route estimated cost units; it returns Allow or
  Reject. Reject surfaces as a 429 with `Retry-After` (reusing the existing
  rejection plumbing); Allow may carry a `PriorityLevel` override applied before
  scheduler enqueue and an optional `X-Ranvier-Admission-Warning` response
  header. The proxy path consults it after priority extraction and before
  backpressure/scheduler admission, so an override participates in tier queueing
  and a rejection short-circuits before any queue slot is consumed. No
  config-file surface (installed by embedder code, not YAML); no factory
  installed => no policy object, no per-request branch cost beyond a null check,
  behaviour bit-for-bit unchanged.

- **Response-side usage accounting — Phase 1: capture actuals** (§20.2
  P1.5/P1.6 follow-up) — The usage-ledger sink and the `request_attribution`
  SQLite row now record the engine's **authoritative** token counts when the
  response carries them, instead of always using pre-flight estimates. A pure
  scanner (`src/response_usage_parser.hpp`) snoops the response `usage` object
  from the proxied stream (non-streaming body, or the final SSE event when
  `stream_options.include_usage` is set) at the existing `StreamParser` hook;
  the terminal attribution/ledger path prefers actuals, **recomputes cost** from
  them, and falls back to estimates otherwise. A new `tokens_estimated` flag on
  `UsageEvent` + `LogRequestOp` + the `request_attribution` row (additive SQLite
  migration, defaults to `1`/estimated for existing rows) tells billing
  consumers which they got. To get actuals for streaming requests that didn't
  ask for usage, `cost_estimation.inject_stream_usage` (env
  `RANVIER_COST_ESTIMATION_INJECT_STREAM_USAGE`, **default off**) injects
  `stream_options.include_usage` into forwarded streaming requests (respecting a
  client that already chose); when off, such requests fall back to estimates.
  The GenAI span's `gen_ai.usage.output_tokens` (needs a span-lifecycle change)
  remains a documented follow-up (`docs/architecture/response-usage-accounting.md`).
  No behaviour change when the response has no usage and injection is off.

- **Inline-vs-sidecar EPP overhead A/B — scope + Phase 1** (BACKLOG §20.2 P1.4
  follow-up) — The headline benchmark: per-request latency of serving through a
  GIE gateway that delegates to Ranvier's EPP vs. Ranvier's inline data plane. A
  scope memo (`docs/benchmarks/inline-vs-sidecar-ab-scope.md`) gates it; Phase 1
  ships a 3-arm, single-backend, single-stream harness — **inline** (Ranvier
  `:8080`), **plain Envoy**, and **Envoy + ext_proc(EPP)** — so the ext_proc
  overhead (`C − B`) is isolated from "Envoy proxies vs Ranvier proxies"
  (`B − A`). New `docker-compose.epp-ab.yml` (one EPP-enabled Ranvier serving
  both planes + Envoy with two listeners from `tests/integration/envoy/bench-bootstrap.yaml`
  + the mock backend), `tests/integration/http_ab_load.py`,
  `scripts/bench-inline-vs-sidecar.sh` (prints per-arm percentiles + deltas),
  and `docs/benchmarks/inline-vs-sidecar-ab-benchmark.md`. `make
  bench-inline-vs-sidecar`. Mock backend by design (isolates the µs–ms hop from
  100s-of-ms inference); the vLLM realism pass is the GPU-gated Phase 2. No
  core/runtime change.

- **GIE EPP integration test + overhead microbenchmark** (BACKLOG §20.2 P1.4
  follow-up) — A gRPC `ext_proc` client (`tests/integration/epp_client.py`,
  stubs generated at runtime from `proto/ext_proc_min.proto`) that drives a real
  running EPP and is shared by both a new integration test and a microbenchmark.
  `tests/integration/test_gie_epp.py` (+ `docker-compose.epp-test.yml`, a
  standalone `WITH_GIE_EPP=ON` node from the `Dockerfile.gie-epp` builder stage +
  a mock backend) asserts the end-to-end picker behaviour the unit/CI coverage
  couldn't: `ImmediateResponse` 503 with no backend, the
  `x-gateway-destination-endpoint` header (+ matching `dynamic_metadata`) once a
  backend is registered, and bodyless (headers-EOS) routing. The
  `epp-overhead` microbenchmark (`scripts/bench-epp-overhead.sh` →
  `tests/integration/epp_microbench.py`, methodology in
  `docs/benchmarks/epp-overhead-microbenchmark.md`) reports the per-request
  ext_proc routing-decision + bridge latency. New `make test-epp` / `make
  bench-epp`; Python gRPC deps in `tests/integration/requirements.txt` (the
  suite skips the EPP test when they're absent). No core/runtime change.

- **GIE Endpoint-Picker (EPP) ext_proc mode — part 2: prefix-aware routing**
  (BACKLOG §20.2 P1.4, completing the item) — The picker now routes on the
  actual prompt instead of part 1's header-level load/hash. It captures the
  ext_proc `request_body` phase, and on a dedicated reactor coroutine
  (`route_on_reactor`, bridged via `alien::submit_to`) extracts the prompt and
  tokenizes it with **the same chat template the inline path uses**
  (`assets.chat_template_format`) before calling `route_request` — so the EPP's
  tokens align with the backend's and with the KV-event-fed residency index
  (P0.1), which is what makes prefix-/residency-aware selection actually hit.
  Bodyless requests (headers `end_of_stream`) and an unloaded/empty prompt fall
  back to load/hash — no regression. The chosen endpoint is also surfaced in
  `dynamic_metadata` (`envoy.lb` namespace) alongside the header, via a minimal
  inlined `google.protobuf.Struct` (wire-compatible, no `struct.proto` import).
  The body is copied reactor-side from a `string_view` (no cross-thread free)
  and the named-coroutine bridge avoids the Rule #16 lambda-lifetime trap.
  429 request-shedding and the inline-vs-sidecar benchmark remain follow-ups.
  Still build-gated `WITH_GIE_EPP=OFF`; stock builds unaffected.

- **GIE Endpoint-Picker (EPP) ext_proc mode — part 1: bridge + server**
  (BACKLOG §20.2 P1.4) — Optional gRPC `envoy.service.ext_proc.v3.ExternalProcessor`
  server that exposes Ranvier's routing core as a Gateway API Inference Extension
  Endpoint-Picker, so a GIE-conformant gateway can delegate endpoint selection to
  it (returning the chosen backend via the `x-gateway-destination-endpoint`
  header, or `ImmediateResponse` 503 when none is ready). A compatibility mode
  alongside — not a replacement for — the standalone inline data plane.
  Build-gated behind `WITH_GIE_EPP` (**default OFF**: gRPC + protobuf are heavy
  and the inline path is primary); when ON, the ext_proc stubs are generated at
  build time from `proto/ext_proc_min.proto` — a minimal, wire-compatible subset
  of the Envoy proto (field numbers verified against upstream) rather than the
  full proto tree. Runtime opt-in via `gie_epp.enabled` / `RANVIER_GIE_EPP_*`.
  gRPC runs on its own threads; handlers bridge into the reactor with
  `seastar::alien::submit_to` (the request/response inverse of the KV
  subscriber's fire-and-forget bridge) and reuse `route_request` +
  `get_backend_address`, with the decision crossing back as a trivially-copyable
  POD so no shard heap is freed off-reactor. This part routes at the header level
  (empty-token load/hash selection); prefix-aware routing over the tokenized
  request body, `dynamic_metadata`, 429 shedding, and the inline-vs-sidecar
  benchmark are the follow-up. Stock builds (`WITH_GIE_EPP=OFF`) are unaffected.
  CI coverage without changing the default: the pure decision-helper test runs
  in the default unit-test lane (always built), and a dedicated `WITH_GIE_EPP=ON`
  lane (`Dockerfile.gie-epp` + `.github/workflows/gie-epp-tests.yml`) compiles +
  links the gRPC server and runs ctest. The gRPC toolchain is in
  `Dockerfile.base`; `make gie-epp-test` is the local one-liner.

- **Usage-ledger sink seam** (BACKLOG §20.2 P1.5) — A pluggable, per-request
  usage-event sink for external metering / attribution / billing backends,
  layered on the existing per-API-key attribution without core depending on any
  concrete implementation. `src/usage_ledger_sink.hpp` defines the
  `UsageLedgerSink` interface, the `NoopUsageLedgerSink` default, and a
  process-wide factory seam (`set_/get_usage_ledger_sink_factory`);
  `src/usage_ledger_schema.hpp` defines the `UsageEvent` with the same
  forward-compatibility contract as the telemetry schema. Unlike the telemetry
  sink (aggregate, content-free, shard-0 windowed), the usage sink is
  per-request and per-shard: each shard installs its own instance and the
  completion path calls a synchronous, non-blocking `record(UsageEvent)` next to
  the existing `request_attribution` enqueue (both fed from one computed set of
  outcome values, independently gated; skipped entirely when neither is active).
  The event carries the attribution identifiers (`api_key_id`, `request_id`),
  `model`, and estimated token/cost figures — **no** prompt/response content and
  **no** token IDs. Token/cost are the same pre-flight estimates the attribution
  rows record (the upstream response `usage` is not parsed). Independent of
  SQLite persistence. Gated by `usage_ledger.enabled` (default false;
  `RANVIER_USAGE_LEDGER_ENABLED`); toggling requires a restart. The stock build
  wires the Noop sink, so the completion path is a single null check when
  disabled.

- **OpenTelemetry GenAI semantic conventions** (BACKLOG §20.2 P1.6) — The
  `ranvier.request` root span now carries the OpenTelemetry GenAI semconv
  (`gen_ai.*`) attributes, so Ranvier slots into the standard
  LLM-observability stack (Datadog/GCP/Azure map the semconv natively):
  `gen_ai.operation.name` (`chat`/`text_completion`), `gen_ai.provider.name`
  (backend engine class — `vllm`/`sglang`/…; semconv sanctions custom values,
  more honest than labeling a self-hosted backend `openai`),
  `gen_ai.request.model`, `gen_ai.request.max_tokens`,
  `gen_ai.usage.input_tokens` (the exact tokenized count — omitted under
  partial or skipped tokenization rather than reporting a fraction),
  `server.address`/`server.port` (the upstream behind the proxy), and
  `error.type` plus span-status Error on the routing-failure paths. The span
  name stays `ranvier.request` (dashboards key on attributes). Response-side
  usage (output tokens, response model) is intentionally not emitted — the
  request span ends at dispatch handoff, before the response streams back.
  Gated by `telemetry.genai_semconv` (default true;
  `RANVIER_TELEMETRY_GENAI_SEMCONV`); with the flag off behavior is
  byte-identical to before. The `model` field is read during the existing
  single request-body parse, adding no JSON work to the hot path.

- **Native KV-event mode, part 1: verified residency** (BACKLOG §20.1 P0.1) —
  Opt-in subscriber for vLLM's native KV-cache event stream (ZMQ PUB,
  msgpack `KVEventBatch`): `BlockStored`/`BlockRemoved` events maintain
  `prefix_hash_index` as a block-exact, ~ms-fresh mirror of each opted-in
  backend's cache. ART hits on stream-fresh backends are then VERIFIED —
  present = resident (probabilistic gate skipped), absent = evicted
  (downgrade fires with certainty); stream faults (sequence gaps, decode
  errors) reset trust and fall back to the probabilistic gossip signal.
  vLLM block hashes are bridged to Ranvier prefix hashes by incremental
  FNV accumulation along block parent chains (`src/kv_event_ledger.hpp`) —
  no shared hash function needed. Per-block evictions deliberately leave the
  RadixTree untouched (the verified check neutralizes stale routes);
  `AllBlocksCleared` purges. Build option `WITH_KV_EVENTS` (libzmq, default
  ON; `-DWITH_KV_EVENTS=OFF` for a zmq-free server build — unit-test-only
  builds never need libzmq); config `kv_events:` /
  `RANVIER_KV_EVENTS_*`; per-backend opt-in via static `kv_events_port` or
  the `ranvier.io/kv-events-port` annotation. Metrics:
  `router_native_kv_ops_total`, `router_native_verified_hits_total`,
  `router_native_verified_evictions_total`, `router_native_stream_resets_total`,
  `router_native_index_overflow_total`.

- **Native KV-event mode, part 2: route materialization + replay**
  (BACKLOG §20.1 P0.1, completing the item) — `BlockStored` token chains now
  insert `RouteOrigin::PUSH` routes at every covered block boundary
  (`kv_events.materialize_routes`, default on; `max_materialize_tokens`
  bounds per-chain retention), so prefixes computed without Ranvier's
  involvement become routable. Trust order enforced at insertion
  (`RadixTree::insert_if_trusted`: locally-learned routes are never
  overwritten, PUSH outranks gossip; same-backend confirmations LRU-refresh)
  and at eviction (`evict_lowest_trust()`: REMOTE → PUSH → LOCAL, replacing
  `evict_oldest_remote()` and making the documented precedence literal).
  Forward sequence gaps recover via vLLM's replay ROUTER socket (per-backend
  `kv_events_replay_port` / `ranvier.io/kv-events-replay-port`; DEALER
  client, 8-byte big-endian start seq, sentinel-terminated, contiguity
  verified) instead of resetting; `replay_on_connect` backfills the
  publisher's buffered window on subscribe — restart cold-start, bounded by
  that buffer. Shipments are chunked by op weight so token-carrying ops
  can't oversize a reactor application. Metrics:
  `router_native_routes_materialized_total`,
  `router_native_materialize_trust_skips_total`.

- **Disaggregated prefill/decode pool roles** (BACKLOG §20.1 P0.3) — Backends
  carry a `pool_role` (`unified` default / `prefill` / `decode`) through
  registration, acting as a hard eligibility filter on the unified route
  score: fresh cache misses and load/cost/price diverts target only
  prefill/unified backends; decode pools are affinity-only (reached via a
  learned/ART route), preserving decode affinity while new turns go to
  prefill. KV transfer between pools stays with the serving stack
  (NIXL/LMCache). If only decode pools are live the filter is waived
  (availability first) and `router_pool_role_fallbacks_total` counts it.
  Unlabeled fleets route identically to before. Labeling:
  `ranvier.io/pool-role` EndpointSlice annotation, static-backend YAML
  `pool_role:`, admin `POST /admin/backends?...&pool_role=`; persisted in
  SQLite (`pool_role` column, additive migration, default `unified`) and
  surfaced in `GET /admin/backends`.

### Changed

- **Routing defaults: node-local in-flight load signal, scraped GPU/KV terms out of the
  divert signal, bounded-load ε 1.0** (`cross_shard_load_sync` false → **true**,
  `gpu_load_weight` 10 → **0**, `capacity_headroom_weight` 5 → **0**,
  `bounded_load_epsilon` 0.25 → **1.0**). Bounded-load diversion was reading a 5 s-stale
  scraped score that moved every backend together plus one shard's share of the node's
  in-flight count, and at ε 0.25 the cap at ~1 in-flight per backend was 1–2, so it diverted
  25–30% of requests without reaching the tail. With the node's live queue as the signal and
  a divert only at twice the mean, the 13B 20-user fitted row went from +3…+21% P99 TTFT
  against round-robin (twelve runs of placement and divert variants) to **−60.4 / −57.5 /
  −55.2%** across three repeats and both arm orders, P50 −28%, KV prefix hits 69–73%; 13B 10
  users −34.5% (was mixed), 8B 20 users −22.6% (was −17%), 13B 30 users −16.4% (was no effect).
  The scrape still runs for observability and residency routing; the old behaviour is four env
  vars away (`RANVIER_CROSS_SHARD_LOAD_SYNC=false RANVIER_ROUTING_GPU_LOAD_WEIGHT=10
  RANVIER_CAPACITY_HEADROOM_WEIGHT=5 RANVIER_BOUNDED_LOAD_EPSILON=0.25`). The benchmark
  compose and `bench.sh` manifest defaults follow.
- **`miss_placement` defaults to `least_loaded`** (was `hash`). On its own, least-loaded
  placement never moved the 13B 20-user tail (four variants, +3…+21%): the route table cannot
  see which routes carry traffic. Under the live divert policy above it earns its place by
  needing fewer diverts, one home per prefix: against `hash` with the same signal, three
  repeats each on the same box and day, P99 −57.5% vs −51.8% median, KV prefix hits 69–73% vs
  49–55%, route consistency 50–53% vs 39–45%, diverts 23–27% vs 30–33%. The eager learn and
  both convergence rules (gossip and local flush) are what make it split-free; a learn that
  yields at the flush is one that meets a lower-id route for a different backend, and learns
  shorter than `block_alignment` (which store nothing under either placement) still ride the
  batch to other shards and over gossip exactly as before. `hash` is one
  env var away (`RANVIER_MISS_PLACEMENT=hash`) and is the right choice on a single node without
  gossip. Details: docs/benchmarks/benchmark-results-current.md (combo, isolation and
  confirmation legs, 2026-10-05).

- **Least-loaded cache-miss placement** (`routing.miss_placement: least_loaded`, env
  `RANVIER_MISS_PLACEMENT`; introduced with default `hash`, made the default in this
  release, see above) — a prefix with no
  learned route is placed on the live backend holding the fewest learned-route tokens
  (`RadixTree::route_tokens_by_backend`, new: the sum of live route key lengths per
  backend), then the fewest routes, then the lowest capacity-adjusted load, then
  jump-probe order, instead of its consistent-hash bucket. Token-weighted so a long
  shared prefix outweighs the short one-off prompts a real mix also learns as routes.
  A placed miss is learned at dispatch as well as at first byte, so other shards and
  nodes stop placing the same new prefix elsewhere within one route-batch flush
  instead of one TTFT, and a gossiped route that still conflicts with a node's own
  placement is settled by lowest backend id on every node (new
  `router_remote_routes_converged_total`) instead of refused under the trust ladder,
  so the cluster converges on one backend per prefix. The node's own learns obey the
  same order at the local batch flush: a learn that lands after a peer's lower-id
  route arrived is dropped before fan-out and gossip (new
  `router_local_routes_converged_total`) instead of moving the prefix back. New gauge
  `backend_resident_route_tokens` (per backend) exports the placement weight. PUSH
  routes and the default `hash` placement are untouched. Motivated by the 2026-10-02 fitted-suite leg A: with every divert mechanism
  off, prefix affinity still lost 8–12% P99 at 13B/20 users against round-robin,
  because hash placement of 16 prefixes over 8 backends left one backend holding four
  prefixes (23% of requests) and another none (0.6%), and the busiest backend's queue
  sets the tail. A miss has no cache to preserve, so placement is free in cache terms;
  hits are untouched. New counter `router_miss_placements_rebalanced_total`;
  `bench.sh --miss-placement`, `bench-runner.sh --suite placement` (the acceptance
  test: fitted 13B 20u turns negative with the prefix arm's Gini near round-robin's).
  Balances prefix count, not popularity; hot-prefix replication is a separate item.

- **Bounded-load diversion targets the least-loaded backend** — `bounded_load_select`
  used to send an over-cap primary to the first under-cap bucket in jump-probe order,
  and the route scorer's dispatch tie order reproduced the same rule for ART-hit
  diverts. That pushed load off hot anchors without ever pulling it toward the
  coldest backend: every prefix arm of the 2026-10-02 fitted suite left one backend
  35–45% below the fleet mean with 30% of requests already diverted, a consistent
  +10% P99 TTFT regression at 13B/20 users. A divert now goes to the least-loaded
  live candidate (jump-probe order breaks equal loads, keeping equal-load targets
  deterministic), on both the hash-miss and ART-hit paths. An under-cap primary
  still keeps affinity; a uniformly saturated fleet stays on the primary and no
  longer counts a divert. **Acceptance run failed** (same box, 13B 20u P99 vs
  round-robin: +10.5, +24.1, +17.5 against +11.7, +11.0, +3.9 for the previous rule):
  the change balances completions without moving the tail, because in the benchmark
  deployment "load" was the 5 s-stale scraped vLLM score rather than queue depth. It
  ships in this release under the new defaults above, where "load" is the node-local
  in-flight count and the divert target is the backend that is actually coldest; see
  BACKLOG §27 and `docs/benchmarks/benchmark-results-current.md`.

- **Unified weighted route scorer** (BACKLOG §20.1 P0.2) — The post-anchor
  routing decision is now one weighted ranking over the live candidates
  (`src/route_scorer.hpp`) instead of the former sequential override chain
  (residency downgrade → load redirect → cost redirect → hardware-cost
  preference). Stable terms (prefix affinity, hardware price) pick the
  placement / learn target; transient terms (load hinge over the strategy
  allowance, cost-budget hinge) pick the dispatch target. Per-signal weights
  in `routing.scoring.*` (env `RANVIER_SCORING_*`): `prefix_weight`,
  `load_weight`, `residency_weight`, `cost_weight`, `price_weight`, plus a
  reserved `slo_weight` seam. **Neutral defaults reproduce the previous
  pipeline's decisions exactly** — the existing strategy/threshold keys keep
  their authority until weights are tuned. Counter-semantics note:
  `router_load_aware_fallbacks_total` now counts every load-driven ART-hit
  divert under `bounded_load` (the former re-probe skipped diverts landing on
  the primary hash bucket); with cost routing enabled, the budget/fast-lane
  triggers blend continuously instead of overriding sequentially, and the 4b
  divert target is deterministic (least budget pressure) rather than
  random-two-choices.

## [2.1.0] - 2026-04-11

Performance release. Introduces partial tokenization for routing — truncates
input text to a byte budget before tokenizing, since the ART lookup only
needs the first `prefix_token_length` tokens (default 128). Full tokenization
is deferred and only performed when token forwarding to `/v1/completions`
backends is enabled.

### Added

- **Partial Tokenization for Routing** — Two-phase tokenization: a truncated
  input (default 768 bytes, ~128 tokens) is tokenized for routing, and full
  tokenization is deferred to the forwarding path only when needed. Disabled
  automatically when multi-depth routing is enabled or token forwarding
  requires the full token vector. Config: `routing.enable_partial_tokenization`
  (default true), `routing.partial_tokenize_byte_budget` (default 768),
  `routing.partial_tokenize_bytes_per_token` (default 6).
  Env: `RANVIER_PARTIAL_TOKENIZATION`, `RANVIER_PARTIAL_TOKENIZE_BUDGET`.
- **TokenizerService::truncate_for_routing()** — UTF-8-safe byte budget
  truncation utility. Returns a `string_view` into the original text
  (zero-copy).
- **Metrics**: `ranvier_tokenization_partial_total`,
  `ranvier_tokenization_partial_bytes_saved`,
  `ranvier_tokenization_deferred_full_total`.

### Performance

CI benchmark (100 users, 60s, docker-compose mock backends) vs v2.0.0 baseline:

| Metric        | v2.0.0  | v2.1.0  | Delta  |
|---------------|---------|---------|--------|
| P50 latency   | 49ms    | 46ms    | -6%    |
| P90 latency   | 66ms    | 52ms    | -21%   |
| P99 latency   | 85ms    | 59ms    | -30%   |
| Throughput    | 502 rps | 513 rps | +2%    |
| Failure rate  | 0%      | 0%      | —      |

The P99 improvement reflects reduced thread pool queue contention and
context-switch overhead — tokenization still runs off-reactor, but the
smaller input produces tokens faster, freeing thread pool capacity.
Real-world impact on GPU-backed deployments (where tokenization is the
dominant per-request cost) is expected to be even more significant;
re-validation on 8x A100 pending GPU availability.

## [2.0.0] - 2026-04-05

Intelligence Layer release. Transforms Ranvier from a "smart router" into a full
Intelligence Layer for Inference Infrastructure, completing the entire VISION.md
roadmap (Phases 1-4, all 🔓 Core/Open Source items).

### Foundation (Phase 1)

- **Request Cost Estimation (VISION 1.1)** — Heuristic token count and cost derivation
  from request body. Populates estimated_input_tokens, estimated_output_tokens, and
  estimated_cost_units in ProxyContext before routing.
- **Priority Tiers (VISION 1.2)** — Four-tier priority classification (CRITICAL, HIGH,
  NORMAL, LOW) via X-Ranvier-Priority header, User-Agent pattern matching, or cost-based
  defaults. Per-priority metrics.
- **Priority Queue (VISION 1.2 integration)** — RequestScheduler with per-tier bounded
  deques, fair scheduling by agent (oldest-last-served wins), queue-jumping for CRITICAL,
  and per-agent pause-aware dequeue. Replaces direct semaphore acquire when enabled.
- **Intent Classification (VISION 1.4)** — Wire-format inspection classifies requests as
  AUTOCOMPLETE (FIM fields), EDIT (system prompt keywords/tags), or CHAT (default).
  Advisory routing hint for downstream phases.

### Cloud Intelligence (Phase 2)

- **BackendRegistry Interface** — Abstract interface decoupling HealthService and
  LocalDiscoveryService from RouterService. Enables independent testing and clean
  extension for vLLM metrics.
- **vLLM Metrics Ingestion (VISION 2.1)** — Periodic scraping of vLLM Prometheus
  `/metrics` endpoint. Extracts GPU request queue depth, KV cache usage, memory,
  and throughput. Composite load_score() (0.0–1.0) for routing decisions.
  Prometheus text parser included. Graceful degradation for non-vLLM backends.
- **GPU-Aware Load Routing (VISION 2.2)** — Per-shard GPU load cache broadcast from
  shard 0. get_composite_backend_load() blends shard-local in-flight counts with
  vLLM GPU metrics. Integrated into P2C, bounded-load, and median-based routing
  strategies. Configurable gpu_load_weight and load_redirect_threshold.
- **Cost-Based Routing (VISION 2.3)** — Per-backend cost budget tracking. Small-request
  fast lane routes cheap requests to least-cost-loaded backends. Large requests check
  budget headroom before routing. Reserve on route, release on completion.

### Ranvier Local (Phase 3)

- **Local Mode Config (VISION 1.3)** — `local_mode.enabled` flag disables clustering,
  gossip, and persistence. RANVIER_LOCAL_MODE=true environment variable support.
  Auto-enables backend discovery.
- **Local Backend Discovery (VISION 3.1)** — Auto-discovers Ollama, vLLM, LM Studio,
  llama.cpp, LocalAI, and Text Generation WebUI using semantic liveness checks (HTTP
  GET /v1/models with 50ms timeout). Solves the zombie port problem. Hot-add/remove
  with 3-miss hysteresis.
- **Agent-Aware Request Handling (VISION 3.2)** — AgentRegistry identifies agents from
  User-Agent headers and X-Ranvier-Agent custom header. Built-in patterns for Cursor,
  Claude Code, Cline, Aider. Pause/resume via admin API. Per-agent metrics.
- **Request Queuing with Pause/Resume (VISION 3.3)** — Paused agents' requests are held
  in queue (not rejected) and skipped during dequeue. Resume signals the condition
  variable for immediate drain. Per-agent queue depth limits prevent starvation.

### Polish & Release (Phase 4)

- **Single-Binary Local Distribution (VISION 4.1)** — `ranvier --local` CLI starts with
  sensible defaults, no config file needed. Tokenizer auto-search (./assets, ~/.ranvier,
  /usr/local/share/ranvier). Startup banner with discovery info. CMake install targets.
  Homebrew formula skeleton. GitHub release workflow skeleton.
- **Local Dashboard UI (VISION 4.2)** — Vanilla JS dashboard at localhost:9180/dashboard.
  Shows discovered backends, request queue depths, active agents with pause/resume
  controls, and throughput stats. Embedded in binary at compile time. Dark theme,
  5-second auto-refresh, no external dependencies.
- **Documentation & Examples (VISION 4.3)** — Getting Started with Ranvier Local,
  Cloud Deployment Guide, IDE Integration Guide (Cursor, Claude Code, Cline, Aider),
  and Benchmark Reproduction Guide.
- **Re-benchmark** — Full intelligence layer validated under CI load. See Performance below.

### Performance

- **Intelligence Layer Overhead**: All §15 features enabled on mock backend CI benchmark
  (100 users, 60s, docker-compose):
  - P50 latency: 49ms (v1.0: 61ms, -20%)
  - P99 latency: 85ms (v1.0: 140ms, -39%)
  - Throughput: 502 rps (v1.0: 473 rps, +6%)
  - Priority queue scheduler wait: ~1.88ms average
  - Zero failures, zero sync errors
- v1.0 benchmark results on 8x A100 GPUs remain valid for prefix-affinity routing.
  Intelligence layer features add advisory signals; core routing path unchanged.

## [1.0.0] - 2026-03-16

Initial public release. Ranvier Core is a high-performance Layer 7+ LLM traffic controller
that reduces GPU KV-cache thrashing by routing inference requests based on token prefixes,
achieving 33-44% faster Time-To-First-Token for prefix-heavy workloads.

### Core Features

- **Prefix-Affinity Routing** — Adaptive Radix Tree (ART) maps token prefixes to GPU backends,
  steering requests to the GPU that already holds the relevant KV cache.
- **Passive Route Learning** — Routes are learned automatically from backend responses;
  no manual prefix configuration required.
- **Streaming Proxy** — Full SSE (Server-Sent Events) pass-through with zero-copy
  `string_view` parsing and read-position tracking.
- **Multi-Node Clustering** — Gossip protocol (v2) with CRDT-based route synchronization
  across cluster nodes. DTLS-encrypted transport.
- **Backend Discovery** — Static YAML configuration, Kubernetes EndpointSlice watch,
  and DNS-based discovery.
- **Load-Aware Routing** — Shard load metrics with cross-shard speculative load
  synchronization to prevent burst hot-spotting.
- **Circuit Breaker** — Per-backend circuit breaker with configurable thresholds,
  half-open probing, and automatic recovery.
- **API Key Authentication** — Multi-key support with metadata (name, creation date,
  expiry), constant-time comparison, and hot-reload via SIGHUP.
- **Rate Limiting** — Token bucket rate limiter with per-key and global limits.
- **Request Rewriting** — Chat template application and tokenized prompt rewriting
  for vLLM-aligned requests.
- **Configuration Hot-Reload** — SIGHUP-triggered config and API key reload
  without downtime.

### Performance

- **Tokenizer Thread Pool** — Dedicated per-shard worker threads with lock-free
  SPSC queues offload HuggingFace tokenizer FFI calls off the Seastar reactor.
- **Cross-Shard Tokenization Dispatch** — On cache miss, tokenization is dispatched
  to another shard via `smp::submit_to`, keeping the calling reactor responsive.
- **Slab Allocator** — Per-shard node pooling for Radix Tree allocations with
  size-classed pools (Node4/16/48/256) and O(1) free-list recycling.
- **Tree Compaction** — Post-order traversal removes tombstoned nodes and downsizes
  oversized internal nodes to reclaim slab memory.
- **Async Persistence** — Fire-and-forget queue with batched SQLite writes via
  `seastar::async`, decoupled from the request hot path.
- **Batched Route Broadcasting** — Locally-learned routes are batched (configurable
  flush interval, default 20ms) to eliminate per-request SMP storms.
- **Zero-Copy SSE Parsing** — Read-position offset parsing with buffer compaction
  at 50% consumption; no `substr()` copies.
- **Jemalloc Isolation** — Rust tokenizer FFI uses statically-linked jemalloc,
  eliminating memory corruption from Seastar allocator interaction.

### Observability

- **Prometheus Metrics** — Radix tree stats (hits/misses, node counts, slab utilization),
  connection pool metrics, routing decisions, tokenization latency, and queue depths.
- **OpenTelemetry Tracing** — Distributed tracing with Zipkin and OTLP exporters
  (compile-time gated via `WITH_TELEMETRY`).
- **Route Table Metrics** — Route count, estimated memory usage, and per-shard
  tree statistics exposed via admin API.

### Deployment

- **Docker Images** — Multi-stage production builds (`Dockerfile.production`) and
  fast incremental builds (`Dockerfile.production.fast`) for linux/amd64 and linux/arm64.
- **Helm Chart** — Kubernetes StatefulSet with HPA, ServiceMonitor, Ingress,
  and configurable gossip/DTLS settings.
- **GitHub Actions CI** — Automated Docker image publishing and benchmark pipelines.

### Testing

- 40 C++ unit tests (GTest) covering all major subsystems.
- 11 Python integration tests including multi-node cluster, prefix routing,
  graceful shutdown, and negative path validation.
- Locust-based load testing with LMSYS benchmark data.
- Benchmark suite validated on 8x A100 GPUs (30-minute runs).

### Benchmark Results (8x A100, February 2026)

| Model | Cache Hit Rate | TTFT Improvement | P99 Latency |
|-------|----------------|------------------|-------------|
| Llama-3.1-70B (TP=2, 4 backends) | 25% → 98% | 44% faster | ~same |
| CodeLlama-13b (8 backends) | 12% → 58-98% | 33% faster | -60% to -85% |
| Llama-3.1-8B (8 backends) | 12% → 68-98% | 40% faster | flat |
