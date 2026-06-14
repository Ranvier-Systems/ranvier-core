# Inline vs. Sidecar (GIE EPP) Routing-Overhead A/B — Scope

**Status:** scoping memo. **Phase 1 implemented** (mock-backend 3-arm A/B) — see
[inline-vs-sidecar-ab-benchmark.md](inline-vs-sidecar-ab-benchmark.md) and
`make bench-inline-vs-sidecar`. Phase 2 (vLLM realism) not started.
**Date:** 2026-06-14
**Context:** BACKLOG §20.2 P1.4 ("pair with a published benchmark of inline
routing vs. a sidecar ext_proc EPP hop, measuring the absolute per-request
routing-overhead delta — a number not currently published anywhere").

This memo specifies the headline benchmark. The
[EPP overhead microbenchmark](epp-overhead-microbenchmark.md) already measures
one side of it — the EPP's own per-request decision + bridge cost, in isolation
(sub-ms on a dev floor). This A/B measures the **end-to-end delta a deployment
actually pays**: serving a request through a GIE gateway that delegates endpoint
selection to Ranvier's EPP, vs. through Ranvier's inline data plane.

## 1. The thesis under test

Ranvier's positioning is that the **inline data plane is the primary, lower-
latency path**, and the EPP is a *compatibility* mode for GIE-conformant
gateways. This benchmark quantifies the cost of that compatibility: how many
microseconds/milliseconds per request does the ext_proc sidecar hop add over
inline routing? A credible, published number lets operators make the inline-vs-
sidecar trade-off with data instead of intuition.

## 2. Topologies

- **Inline (arm A):** `client → Ranvier :8080 → backend`. Ranvier tokenizes,
  routes (`route_request`), and proxies the request itself.
- **Sidecar / EPP (arm C):** `client → Envoy → [ext_proc → Ranvier EPP :9002
  picks endpoint] → Envoy forwards to the chosen backend → backend`. Ranvier
  only *picks*; Envoy proxies.

Both arms run the **same RouterService decision logic**, so the routing
*outcome* is identical — only the request path differs. That is what makes the
latency delta attributable to the indirection rather than to different routing.

### 2.1 The third arm (recommended): isolate the ext_proc-specific cost

A bare 2-arm A/B (A vs C) conflates two things: the ext_proc round-trip **and**
"Envoy proxies vs Ranvier proxies." To separate them, add:

- **Plain Envoy (arm B):** `client → Envoy (no ext_proc, static/LB route) →
  backend`.

Then:
- **ext_proc-specific overhead** = `C − B` (same proxy, with vs without the EPP
  call) — the cleanest "what does delegating to the EPP cost" number.
- **inline-vs-sidecar delta** = `C − A` — the operator-facing "what does running
  the sidecar instead of inline cost" number.
- `B − A` characterises Envoy-proxy vs Ranvier-proxy, useful context.

## 3. Methodology

Mirror the discipline in [cache-residency-ab-benchmark.md](cache-residency-ab-benchmark.md):
identical workload, backend, and host across arms; warm-up before timing; enough
samples for stable tails; report p50/p90/p99 TTFT and total latency plus
throughput and (if cheap) gateway/router CPU. A run is only meaningful if the
routing *outcome* matches across arms (same backend distribution) — assert that,
not just latency.

### 3.1 Backend: mock vs. real vLLM — the central decision

The overhead delta is **µs–ms**; a real model's inference time is **100s of ms**.
With vLLM, the routing-hop delta is a rounding error on TTFT and the A/B mostly
measures the model. To get the headline *overhead* number cleanly, use the
existing deterministic `mock_backend` (fast, fixed-latency). A vLLM arm is worth
running **only** for end-to-end realism context (does the hop matter against real
TTFT — almost certainly not, which is itself the point), and it needs GPUs.

Recommendation: **Phase 1 = mock backend** (the publishable overhead number),
**Phase 2 = optional vLLM realism pass** (GPU-gated).

### 3.2 Load generator

Reuse the existing Locust harness (`tests/integration/locustfile.py`,
`run_benchmark_comparison.py`) pointed at each arm's ingress, so throughput +
concurrency behaviour is captured with tooling the repo already knows. A
latency-focused tool (`ghz`/`wrk`) could supplement for tight single-stream
percentiles, but is not required for v1.

## 4. Infrastructure to build

1. **Envoy configs** (net-new — no Envoy in the tree today):
   - `bootstrap-eppp.yaml`: HTTP listener → ext_proc filter targeting
     `ranvier-epp:9002` (request_body BUFFERED so the picker tokenizes), routing
     on the `x-gateway-destination-endpoint` header the EPP sets.
   - `bootstrap-plain.yaml`: the same listener with a static/round-robin cluster
     and no ext_proc (arm B baseline).
   We adapt the upstream GIE reference Envoy config rather than author from
   scratch.
2. **Compose topology** `docker-compose.epp-ab.yml`: Ranvier (one container in
   inline mode for arm A; one in EPP mode reused by arm C), Envoy (arms B/C),
   `mock_backend`(s), and Locust — wired so each arm has a clean ingress on a
   distinct host port. Reuses the `Dockerfile.gie-epp` builder image for the EPP
   Ranvier (as `docker-compose.epp-test.yml` does).
3. **Runner + parser**: a `scripts/bench-inline-vs-sidecar.sh` that runs the
   three arms under identical Locust load and a small results table emitter
   (extend `run_benchmark_comparison.py`).
4. **Results doc**: `docs/benchmarks/inline-vs-sidecar-ab-benchmark.md`
   (methodology + a results table to fill from the operator's run), mirroring the
   cache-residency-ab layout.

## 5. Phasing

- **Phase 1 (mock A/B, no GPU):** Envoy configs + compose + runner; produce the
  ext_proc-overhead and inline-vs-sidecar deltas against the mock backend.
  Buildable largely without special hardware; runnable on a single beefy host
  (a laptop gives a directional floor, like the microbenchmark).
- **Phase 2 (vLLM realism, GPU):** add a vLLM backend arm for end-to-end TTFT
  context. Operator-run on GPUs.

Each phase is its own PR; Phase 1 is the bulk and the headline number.

## 6. Risks / caveats

- **Unverifiable here:** Envoy config + multi-container compose + load runs are
  all Docker/hardware — the developer runs them; expect config iteration
  (the ext_proc filter wiring + header-based routing is the fiddly part).
- **Fairness is everything:** any difference in backend, host load, warm-up, or
  routing outcome invalidates the delta. The runner must pin all of these and
  assert outcome-equivalence.
- **Laptop floors vs published figures:** as with the microbenchmark, a dev-host
  run is directional; the published number should come from a representative,
  isolated host (see `interpreting-benchmark-numbers.md`).

## 7. Open questions for the reviewer

1. **Backend:** Phase 1 mock-only for the clean overhead number (rec), or insist
   on a vLLM arm in v1 for realism?
2. **Arms:** 3-arm (inline / plain-Envoy / Envoy+EPP, isolating ext_proc cost —
   rec) or just 2-arm (inline vs Envoy+EPP)?
3. **Load tool:** reuse Locust only (rec), or add `ghz`/`wrk` for tighter
   single-stream latency?
4. **Where Phase 1 runs:** your Docker host / a representative box — and who runs
   the GPU Phase 2?
5. **Bar for "published":** is the microbenchmark floor + a Phase-1 mock A/B
   enough to publish the overhead claim, or is the vLLM realism pass required
   before we state it?
6. **Envoy provenance:** adapt the upstream GIE reference Envoy config (rec) vs.
   hand-author a minimal bootstrap?

Answer these and I'll turn it into the Phase-1 implementation PR.
