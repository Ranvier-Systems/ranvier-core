# Strategic Assessment — 2026-10-02

External-CTO review of Ranvier Core against its stated goal: **a Layer 7+ LLM traffic
controller with prefix-affinity routing that measurably reduces GPU KV-cache thrashing.**
Prompted by the 2026-10-01 re-baseline, the first GPU campaign on tooling the 2026-09-30
audit had fixed. Evidence is cited inline; every number traces to a file in this tree or
to `docs/benchmarks/results/2026-10-01-rebaseline/`.

> **Addendum, 2026-10-05.** The 13B 20-user regression this assessment cites (+17.4 / +11.0 /
> +4.6) is resolved: it was the load-divert policy, not affinity concentrating onto evicting
> backends. Bounded-load diversion read a 5 s-stale scraped score plus one shard's share of the
> node's in-flight count, at ε 0.25, and diverted 25–30% of requests without reaching the tail.
> With the node-local in-flight signal, ε 1.0 and least-loaded placement with convergence (now the
> defaults), the same row is **−57.5% median P99** across three repeats, 13B/10u −34.5%, 8B/20u
> −22.6%. The §4 recommendation to give the scorer a KV-occupancy term still stands for the
> eviction regime (13B/30u, 50 prefixes, timeouts in both arms), but it was not what the 20-user
> row needed. Record: `docs/benchmarks/benchmark-results-current.md`, 2026-10-03..05 sections.

## Scorecard

| Area | Grade | One line |
|---|---|---|
| Architecture | **B** | Shared-nothing core is sound and the pure components (`radix_tree.hpp`, `route_scorer.hpp`) are clean, but 10.3k of the 49k source lines sit in two files with no fuzz coverage, and the scorer weighs six signals except the one that decided the latest campaign. |
| Reliability | **B−** | 45.6k lines of unit tests against 49k of source, four fuzz targets, integration suite. But the measurement layer shipped 15 accuracy defects through three GPU campaigns, and nobody noticed until an audit. The code is tested; the claims were not. |
| Progress to goal | **C+** | After nine months, exactly one configuration (Llama-3.1-8B, 20 users) demonstrates the goal with a real cache metric: KV hit rate 72% → 94%, P99 TTFT −17%, three of three repeats. Every 13B row was measured in a regime where no router could help. Meanwhile the product grew sideways. |

## 1. Goal alignment

**Is this a prefix-caching balancer or a general proxy?** By mass, a proxy with a routing
core. `src/` is 49,019 lines: routing 11.3k, HTTP/proxy 7.5k, gossip/cluster 6.3k,
infrastructure 6.0k, config 4.0k, telemetry/usage 2.4k, persistence 2.3k, KV events 1.5k,
GIE EPP 0.5k. The `[Unreleased]` changelog adds Kimi templates, an admission-policy seam,
response-side usage accounting, a usage-ledger sink, OpenTelemetry GenAI conventions, GIE
EPP mode, and disaggregated prefill/decode roles. `docs/architecture/VISION.md` names the
goal as an "Intelligence Layer for Inference Infrastructure" with two product lines. The
code followed the vision, and the vision is broader than the stated goal.

**Are the prefix constraints real?** Yes. The ART (`radix_tree.hpp`, fuzzed), the
route-learning path, the unified scorer and the KV-event subscriber exist and work. What
is missing is narrower and more damaging: the scorer (`route_scorer.hpp:42-77`) weighs
prefix affinity, in-flight load, gossiped residency, cost, price and SLO, and has **no
term for KV-cache occupancy**. `health_service.cpp:425-428` already scrapes vLLM's
`kv_cache_usage_perc` into `vllm_metrics.hpp`, but dispatch never sees it. "Load" in
load-aware routing is `active_requests` (`router_service.cpp:84, 544-552`), a count of
in-flight requests, which is blind to the variable that decided the 13B rows.

**Do the numbers support the headline?** The 2026-07-13 headline does not survive; see
`docs/benchmarks/benchmark-results-current.md`. On fixed tooling:

| Row | P99 TTFT per repeat | Verdict | KV hit RR → prefix | KV/backend |
|---|---|---|---|---|
| 8B 20u | −17.0, −6.5, −17.4 | consistent improvement | 72% → 94% | 142,144 tok |
| 13B 30u | −2.4, −1.6, +3.6 | no reliable effect | 5% → 14% | 11,648 tok |
| 13B 20u | +17.4, +11.0, +4.6 | consistent regression | 4% → 19% | 11,648 tok |
| 13B 10u | +12.2, −10.8, +6.1 | no reliable effect | 6% → 26% | 11,648 tok |

The "~3× cache hit rate" of every earlier headline was the client-side route-consistency
proxy, not a cache measurement; the real KV lift at 8B is 72 → 94. The 13B rows were run
with a ~250k-token hot set against 11.6k tokens of KV per backend: both arms cache-cold,
216–277 vLLM preemptions per backend per 45 min. The deciding variable is not model size
but whether a backend's KV cache can hold its share of the hot set: CodeLlama-13B is an
MHA model (~0.8 MB KV/token) while Llama-3.1-8B is GQA (~0.13 MB/token), a 12× capacity
difference on the same card. The one row inside the favourable regime is the one that
works. **The product's value proposition is therefore conditional on a quantity the
product does not measure at runtime and does not act on.**

## 2. Complexity vs value

- **`router_service.cpp` (5,985 lines) and `http_controller.cpp` (4,346 lines)** carry
  the request path. The 24 Hard Rules exist largely to keep these two files safe. Neither
  has a fuzz target (`tests/fuzz/`: radix tree, request rewriter, stream parser, KV-event
  decoder). This is where a KV-pressure term must be added, and it is the hardest place
  in the tree to change safely.
