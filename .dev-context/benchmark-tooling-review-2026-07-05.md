# Benchmark Tooling & Methodology Review — 2026-07-05

**Scope:** `scripts/bench.sh`, `scripts/bench-runner.sh`, `docs/benchmarks/benchmark-guide-8xA100.md`,
plus the surrounding ecosystem (other `scripts/bench-*`, Makefile targets, `tests/integration/`
harness, `.github/workflows/benchmark.yml`, other `docs/benchmarks/*.md`, `.dev-context` records).

**Purpose:** planning-level review — what should change in the benchmark tooling, docs, and
suite shape before the next GPU campaign. No code changes made; this is the plan.

---

## Verdict (TL;DR)

The *tooling* is in decent shape — `bench.sh` is battle-hardened and encodes real lessons
(effective-config banner, `NUM_LARGE_PREFIXES=50` default, KV-fit autosizing, `--build-image`
staleness guard). The *methodology and documentation* are not:

1. **Every headline number in the 8xA100 guide was measured on the now-deprecated 5-prefix
   workload.** The May→June affinity-thrashing investigation concluded `NUM_LARGE_PREFIXES=5`
   against 8 backends *manufactures* both the failure modes and (by concentration) much of the
   headline win. At 50 prefixes the only data points we have are D1/D2 (P99 +19% pure-affinity,
   **-6%** load-aware with raised thresholds) — a very different story from the advertised
   -80%/-85%. There is currently **no citable headline under the representative workload.**
2. The guide still presents the thrashing investigation as *unresolved* (TL;DR note) when it was
   closed 2026-05-26 as a workload artifact; the pigeonhole finding made it into
   `kv-cache-prefix-routing-benchmark.md` but never into the main guide.
3. Run-to-run variance is the #1 documented confound (2-of-4 runs hot-spotting; identical
   back-to-back configs giving P99 -37% vs +31%), yet the tooling has **zero statistical
   machinery** — no repeats, no aggregation, no outlier detection. "Run 2x for variance" is done
   by hand and recorded by hand into markdown tables, which is exactly how the guide accumulated
   contradictions.

The single most valuable "new benchmark" is therefore not a new shape at all: it is a
**re-baseline campaign of the existing matrix at 50 prefixes with repeats**, backed by small
tooling changes (repeat/aggregate support, full-cluster metrics capture, run manifests) and a
doc restructure that separates methodology from the historical lab notebook.

---

## Findings

### F1 — Headline results are anchored to a deprecated workload (highest impact)

- Guide TL;DR (`docs/benchmarks/benchmark-guide-8xA100.md:5-44`) advertises P99 -80%…-85%,
  +13-22% throughput. All source runs (Instances 1-9, Jan–Apr 2026) predate the
  `NUM_LARGE_PREFIXES` 5→50 default change (`scripts/bench.sh:1665-1671`), i.e. they ran the
  pigeonhole workload.
- `next-benchmark-checklist.md` "Status (2026-05-26) — INVESTIGATION COMPLETE": at 50 prefixes,
  load-aware ON with raised thresholds gave **P99 -6%** (D2); pure affinity gave +19% (D1).
  The -76%-class wins only appear at 5 prefixes (with 9.4% timeouts).
- The guide's May 22 note still says the headline "should not be quoted … until the
  affinity-thrashing root cause is resolved." It *was* resolved — as a workload artifact — but
  the guide was never updated (the sibling `kv-cache-prefix-routing-benchmark.md` was).
- Internal contradiction left in place: "10ms confirmed as the correct default" (invalidated
  c219fbd section, line ~1102) vs "20ms confirmed as optimal … change default from 10 to 20"
  (line ~1975-2006). The 20ms change **shipped** (`src/config_schema.hpp:182`,
  `docker-compose.benchmark-real.yml:171`); the guide never reconciled.

**Consequence:** any new run on current defaults is incomparable to every table in the guide,
and the product's advertised numbers are not currently reproducible by the tool that
allegedly produced them.

### F2 — The 5-prefix trap is still live everywhere except bench.sh

