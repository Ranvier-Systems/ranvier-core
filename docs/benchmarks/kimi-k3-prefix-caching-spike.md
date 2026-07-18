# Kimi K3 Prefix-Caching Spike (Runbook)

**Status:** ready to execute; blocked only on a K3-on-vLLM GPU box.
**Owner:** _tbd_  **Date run:** _tbd_  **Result:** _tbd_
**Backlog:** §26 (item D).

## Why this spike exists

Ranvier's entire value proposition for Kimi is prefix-affinity routing: send
requests that share a token prefix to the backend that already holds that
prefix's KV blocks, so the backend gets a prefix-cache hit and TTFT drops. That
assumes the backend's KV cache is **block-structured and prefix-reusable** the
way vLLM's Automatic Prefix Caching (APC) is for a normal transformer.

Kimi K3 introduces **Kimi Delta Attention (KDA), a hybrid *linear* attention**.
Linear-attention layers carry a recurrent state rather than per-token KV blocks,
so for those layers there may be no prefix "blocks" to reuse, and the KV-event
stream Ranvier ingests may look different or be absent. The full-attention layers
in the hybrid stack should still cache — but *whether prefix caching produces a
meaningful hit-rate/TTFT win for K3, and whether Ranvier's KV-event path still
works, is unverified.* This spike answers that empirically before we invest in a
full benchmark campaign.

We validate on K2 first (pure attention, known-good) as a control, then K3.

## The three questions (in dependency order)

1. **Does vLLM prefix caching even help K3?** (model-architecture question —
   isolate it on a *single* backend, using vLLM's own metrics, before Ranvier is
   in the loop.)
2. **Does the KV-event stream work for K3?** (Ranvier's `KvEventSubscriber`
   ingests `BlockStored`/`BlockRemoved` without decode errors.)
3. **Does Ranvier prefix routing beat random for K3?** (the payoff: cache-hit and
   TTFT lift, A/B prefix vs random across a multi-backend fleet.)

If Q1 fails, Q2/Q3 are moot and the conclusion is "prefix routing does not help
K3" — a real, valuable finding. Each question gates the next.

## Pre-registered decision rules (write the verdict criteria BEFORE running)

- **Q1 PASS** if, on a single backend replaying a shared long prefix, vLLM's
  prefix-cache-hit metric climbs to roughly the K2 control's level (expect
  high-tens to ~90% on warm repeats) **and** TTFT on a cache hit is materially
  below a cold request (target ≥30% TTFT reduction; K2 control sets the bar).
  **FAIL** if hit-rate stays near zero or TTFT shows no cache benefit → K3's
  hybrid attention does not yield reusable prefix cache under this vLLM build.
- **Q2 PASS** if `ranvier`'s KV-event ingestion advances (events processed > 0,
  `decode_errors == 0`, ledger/residency state populated) over a run that stores
  and evicts prefixes. **FAIL** if the stream is empty or decode errors accrue →
  the decoder or the vLLM K3 event schema needs work (decoder is forward-compat,
  so this most likely means "K3/vLLM emits no usable KV events").
- **Q3 PASS** if `routing_mode: prefix` shows a materially higher cache-hit ratio
  and lower mean/P50 TTFT than `routing_mode: random` on the same workload and
  fleet (the standard-model reference is ~81% vs ~49% cache hit; K3 need not
  match that, but must show a clear, significant separation). **FAIL / INCONCLUSIVE**
  if prefix ≈ random within noise → routing gives K3 no edge; revisit whether K3
  is a fit for prefix-affinity routing at all.

Record the numbers even on FAIL — a negative result is the deliverable.

## Prerequisites & GPU budget

- **Weights/tokenizer:** K3 weights served by a **vLLM build that supports KDA**.
  Produce and **validate the fast `tokenizer.json`** first (K3 ships tiktoken
  only) — see `tests/tokenizer_parity/` (convert → `kimi_tokenizer_parity.py`
  must pass) — and pin the *same* file on vLLM and Ranvier.
- **Fleet:** ≥2 K3 backends for Q3 routing (Q0–Q2 need 1). K2 control backend(s)
  for the baseline.
- **Ranvier:** built with `WITH_KV_EVENTS=ON` (default). `chat_template_format: kimi`.
- **Budget:** ~2–4 GPU-hours for a first pass (bring-up + Q1/Q2 on one box, a
  short Q3 A/B). This is a spike, not the full campaign in
  `benchmark-rebaseline-campaign.md`.

## Phase 0 — Bring-up + validity gates

1. Serve K3 on vLLM with prefix caching **and** KV events on. Confirm the exact
   flags against your vLLM version (names change across releases):
   ```bash
   # VERIFY flags against the K3-capable vLLM build:
   vllm serve <k3-model> \
     --enable-prefix-caching \
     --tokenizer <path>/kimi_fast/tokenizer.json \
     --kv-events-config '{"enable_kv_cache_events": true, "endpoint": "tcp://0.0.0.0:5557"}'
   # ^ endpoint/topic must match Ranvier's kv_events_port for that backend.
   ```
2. Point Ranvier at it (`ranvier.yaml`):
   ```yaml
   routing:
     routing_mode: prefix
   assets:
     chat_template_format: kimi
     tokenizer_path: "<path>/kimi_fast/tokenizer.json"   # SAME file as vLLM
   kv_events:
     enabled: true
     # per-backend kv_events_port matching the vLLM endpoint above
   ```
