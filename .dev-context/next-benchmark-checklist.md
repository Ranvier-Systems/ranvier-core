# Next Benchmark Checklist — May 22 2026 Affinity-Thrashing Follow-up

**Branch:** `claude/dreamy-hypatia-b1Xjo` (or whichever ships these checklist items)
**Reference commit:** `5079f9f` (May 22, 2026 reproduction)
**Reading order:** [investigation-289](investigation-289-routing-regression.md)
→ [investigation-may22](investigation-may22-affinity-thrashing-reproduction.md)
→ this file.

The May 22 13B/30m runs at 10u/20u/30u reproduced the affinity-thrashing
failure mode investigation #289 predicted. This checklist runs the
smoking-gun A/B experiments needed to commit to a fix path (revert, threshold
tune, or hysteresis). **Do these experiments first; do not write a code fix
until the data points to one.**

## Pre-flight (every run)

- Confirm `5079f9f` (or descendant, no router changes since) is what's
  deployed.
- Capture `nvidia-smi --query-gpu=clocks.sm,clocks.mem,clocks_throttle_reasons.active --format=csv`
  at start and end (bench.sh now does this automatically; verify
  `$REPORT_DIR/nvidia_smi_{start,end}.csv` exist after each run).
- Curl `/metrics` at end of run to `$REPORT_DIR/prometheus_metrics.txt`
  (bench.sh also does this; verify it exists). The parser surfaces
  `ranvier_routing_load_aware_fallbacks_total` and per-backend
  `ranvier_backend_active_requests` from this dump.
- Always pair each prefix-aware run with a round-robin baseline of the same
  duration on the same instance — no cross-instance comparisons.

## Experiments

Run in this order. Stop early only if Experiment A is conclusive.

### Experiment A — Disable load-aware routing entirely (smoking gun)

**Hypothesis:** If investigation #289 is the root cause, switching off load-aware
routing collapses P99 back to baseline-or-better at 30u and the Mar 5 doc
numbers (`08e5a93`: P99 -79.6%, +13.2% throughput) reappear.

```bash
# 30u/30m, prefix mode, load-aware OFF
RANVIER_LOAD_AWARE_ROUTING=false \
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expA-la-off
```

**Expected if hypothesis is correct:**
- P99 TTFT collapses from +44% to ≤0% vs RR.
- Cache Miss P99 collapses from +69% to ≤+10%.
- `load_aware_fallbacks_total` ≈ 0 (sanity check the env var actually took
  effect — the bench script doesn't validate this).
- 0 incompletes on both sides.

**Expected if hypothesis is wrong:** something else regressed between
`08e5a93` and `5079f9f`. Open a new investigation.

Env var verified to exist: `RANVIER_LOAD_AWARE_ROUTING` (`src/config_loader.cpp:166`).
Toggle is honored uniformly across hash strategies (`src/router_service.cpp:1199, 1281, 2433`).

### Experiment B — Raise the median-comparison threshold (production-shaped fix)

**Hypothesis:** Investigation #289 Fix 1 was right in spirit but the parameter
names are stale; today's equivalents are `load_imbalance_factor` (default 2.0)
and `load_imbalance_floor` (default 2). Raising both should make load-aware
fire only on genuine outliers, not on every request at 3.75 req/GPU.

```bash
# 30u/30m, prefix mode, load-aware ON with raised thresholds
RANVIER_LOAD_AWARE_ROUTING=true \
RANVIER_LOAD_IMBALANCE_FACTOR=3.0 \
RANVIER_LOAD_IMBALANCE_FLOOR=4 \
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expB-thresholds-raised
```

**Expected if hypothesis is correct:**
- P99 better than Experiment A but possibly not as good — load-aware still
  protects against single-backend hotspots that pure consistent-hash would
  miss.
- `load_aware_fallbacks_total / total_requests` < 0.05 (vs whatever it is
  today; currently uninstrumented in the May 22 data).
- 0 incompletes.

**Expected if hypothesis is wrong:** P99 still elevated — the no-hysteresis
problem is structural and threshold tuning can only soften, not solve, the
ART/load-aware misalignment introduced by `#442`. Move to investigating the
ART learning path (`#289 Fix 2` / proper hysteresis).

**Env-var note (verified May 23, 2026):**
- `RANVIER_LOAD_AWARE_ROUTING` → `routing.load_aware_routing` (config_loader.cpp:166)
- `RANVIER_LOAD_IMBALANCE_FACTOR` → `routing.load_imbalance_factor` (config_loader.cpp:169)
- `RANVIER_LOAD_IMBALANCE_FLOOR` → `routing.load_imbalance_floor` (config_loader.cpp:172)

Investigation #289's `queue_depth_threshold` and `queue_diff_threshold`
parameter names **do not exist** in the current code — the implementation
switched to a multiplicative-factor-on-median scheme before they were ever
introduced. Above is the current equivalent. No YAML overlay or code change
needed for Experiment B.

### Experiment C — 20u repeats of A and B (zombie-timeout test)

The 20u/30m run on `5079f9f` had 639 incompletes (6.0%) — the "diverted
request never completes" failure mode predicted at lines 200-220 of
investigation #289. Both A and B should drive this to zero.

```bash
# C1: 20u/30m, load-aware OFF
RANVIER_LOAD_AWARE_ROUTING=false RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 20 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expC1-la-off-20u

# C2: 20u/30m, load-aware ON with raised thresholds
RANVIER_LOAD_AWARE_ROUTING=true \
RANVIER_LOAD_IMBALANCE_FACTOR=3.0 RANVIER_LOAD_IMBALANCE_FLOOR=4 \
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 20 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expC2-thresholds-20u
```

**Expected:** incomplete count goes to 0 in both. If only A clears the timeouts
and B doesn't, the flapping zone is wider than #289 modeled and threshold
tuning alone isn't enough — hysteresis becomes the next experiment.

## Methodology notes

- **Minimum duration:** 30m at 20u+. 10m runs at 20u-30u are not long enough
  to distinguish steady state from warm-up variance — the Mar 5 `08e5a93`
  result that didn't reproduce was a 30m run, so use that as the floor.
- **Always run `--compare`** so prefix-aware and round-robin runs share the
  same backend state and warm-up. Bench.sh does this.
- **Save the Prometheus dump.** The single most important question — "did
  load-aware fire 0%, 5%, or 90% of the time?" — can only be answered from
  the `load_aware_fallbacks_total` counter. The May 22 runs apparently did
  not capture this; the bench.sh changes on this branch ensure future runs
  do.
- **Save nvidia-smi.** Throttle reasons silently destroy benchmarks. If
  `clocks_throttle_reasons.active` is anything other than `0x0000000000000000`
  on multiple GPUs, the run is suspect regardless of routing numbers.
- **Compare using the updated parser.** The Validation/incompletes banner is
  now at the top of the report; the load_aware_fallbacks line and the
  per-backend distribution block sit below the metrics. Read all of them
  before quoting a headline.

## Out of scope for this session

- Writing a code fix. The fix path (revert `#442`, add hysteresis, or just
  raise thresholds in `config_schema.hpp` defaults) must follow from
  Experiments A/B/C data, not from this static analysis. Flag for human
  decision per the task brief.
- Adding `nvidia-smi`/Prometheus scraping to the locust-only path
  (`tests/integration/run-benchmark.sh`). The harness fix is in
  `scripts/bench.sh`, which is the production path. Anyone running the legacy
  script will need a parallel change.