- `tests/integration/locustfile_real.py:592` — `NUM_LARGE_PREFIXES` defaults to **5**.
- `tests/integration/run_benchmark_comparison.py:639` — `--num-prefixes` defaults to **5**
  (used by `make benchmark-comparison`).
- Only `bench.sh` overrides to 50 (`scripts/bench.sh:1671,1838`). Direct locust runs, the
  comparison driver, and the deprecated wrappers all still reproduce the false-regression
  workload silently.
- Same class of drift: `SHARED_PREFIX_RATIO` defaults to 0.7 in the locustfile
  (`locustfile_real.py:634`) but 0.9 via bench.sh; churn knobs (200/24/20/8/seed 42) are
  hardcoded in three places (`locustfile_real.py:618-621`, `bench.sh:1675,1842`,
  `bench-residency-ab.sh:77-81`) — they agree today, by luck.

### F3 — No statistical machinery despite variance being the top confound

- Documented: transient hot-spotting in 2-of-4 identical runs (guide lines 1907-1910);
  identical configs back-to-back at -37% vs +31% P99 (Instance 6, line 1129-1138); the guide's
  own advice is "run 2x for variance."
- Tooling reality: `bench.sh` runs once; `bench-runner.sh` runs each config once;
  `results_parser.py compare` takes exactly two logs. No `--repeat`, no median/IQR, no
  hot-spot outlier flag, no "insufficient confidence" verdict. Aggregation across runs is a
  human with a markdown table — the mechanism by which the guide accumulated errors.

### F4 — A/B design biases in `--compare`

- **Fixed arm order** (round-robin always first, `bench.sh:1939-1946`), so thermal drift and
  cache carry-over always land on the same side.
- **vLLM state carries across arms.** Ranvier containers are removed between arms (good — clears
  `/tmp/ranvier.db`), but the vLLM processes keep running: the RR arm sprays every prefix
  across all backends, so the prefix arm's "misses" can be vLLM-prefix-cache-warm. Direction of
  bias varies; it is unmeasured.
- **Warm-up runs once, before the first arm, under prefix mode**, against a cluster that is then
  discarded by the mode restart — the two arms are not identically warmed (`bench.sh:1787-1877`).
- `bench-residency-ab.sh --order` already establishes the precedent for order control.

### F5 — Telemetry capture gaps

- **Prometheus scrape captures one node, not three:** the loop in `bench.sh:1768-1775` writes
  all three nodes to the *same* `prometheus_metrics.txt` and `break`s on the first success —
  diversion counters and per-backend distribution from ranvier-bench1 only, on a 3-node cluster.
- **No machine-readable run manifest.** Commit, argv, banner config, workload knobs, GPU type,
  vLLM version are scattered across `run_*.log` prose. Nothing enforces comparability when
  comparing two report dirs.
- The locust-side "cache hit rate" is a same-backend heuristic, not server ART hits (guide
  admits this, line ~2249). Server counters should be authoritative — which makes the
  one-node scrape worse.

### F6 — Structural / hygiene issues

- `bench.sh` is a 2,011-line monolith; the warm-up block duplicates ~80 lines of
  `run_benchmark()`'s docker invocation (drift risk); the `$()`-capture return pattern forces
  the `>&2` discipline everywhere.
- `bench-runner.sh` duplicates `parse_duration`/logging, and re-implements metric extraction
  with grep/regex (`extract_metrics`, lines 150-218) instead of calling `results_parser.py`.
- Help-text drift: `bench.sh --model` help says default `Llama-3.2-1B-Instruct` (line 239);
  actual default is `Llama-3.1-8B-Instruct` (line 40). `bench-runner.sh` help says suite
  `high` is "~45 min" in one place and "~1.5h" in another; "medium - high + 3 medium-priority
  runs" but defines 4; priority comment says "low = runs 8-10" but defines 8-11.