3. **Validity gates (all must hold before trusting any number):**
   - A chat request round-trips and streams from K3 through Ranvier.
   - Ranvier and vLLM agree on token counts for a fixed prompt (the tokenizer
     parity guarantee from `tests/tokenizer_parity/` — re-confirm here live; if
     they disagree, STOP and fix the tokenizer, everything downstream is invalid).
   - `chat_template_format: kimi` is active (startup log line).

## Phase 1 — Does vLLM prefix caching help K3? (single backend, no Ranvier routing)

Bypass routing (hit one vLLM directly). Send a batch that shares a long system/RAG
prefix, then repeat it warm.

```bash
# Cold then warm replays of a shared-prefix workload against ONE backend.
# Reuse the existing Locust profile (long shared prefix) pointed at vLLM directly:
scripts/bench.sh --target http://<k3-backend>:8000 --workload shared-prefix   # VERIFY flags
```
Observe vLLM's own metrics on its `/metrics`:
- prefix-cache hit rate (e.g. `vllm:gpu_prefix_cache_hit_rate` / prefix cache
  queries vs hits — **VERIFY the exact series in your vLLM build**),
- TTFT cold vs warm.

Run the **identical** procedure against a **K2** backend as the control. Apply the
Q1 decision rule (K3 vs its own cold baseline, sanity-checked against K2).

## Phase 2 — Does the KV-event stream work for K3?

With `kv_events.enabled: true`, drive the same store/evict workload and watch
Ranvier's metrics on `:9180`:

```bash
watch -n2 'curl -s http://<ranvier>:9180/metrics | \
  grep -E "kv_event|router_cache_(hits|misses)|radix_tree_lookup_(hits|misses)_total|radix_tree_node_count"'
```
- KV-event ingestion counters advance; **`decode_errors` stays 0** (the decoder is
  forward-compatible — unknown tags are skipped, not fatal — so persistent zero
  ingestion means "no usable events emitted," which is itself the finding).
- `radix_tree_node_count` grows as prefixes are learned. Apply the Q2 rule.

## Phase 3 — Does prefix routing beat random for K3? (A/B)

Mirror the existing residency A/B pattern
(`docs/benchmarks/cache-residency-ab-benchmark.md`, `scripts/bench-residency-ab.sh`)
but vary `routing_mode` across a ≥2-backend K3 fleet:

```bash
# A: prefix-affinity
RANVIER_ROUTING_MODE=prefix  scripts/run-multi-gpu-benchmark.sh --tag k3-prefix   # VERIFY flags
# B: random (control — skips tokenization/routing)
RANVIER_ROUTING_MODE=random  scripts/run-multi-gpu-benchmark.sh --tag k3-random
```
Compare with `scripts/run_benchmark_comparison.py`. Primary metrics:
- cache-hit ratio: Ranvier `router_cache_hits / (hits+misses)` and vLLM-side
  prefix-cache hit rate;
- TTFT mean/P50/P95;
- throughput (tps) as a guardrail (prefix routing must not cost throughput).

Hold workload, fleet size, and duration identical between A and B. Apply the Q3
rule. Repeat the whole A/B on **K2** as the control so "prefix ≫ random" is known
to reproduce on this rig before judging K3.

## Data capture

Record into `docs/benchmarks/history/` (same convention as other runs):

| Q | Metric | K2 (control) | K3 | Verdict |
|---|--------|--------------|----|---------|
| 1 | vLLM prefix-cache hit % (warm) | | | |
| 1 | TTFT cold → warm | | | |
| 2 | KV events processed / decode_errors | | | |
| 2 | radix_tree_node_count growth | | | |
| 3 | cache-hit % prefix vs random | | | |
| 3 | TTFT P50 prefix vs random | | | |
| 3 | tps prefix vs random | | | |

## Failure-mode playbook

- **Q1 fails (no prefix-cache benefit for K3):** the headline conclusion. Prefix
  routing can't help what the backend won't cache. Capture it in §26; consider
  whether K3 should route by a cheaper signal (load/random) instead, and whether
  the KDA layers fundamentally preclude prefix reuse in this vLLM build.
- **Q1 passes, Q2 fails (caching works but no KV events):** prefix routing can
  still work via ART learning + the `/v1/cache/events` HTTP path; native
  KV-event verified residency is just unavailable. Note which vLLM K3 build
  emits (or doesn't emit) `KVEventBatch`, and whether the schema differs from the
  decoder's expectations (`src/kv_event_decoder.hpp`).
- **Q3 inconclusive (prefix ≈ random):** check the validity gates first
  (tokenizer parity live, fleet actually ≥2, workload actually shares prefixes);
  a broken gate is the usual cause before concluding K3 gains nothing.

## References

- `docs/benchmarks/benchmark-methodology.md` — measurement discipline.
- `docs/benchmarks/cache-residency-ab-benchmark.md` — the A/B pattern this mirrors.
- `docs/internals/prefix-affinity-routing.md` — backend KV-cache prerequisites,
  per-`BackendType` applicability, the ~81% vs ~49% reference numbers.
- `tests/tokenizer_parity/` — the tokenizer contract this spike depends on.
- The `/benchmark` skill — for designing/running the Locust A/B.