- **Gossip/cluster (6.3k lines: DTLS, UDP transport, consensus, crypto offload, topology
  index)** supports a 3-node Ranvier tier. Every GPU campaign has run all three nodes on
  one host. There is no measurement of what multi-node sync buys at the routing layer
  and no multi-host benchmark. This is the clearest case of infrastructure built ahead of
  evidence.
- **Hash strategies `JUMP`/`MODULAR` with `load_imbalance_factor/floor`** are dead under
  the shipped `bounded_load` default (`router_service.cpp` `compute_load_allowance`) and
  produced an entire "threshold leg" that measured nothing (BACKLOG §25 item 5, D2). Knobs
  that cannot take effect under the default are negative value: they generate wrong
  experiments.
- **Telemetry/usage ledger/GPU-seconds accounting (2.4k lines)** and the autoscaling
  telemetry plan (BACKLOG §21) build signals for external consumers on top of a cache
  metric that, until last week, was a proxy. No consumer has been shown.
- **The route scorer itself (268 lines, pure, tested)** is the right shape. The problem is
  its inputs, not its design.

## 3. Hidden fragility

**Load-bearing files:** `router_service.cpp`, `http_controller.cpp`, `radix_tree.hpp`,
`stream_parser.cpp`, `config_loader.cpp`. The last three are fuzzed or pure and
well-tested. The first two are guarded by rules and unit tests but not by fuzzing, and
they are the files every routing change touches.

**The measurement layer was the weakest link, not the data plane.** Fifteen confirmed
defects in the benchmark code survived three campaigns and a methodology review
(`.dev-context/benchmark-accuracy-audit-2026-09-30.md`). The project's engineering
discipline on the C++ side (Hard Rules, audits, fuzzing) was never applied to the
Python and bash that produced its public claims. That asymmetry is the systemic risk.

**Next big risk, from the backlog:** 102 open items across 26 sections, with the largest
buckets in Benchmark Extensions (16), Developer Experience (13) and the Intelligence
Layer roadmap (11). Two specific items would spend GPU time in the wrong regime:
the "powered V0 rerun" of cross-shard load sync at 13B/10u (~9 GPU-hours, §25) and the
epsilon leg as currently specified at 13B on the default 50-prefix set. Both would
measure eviction noise. Run them, if at all, on a set that fits.

## 4. Staff-engineer recommendation

**Delete:** the `JUMP`/`MODULAR` hash strategies and the `load_imbalance_factor/floor`
knobs, with their config, compose and docs surface. They are unreachable under the
default, they have already cost a GPU campaign, and `bench.sh` had to grow a refusal
check just to stop them being misused. Keep `bounded_load` and `p2c`.

**Refactor immediately:** make backend "load" a composite pressure signal that includes
KV-cache occupancy, and give the scorer a KV term. Concretely: extend `BackendLoad` with
the scraped `gpu_cache_usage_percent`, add a `kv_pressure` candidate field and weight in
`route_scorer.hpp`, and let dispatch divert off an anchor whose KV is near full the same
way it diverts off one whose in-flight count is over allowance. This is the one change
that addresses the 13B 20-user regression at its mechanism (affinity concentrating onto
backends that are already evicting) and it is squarely inside the stated goal. Pair it
with a fuzz target on the dispatch path, because `router_service.cpp` is where it lands.

## 5. Direction

1. **Reframe the claim.** Not "prefix routing cuts P99 by X" but "prefix routing cuts
   P99 when the fleet's KV cache can hold its share of the hot set, and Ranvier detects
   which regime you are in." The bench banner now computes that ratio; the product
   should too, at runtime, from the KV capacity vLLM reports and the ART's working set.
2. **Act on the regime.** KV-aware dispatch (above) in the near term; a runtime advisory
   ("fleet is in eviction regime; affinity degraded to load-only") as the operator-facing
   form of the same signal. This supersedes the load-gating proposal's Option A/B
   framing, which gated on throughput, a correlate of the real variable.
3. **Prove the mechanism properly.** The four-arm design (direct-to-vLLM, random,
   least-loaded without affinity, prefix) is the only way to separate affinity from load
   balancing and to measure Ranvier's own cost. A least-loaded mode is a small addition.
   Then a capacity sweep (working set ÷ KV) at fixed load, and a multi-turn workload.
4. **Stop growing sideways until the core claim is settled.** Freeze new Intelligence
   Layer, telemetry-export and model-template work. Keep GIE EPP maintained (it is the
   deployment path into Kubernetes gateways) but do not expand it.
5. **Hold the benchmark code to the data plane's standard.** Unit tests on the parser and
   scrape layer now exist; add them to CI, keep the preflight in the runbook, and treat a
   headline number without a manifest and a KV hit rate as not a result.

## Addendum (same day): the fitted suite landed

13B inside its cache: 10 users −5.8% P99, consistent (KV hits 17% → 65%); 20 users **+10.1%,
consistent** (+10.1, +8.2, +21.4; KV hits 14% → 45%; preemptions single digits). The 20-user
regression is therefore not memory pressure. All six prefix arms show one backend 35–45% below
the mean while round-robin is flat; `bounded_load_select` pushes load away from over-cap
anchors to the first under-cap hash probe and never pulls toward the coldest backend, so with
ε 0.25 a stranded eighth of the fleet is invisible to the policy and becomes tail latency once
the fleet is queue-bound. The §4 recommendation is revised in order: **least-loaded diversion
first** (small change, acceptance test = fitted 20u turns positive), KV-aware dispatch second
(it protects the eviction regime), and the epsilon leg re-aimed tighter rather than looser.
The four-arm design remains the way to separate affinity from load balancing in general; the
fitted suite has already done so for this one failure.
