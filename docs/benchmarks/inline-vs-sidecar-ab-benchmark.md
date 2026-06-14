# Inline vs. Sidecar (GIE EPP) Routing-Overhead A/B

Measures the per-request latency a deployment pays for serving through a GIE
gateway that delegates endpoint selection to Ranvier's EPP, vs. Ranvier's inline
data plane — the headline BACKLOG §20.2 P1.4 proof point. Design rationale and
the decisions behind this layout are in
[inline-vs-sidecar-ab-scope.md](inline-vs-sidecar-ab-scope.md); the EPP's
in-isolation decision cost is in
[epp-overhead-microbenchmark.md](epp-overhead-microbenchmark.md).

This is **Phase 1**: mock backend, three arms, single-stream latency — the clean
overhead number. The vLLM realism pass (does the hop matter against real TTFT)
is the GPU-gated follow-up.

## Arms (one backend, one Ranvier)

| arm | path |
|---|---|
| **A — inline** | client → Ranvier `:8080` (tokenize + route + proxy) → backend |
| **B — plain Envoy** | client → Envoy `:10000` → backend (no ext_proc) |
| **C — Envoy + EPP** | client → Envoy `:10001` → ext_proc(Ranvier EPP `:9002`) → backend |

The single EPP-enabled Ranvier serves both the inline plane (A) and the picker
(C). Both arms run the *same* `RouterService` decision, so the latency delta is
attributable to the request path, not different routing.

- **ext_proc-specific overhead = C − B** — same proxy, with vs. without the EPP
  call. The honest "what delegating to the EPP costs."
- **inline-vs-sidecar delta = C − A** — the operator-facing number.
- **B − A** — Envoy-proxy vs. Ranvier-proxy, for context.

**Phase-1 simplification:** with one backend, arm C's Envoy routes statically to
that backend rather than honoring `x-gateway-destination-endpoint` — but the
ext_proc filter still makes the full `Process` call every request, so the
*overhead* is measured faithfully. Header-driven multi-backend routing is a
realism-phase extension (see the scope memo).

## Running

```sh
make bench-inline-vs-sidecar                                   # 2000 reqs/arm
make bench-inline-vs-sidecar BENCH_AB_ARGS="--requests 5000 --warmup 500"
```

`scripts/bench-inline-vs-sidecar.sh` brings up `docker-compose.epp-ab.yml`,
registers a backend, runs `tests/integration/http_ab_load.py` against all three
arms (single-stream by default), prints per-arm p50/p90/p99 and the deltas, then
tears down. Requires Docker + a `WITH_GIE_EPP=ON` build + Python `requests`.

## Results

### Dev baseline (2026-06-14 — illustrative, not canonical)

Single-stream, 2000 reqs/arm, single-shard Ranvier + mock backend, Docker on a
macOS dev laptop. A floor — reproduce on a representative host before quoting.
The ~54 ms baseline is the **mock backend's response time**, identical across
arms, so it cancels in the deltas; the deltas are the signal.

| arm | p50 (ms) | p90 (ms) | p99 (ms) |
|---|---|---|---|
| A — inline | 54.8 | 59.1 | 68.9 |
| B — plain Envoy | 53.9 | 57.3 | 60.2 |
| C — Envoy + EPP | 55.4 | 59.0 | 61.9 |

| delta | p50 (ms) | p99 (ms) | mean (ms) |
|---|---|---|---|
| **ext_proc overhead (C − B)** | **+1.5** | **+1.7** | **+1.4** |
| inline-vs-sidecar (C − A) | +0.6 | −7.0 | +0.1 |

**Reading it:**

- **The ext_proc sidecar hop costs ~1.5 ms/request** (C − B, both Envoy-fronted
  with tight tails, so this is the clean signal). That cross-checks with the
  [microbenchmark](epp-overhead-microbenchmark.md): the EPP's own decision is
  ~0.5 ms, and the remaining ~1 ms is Envoy's ext_proc machinery (buffering the
  body, the gRPC call from Envoy's side, the extra filter pass).
- **Inline vs. sidecar is within ~0.6 ms at p50 / ~flat at the mean.** The
  C − A p99 came out *negative* here only because the single inline run had a
  180 ms outlier inflating arm A's tail; with Envoy fronting B/C their tails are
  tighter. Treat the C − A p99 as noise from one run, not a real "sidecar is
  faster" result — re-run for a stable tail.
- Bottom line: the EPP sidecar adds **single-digit-millisecond** per-request
  overhead, dominated by the ext_proc indirection, not Ranvier's routing.

> Reproduce on a representative, isolated host (and ideally several runs for
> stable tails) before publishing; see
> [interpreting-benchmark-numbers.md](interpreting-benchmark-numbers.md).

## Caveats

- **Fairness is the whole game.** Identical workload, backend, host, and warm-up
  across arms; the runner registers one backend so all arms route to the same
  place. Re-run if the host is under other load.
- **Mock backend by design.** A real model's inference time (100s of ms) would
  swamp a µs–ms routing delta; the mock isolates the path overhead. The mock's
  fixed response time cancels in the deltas.
- **Single-stream by default.** `--concurrency N` is available for a throughput
  view, but the clean per-request overhead is the sequential measurement.
- **Phase 1 / dev-floor.** Envoy config + compose are unverified outside a
  Docker host; the published figure should come from a representative box, and
  the vLLM realism pass is still outstanding.
