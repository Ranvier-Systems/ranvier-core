---
name: benchmark
description: Plan, run, and interpret Locust load benchmarks (mock-backend and real-GPU) including A/B routing experiments. Use when the user wants to measure cache-hit ratio, TTFT, or throughput, compare routing modes, chase a benchmark regression, or design a benchmark experiment.
argument-hint: [what-to-measure]
---

## BENCHMARK TASK

**Goal:** $ARGUMENTS

> Ref: `.dev-context/claude-context.md` for architecture. Benchmarks run on the developer's machine or GPU rig, **never in this sandbox** — your deliverables are the exact commands, the experiment design, and the interpretation of results the developer pastes back.

---

## RUNNERS

| Runner | Command | What it measures |
|--------|---------|------------------|
| Mock-backend load test | `make benchmark` | Routing/proxy behavior without GPUs. Docker Compose benchmark profile; builds images inline (set `SKIP_BUILD=1` to reuse); reports to `benchmark-reports/` |
| CI regression | `.github/workflows/benchmark.yml` | Same, on PRs and main; baseline in `tests/integration/benchmark-baseline.json` |
| Real GPU A/B | `scripts/bench.sh` | TTFT / cache-hit against real vLLM backends |
| Regression bisect | `tests/integration/bisect-benchmark.sh` | Which commit moved the number |
| Micro/EPP | `make bench-hot-prefix`, `make bench-epp`, `make bench-inline-vs-sidecar` | Component-level costs |

## HARD-WON GOTCHAS (read before any long run)

These come from real lost benchmark days — see `.dev-context/next-benchmark-checklist.md`:

1. **Env-var prefixes do NOT work with bench.sh.** `bench.sh` unconditionally exports `RANVIER_LOAD_AWARE_ROUTING=true`, overwriting any `VAR=x ./bench.sh` prefix. Use the CLI flags: `--no-load-aware`, `--load-imbalance-factor <N>`, `--load-imbalance-floor <N>`, `--cache-residency-threshold <N>`, `--client-tokenize`, `--multi-depth`, `--compare`.
2. **Confirm the "Effective Routing Config" banner** bench.sh prints at startup matches the intended experiment *before* letting a 30-minute run proceed. A mislabeled run is worse than no run.
3. **Routing has multiple diversion layers** (prefix affinity, load-aware override, cache-residency override — unified scoring in `src/route_scorer.hpp`). An A/B that toggles one layer while another stays active measures nothing. State explicitly which layers are on in each arm.
4. **Python/C++ coupling is silent.** If the change touched `router_service.cpp`, `http_controller.cpp`, `request_rewriter.hpp`, or metrics names, verify `.dev-context/claude-locust-sync-map.md` first — drift produces *wrong metrics*, not errors.
5. **SQLite state persists across runs.** For clean comparisons, remove the container (not just stop it) to clear `/tmp/ranvier.db`, re-register backends, and warm up identically.

## EXPERIMENT DESIGN (A/B discipline)

- One variable per experiment. Same model, duration, user count, spawn rate, prompt distribution, and prefix ratio across arms.
- Decide the discriminating metric and the decision rule **before** running ("if p99 TTFT gap < X%, the override is not the cause").
- Do not write a code fix until the data points to one — run the smoking-gun experiment first.
- Record every run: date, commit (`git rev-parse --short HEAD`), full command line, banner config, and the report path. Multi-run campaigns get a checklist file in `.dev-context/` (follow `next-benchmark-checklist.md` as the format).

## INTERPRETING RESULTS

Key metrics and where they come from:
- **Cache-hit ratio** — `ranvier_router` metrics (`:9180`); the point of the product. Prefix mode target vs consistent-hash baseline is documented in `docs/internals/prefix-affinity-routing.md` (~81% vs 49%).
- **TTFT p50/p99** — Locust report; the headline claim is ~48% TTFT improvement, so regressions here are release blockers.
- **Affinity thrashing** — cache-hit oscillation + backend flip-flopping under load; see `.dev-context/investigation-may22-affinity-thrashing-reproduction.md` for the signature.
- Compare against `tests/integration/benchmark-baseline.json`; results parser: `tests/integration/results_parser.py`.

---

## OUTPUT FORMAT

1. **Experiment plan:** arms, fixed variables, discriminating metric, decision rule
2. **Exact commands** for each arm (flags, not env prefixes), plus reset steps between arms
3. **What to paste back** (report files, banner output, metric snapshots)
4. **Interpretation** once results arrive: verdict against the decision rule, and next step (fix / more data / `/incident` if it's a regression)