- Prompt-dist naming drift: guide documents `large_prefix`, locustfile spells it
  `large-prefix`, bench.sh help omits it entirely (but adds `churn`, which the guide's
  parameter tables don't mention).
- Two deprecated scripts still shipped and executable (`run-multi-gpu-benchmark.sh`,
  `setup-lambda-benchmark.sh`); the latter heredoc-generates a repo-root `run-benchmark.sh`
  that name-collides with `tests/integration/run-benchmark.sh` (the CI baseline comparator).
- `benchmarks/cache_event_generator/` (push-cache-eviction harness, Phase 4) is an orphan:
  no Makefile target, outside both `scripts/` and `tests/integration/`.
- `.github/workflows/benchmark.yml` and `tests/integration/bisect-benchmark.sh` copy-paste the
  same compose/locust invocation; P99-CSV-column-18 extraction duplicated with the parser.
- Naming trap: `make bench` (real GPU via bench.sh) vs `make benchmark` (mock) vs
  `make benchmark-real` (external vLLM) are three different things one word apart.

### F7 — Suites and CI encode yesterday's questions

- `bench-runner.sh` suites are the February "re-run Jan baselines with load-aware routing"
  campaign — completed months ago. Nothing in any suite covers: the 50-prefix re-baseline,
  the shipped-thresholds-at-50-prefixes gap (D2 used factor 3.0/floor 4; shipped defaults
  2.0/2 were **never tested at 50 prefixes** — explicitly flagged in the checklist), churn/
  residency workloads, trace replay, `--priority-queue`, or `--multi-depth` (both flags exist,
  neither has ever been in a suite).
- Cross-shard sync re-evaluation (guide "Priority 3") has sat **Pending since February**.
  Decide: schedule it or formally drop it.
- CI (`benchmark.yml` + `benchmark-baseline.json`) gates only mock-backend p99 (+10%) /
  throughput (-5%) / failures. **No cache-hit-ratio or routing-quality gate exists anywhere**
  — a routing-quality regression reaches a GPU campaign before anything automated notices.

---

## Recommended plan

### P0 — Re-anchor the truth (cheap; mostly no GPU time)

1. **Kill the 5-prefix default at the source.** `locustfile_real.py` `NUM_LARGE_PREFIXES`
   5→50, `run_benchmark_comparison.py` `--num-prefixes` 5→50; align `SHARED_PREFIX_RATIO`
   (pick 0.9 as the single default); make wrappers pass churn knobs through only when set so
   the locustfile is the single source of truth.
2. **Restructure the guide.** Split `benchmark-guide-8xA100.md` (2,536 lines, ~60% lab
   notebook) into:
   - `benchmark-methodology.md` — how to run, knobs, banner discipline, pitfalls, A/B rules
     (fold in the `/benchmark` skill gotchas and `interpreting-benchmark-numbers.md` pointers);
   - `benchmark-results-current.md` — only numbers valid on **current defaults**
     (50 prefixes, 20ms flush, residency 0.2), each stamped with commit + manifest;
     until the re-baseline runs, this page honestly says "representative-workload headline: TBD";
   - `docs/benchmarks/history/` — the instance-by-instance tables, invalidated sections, and
     re-run plans, moved verbatim (append-only archive).
   Fix the TL;DR to state that pre-June numbers were measured on the 5-prefix workload, and
   reconcile the 10ms/20ms flush contradiction (20ms shipped).
3. **Delete the deprecated scripts** (`run-multi-gpu-benchmark.sh`,
   `setup-lambda-benchmark.sh`) or make them exit non-zero with a pointer to bench.sh.

### P0/P1 — The one campaign that matters: 50-prefix re-baseline

4. Standard matrix on current defaults (load-aware ON 2.0/2, residency 0.2, 20ms flush,
   50 prefixes, stress dist, ratio 0.9): **13B 30u/30m, 13B 20u/10m, 13B 10u/10m,
   8B 20u/10m — each `--compare`, ×3 repeats.** This produces the first citable headline
   under the representative workload.
5. Add the checklist's open threshold leg: 13B 30u/30m, factor 2.0/floor 2 vs 3.0/floor 4
   (D2 only validated the raised thresholds). Decision rule pre-registered: adopt 3.0/4 as
   default only if median P99 improves ≥10% with no incomplete-rate regression.
6. Pre-register the variance rule: report median-of-3; if the IQR spans zero on the
   discriminating metric, the verdict is "no reliable effect," not the best run.

### P1 — Statistical + capture machinery (pays for itself on the first campaign)

7. **`--repeat N`** (in bench-runner, operating per-config) + aggregation in
   `results_parser.py`: median/IQR per metric across repeat dirs, hot-spot outlier flag
   (signature: prefix-arm P99 ≫ RR with high hit rate), explicit refuse-to-conclude output.
8. **Run manifest**: write `manifest.json` into every report dir (commit, full argv, effective
   routing config — the banner, as JSON — workload knobs, GPU name/count, vLLM version, host).
   `compare` refuses (or loudly warns) when manifests differ on workload knobs. This ends the
   hand-copied-table failure mode.
9. **Scrape all 3 nodes** to `prometheus_metrics_node{1,2,3}.txt` (fix the `break` bug),
   aggregate in the parser; emit per-backend request distribution + Gini + both diversion
   counters as first-class report fields (the thrash signature, currently reconstructed by hand).
10. **A/B fairness**: add `--order` (rr-first | prefix-first | alternate-across-repeats) —
    alternation across the 3 repeats cancels order bias for free; document (or optionally
    reset) vLLM cache carry-over between arms; run the warm-up per-arm after each mode restart
    so both arms are identically primed.

### P2 — Consolidation & hygiene

11. Refresh `bench-runner.sh` suites around the current questions (re-baseline matrix,
    threshold leg, churn/residency quick, trace-replay leg); fix help-text counts; have it
    call `results_parser.py` instead of grep; share the duplicated bash helpers (or fold the
    runner into `bench.sh --suite`).
12. Unify the config surface: `--flush-interval` and `--cross-shard-sync` flags that export +
    echo in the banner (the guide currently instructs env prefixes for exactly these — the
    same trap class that burned the load-aware experiments).
13. De-duplicate the warm-up block inside bench.sh; fix the `--model` help default; document
    `large-prefix` and `churn` consistently across help + guide.
14. Decide cross-shard sync P3: schedule into a suite or mark formally dropped in the guide.
15. Wire `benchmarks/cache_event_generator/` into make + `scripts/README.md`, or move it
    under `tests/integration/`.

### P3 — New benchmark shapes (after the re-baseline, not before)

16. **Multi-turn conversation workload** exercising `--multi-depth` (route storage at message
    boundaries) with cache-hit accounting beyond system-message-only — closest to the actual
    product story (agents/conversations), never benchmarked.
17. **Trace-replay leg in the standard suite** (`--prompt-file` with the lmsys shared-prefix
    set) as the realism check on synthetic-stress conclusions.
18. **Priority-queue/agent-simulation leg** (`--priority-queue` exists, never measured).
19. **CI routing-quality gate**: deterministic mock workload asserting a server-side
    cache-hit-ratio band in `benchmark.yml`, so routing regressions fail PRs instead of
    burning a GPU day. Cheap and currently the biggest coverage hole in CI.
20. **Goodput/SLO metric** in the parser (e.g. % requests with TTFT < X, incompletes counted
    as failures). The Instance-9 10u case — where RR "won" P99 by dropping 412 requests —
    shows P99-only reporting actively misleads; a completion-weighted metric prevents that
    class of misreading.

### Explicitly not recommended now

- **Rewriting bench.sh in Python.** It is a monolith, but a battle-hardened one whose comments
  encode months of lessons; the marginal-value order is: fix capture/statistics first,
  restructure later if it keeps growing.
- New model sizes / 70B / compression legs before the re-baseline — they'd inherit the same
  workload-validity problem.

---

*Sources: scripts/bench.sh, scripts/bench-runner.sh, docs/benchmarks/benchmark-guide-8xA100.md,
docs/benchmarks/kv-cache-prefix-routing-benchmark.md, .dev-context/next-benchmark-checklist.md,
.dev-context/claude-locust-sync-map.md, tests/integration/{locustfile_real.py,
results_parser.py, benchmark-baseline.json, run_benchmark_comparison.py, bisect-benchmark.sh},
.github/workflows/benchmark.yml, docker-compose.benchmark-real.yml, src/config_schema.hpp,
Makefile.*
