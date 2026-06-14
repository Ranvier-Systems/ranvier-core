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

## Results (fill from your run)

> Single-stream, mock backend, single-shard Ranvier. Record the host so the
> numbers are interpretable; a dev laptop gives a directional floor (see
> [interpreting-benchmark-numbers.md](interpreting-benchmark-numbers.md)).

| arm | p50 (ms) | p90 (ms) | p99 (ms) |
|---|---|---|---|
| A — inline | _tbd_ | _tbd_ | _tbd_ |
| B — plain Envoy | _tbd_ | _tbd_ | _tbd_ |
| C — Envoy + EPP | _tbd_ | _tbd_ | _tbd_ |

| delta | p50 (ms) | p99 (ms) |
|---|---|---|
| ext_proc overhead (C − B) | _tbd_ | _tbd_ |
| inline-vs-sidecar (C − A) | _tbd_ | _tbd_ |

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
