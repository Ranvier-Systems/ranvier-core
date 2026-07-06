# 50-Prefix Re-Baseline Campaign — Runbook

**Goal:** produce the first **citable** prefix-aware-vs-round-robin headline under the
representative (50-prefix) workload, on current shipped defaults, with variance stats — and
decide whether the raised load-aware thresholds should become the default. This fills the
`TBD` in [benchmark-results-current.md](benchmark-results-current.md).

This is BACKLOG §25 items **4–6**. It is the **only** work that needs GPU time; all the
supporting tooling (repeats + median/IQR verdicts, manifests, 3-node telemetry, fair A/B
ordering) already landed (§25 items 7–10). **Do not hand-copy a single run into a table** —
that is exactly how the old guide accumulated contradictions.

> **Hardware:** 8× A100 40GB, vLLM **v0.15.1**, `HF_TOKEN` exported. Budget **a full GPU
> day** (~10–15h wall clock; see [§5](#5-gpu-time-budget)).

---

## 0. Pre-flight (do this once, before any long run)

1. **Confirm the defaults are actually in effect.** Start any run and read the
   **"Effective Routing Config"** banner bench.sh prints at second 0. It MUST show:

   | Knob | Required value |
   |------|----------------|
   | `NUM_LARGE_PREFIXES` | **50** (locust default) |
   | `SHARED_PREFIX_RATIO` / prefix-ratio | **0.9** |
   | `RANVIER_LOAD_AWARE_ROUTING` | **true** |
   | `RANVIER_LOAD_IMBALANCE_FACTOR` / `FLOOR` | **2.0 / 2** |
   | `RANVIER_CACHE_RESIDENCY_THRESHOLD` | **0.2** |
   | Route Batch Flush Interval | **20ms** |
   | prompt distribution | **stress** |

   If any differ, stop and fix — a run on the wrong config is wasted GPU time and is
   incomparable to every other run.

2. **Record the commit.** `git rev-parse --short HEAD`. Every report dir also gets a
   `manifest.json` stamping this automatically.

3. **Clean GPU state.** No leftover vLLM/containers: `./scripts/bench.sh` (setup path) or
   `nvidia-smi` should show ~40GB free per GPU.

---

## 1. Standard matrix (item 4) — the headline

Four configs, each `--compare` (prefix vs round-robin), each **×3 repeats**. Everything rides
current defaults; only model/users/duration/max-model-len are set.

| Config | Model | Users | Duration |
|--------|-------|-------|----------|
| 13B 30u/30m | CodeLlama-13b-Instruct-hf | 30 | 30m |
| 13B 20u/10m | CodeLlama-13b-Instruct-hf | 20 | 10m |
| 13B 10u/10m | CodeLlama-13b-Instruct-hf | 10 | 10m |
| 8B 20u/10m | Llama-3.1-8B-Instruct | 20 | 10m |

Run file: [`rebaseline/standard-matrix.runs`](rebaseline/standard-matrix.runs). One command:

```bash
HF_TOKEN=hf_xxx ./scripts/bench-runner.sh \
  --suite custom \
  --file docs/benchmarks/rebaseline/standard-matrix.runs \
  --repeat 3 \
  --output-dir benchmark-reports/rebaseline/matrix
```

`--repeat 3` runs each config 3×, **alternates the A/B arm order across repeats** (rr-first,
prefix-first, rr-first — cancels order bias), and writes a per-config median/IQR aggregate to
`benchmark-reports/rebaseline/matrix/aggregates/agg_*.json`. Preview first with `--dry-run`
(expands to 12 runs, ~7.5h).

**The headline for each config is the verdict in its aggregate JSON** — not any single run.

---

## 2. Threshold leg (item 5) — 2.0/2 vs 3.0/4

D2 (2026-05-26) validated the **raised** thresholds `factor 3.0 / floor 4` at 50 prefixes
(P99 **−6%**, 60% hit, incompletes below baseline). The **shipped** defaults `2.0 / 2` were
**never tested at 50 prefixes**. This leg decides whether 3.0/4 should replace them.

Run **both** thresholds fresh (same session = same thermal/vLLM state → clean comparison),
13B 30u/30m, `--compare` ×3, each into its **own** output dir so the prefix arms glob cleanly:

```bash
HF_TOKEN=hf_xxx ./scripts/bench-runner.sh --suite custom \
  --file docs/benchmarks/rebaseline/threshold-2.0-2.runs \
  --repeat 3 --output-dir benchmark-reports/rebaseline/thr_2.0_2

HF_TOKEN=hf_xxx ./scripts/bench-runner.sh --suite custom \
  --file docs/benchmarks/rebaseline/threshold-3.0-4.runs \
  --repeat 3 --output-dir benchmark-reports/rebaseline/thr_3.0_4
```

> **Reuse option (saves ~3.5h):** the `thr_2.0_2` leg is identical to the matrix's 13B-30u
> config. You *may* skip it and reuse the matrix's 13B-30u **prefix** dirs as the baseline —
> at some comparability cost (different session/thermal state). The manifests record both, so
> the choice is auditable. Recommended only if GPU time is tight.

Then compare the two thresholds' **prefix arms** (median-of-3), discriminating metric P99 TTFT:

```bash
python3 tests/integration/results_parser.py aggregate \
  benchmark-reports/rebaseline/thr_3.0_4/*_prefix/ \
  --baseline benchmark-reports/rebaseline/thr_2.0_2/*_prefix/ \
  --metric p99_ttft_ms --json benchmark-reports/rebaseline/threshold-verdict.json
```

(The two legs differ only in the `routing` block of the manifest, not `workload`, so this does
**not** trip the workload-mismatch warning — that guard is correct here.)

### Pre-registered decision rule (item 5)

> **Adopt `factor 3.0 / floor 4` as the new default IFF** the median P99 improvement of 3.0/4
> over 2.0/2 is **≥ 10%** (i.e. the `aggregate` verdict is `IMPROVEMENT` with median ≤ −10%)
> **AND** there is **no incomplete-rate regression** (3.0/4's incomplete rate ≤ 2.0/2's within
> noise). Otherwise **keep 2.0/2**. If the verdict is `NO RELIABLE EFFECT`, keep 2.0/2 (the
> shipped default wins ties). Record the verdict either way.

Adopting means editing `src/config_schema.hpp` + `docker-compose.benchmark-real.yml`; that is a
**separate PR**, not part of this campaign.

---

## 3. Pre-registered analysis rules (item 6) — decide these BEFORE looking

- **Median-of-3, not best-of-3.** The citable number is the median across the 3 repeats.
- **IQR spans zero ⇒ "no reliable effect."** If the middle 50% of the per-repeat %change
  straddles zero on the discriminating metric, the verdict is *no reliable effect* — report
  that, do not cherry-pick the favorable run. (`aggregate` enforces this automatically.)
- **Discriminating metric = `p99_ttft_ms`.** Throughput and cache-hit rate are secondary.
- **Incompletes count.** A run that "wins" P99 by dropping requests has not won. Check the
  incomplete rate on both arms; a P99 improvement with an incomplete-rate regression is not a
  win. (This is the Instance-9 trap the review flagged.)
- **Hot-spot / Gini fingerprint.** In each `compare`, check `nodes scraped: … = 3`,
  `load_aware_fallbacks_total` %, and the prefix-arm Gini. High fallbacks-% **and** high Gini =
  affinity thrash — investigate before trusting the headline.

---

## 4. Read & record the results

For each standard-matrix config, read its aggregate verdict:

```bash
cat benchmark-reports/rebaseline/matrix/aggregates/agg_*.json | \
  python3 -c 'import json,sys; [print(json.loads(l).get("verdict")) for l in sys.stdin]' 2>/dev/null
# or just read the human-readable runner log / the per-config aggregate printout
```

Then update [benchmark-results-current.md](benchmark-results-current.md): replace the
**"Representative-workload headline: TBD"** section with a table of the four configs' **median**
P99 %change + verdict, each stamped with the **commit** and a pointer to its `manifest.json`.
Record the threshold decision (adopt 3.0/4 or keep 2.0/2) with the verdict JSON. Only then is
the headline "filled."

Sanity gates before recording — a run is **invalid** (re-run it) if:
- the effective-config banner didn't show the required defaults (§0), or
- `nodes scraped` < 3 in the compare output (partial telemetry), or
- a `WORKLOAD MISMATCH` warning fired across repeats of one config, or
- warm-up did not complete on both arms, or
- incomplete rate is abnormally high (investigate; don't average a broken run in).

---

## 5. GPU-time budget

| Phase | Runs | Est. wall clock |
|-------|------|-----------------|
| Standard matrix (4 configs ×3) | 12 | **~7.5h** (bench-runner `--dry-run` estimate) |
| Threshold leg, both fresh (2×13B-30u ×3) | 6 | **~7h** |
| Threshold leg, reusing matrix 2.0/2 (3.0/4 only ×3) | 3 | **~3.5h** |
| **Total** | | **~11h (reuse) – ~15h (fresh)** |

Allow 2–3 min GPU cooldown between runs (bench-runner's default `--pause 60` plus restarts).
Budget a full GPU day and don't interleave other workloads on the box — thermal drift is a
confound the alternation only partly cancels.

---

## 6. One-line summary of what "done" looks like

`benchmark-results-current.md` has a four-row headline table (median P99 %change + verdict +
commit + manifest per config) and a recorded threshold decision — replacing today's `TBD` —
and every number is reproducible from the manifests under `benchmark-reports/rebaseline/`.

---

*Runbook for BACKLOG §25 items 4–6. Tooling prerequisites (items 7–10) are complete. See
[benchmark-methodology.md](benchmark-methodology.md) for knob/verdict reference.*
