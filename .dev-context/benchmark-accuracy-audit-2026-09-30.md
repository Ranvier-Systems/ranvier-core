# Benchmark Code Accuracy Audit — 2026-09-30

**Scope:** Every benchmark path in the repo, read for whether it measures what it claims:
`tests/integration/locustfile_real.py`, `locustfile.py`, `results_parser.py`,
`run_benchmark_comparison.py`, `run-benchmark.sh`, `bisect-benchmark.sh`, `mock_backend.py`,
`benchmark-baseline.json`, `.github/workflows/benchmark.yml`; `scripts/bench.sh`,
`bench-runner.sh`, `bench-residency-ab.sh`, `bench-epp-overhead.sh`,
`bench-inline-vs-sidecar.sh`; `docker-compose.benchmark-real.yml`, `docker-compose.test.yml`;
`tests/integration/epp_microbench.py`, `tests/bench/hot_prefix_bench.cpp`,
`benchmarks/cache_event_generator/`; and the docs that cite their output.

**Method:** Static read only (no builds, no runs). Locust claims were checked against the
pinned Locust 2.24.0 source. Every finding marked CONFIRMED was traced end to end in the
code; SUSPECTED means the mechanism is real but the magnitude or trigger was not proven.
Follow-up to `.dev-context/benchmark-tooling-review-2026-07-05.md`; that review's P0/P1
items (manifests, `--repeat`/`aggregate`, three-node scrape, `--order`, per-arm warm-up,
50-prefix default) did land and are not re-raised here except where the fix is incomplete.

---

## Verdict

The A/B **direction** of the July 2026 headline (prefix routing lowers P99 TTFT on a
saturated fleet, raises it at low load) is not undermined: both arms run the same client
code path, pairs are aligned by repeat, sign conventions are right, and arm order is
alternated. Almost every **absolute number and label** around that headline is wrong or
overclaimed:

| Reported as | Actually is |
|---|---|
| "Cache hit rate", "~3× higher", "12→48%" | Same-backend routing-consistency proxy. Never reads a KV-cache signal. Round-robin arm is ≈1/N by construction. |
| "~47 req/s", "~16 req/s", "crossover between ~16 and ~28 req/s" | Locust *event* rate: ~6× the HTTP request rate in the real workload, 2× in CI. |
| P99 TTFT to 0.1%, "IQR −15.6…−8.5" | Locust's approximated table, quantized to 100 ms above 1 s. Tool never prints a Q1…Q3 range. |
| "✅ reliable improvement" (n=3) | Q3 = mean of the two worst repeats. One repeat at +15% with two at −20/−30% still passes as "reliable". |
| Routing / Tokenization / Connect "P50" and "P99" | One shard's *mean*, printed in both columns. Histogram regex can never match Seastar's `shard` label. |
| CI "P99 latency" gate | 98th percentile of a mixed TTFT + total-time sample over a mock with a hard 40 ms floor. |
| "Fresh Ranvier per arm clears `/tmp/ranvier.db`" | DB is a host bind mount shared by all three nodes and never deleted. Every prefix-mode start restores the previous run's routes. |
| `--load-imbalance-factor / --load-imbalance-floor` | Ignored under the shipped default `bounded_load` strategy; banner and manifest do not say which strategy ran. |

---

## Tier 1 — Distorts published numbers

### 1. "Cache hit rate" is a routing-affinity proxy, not a cache-hit rate — Sev 8, CONFIRMED
- `locustfile_real.py:2641-2672` `record_request`: hit ⇔ same `X-Backend-ID` as the previous
  request with this prefix hash; first sight = miss; a route change = miss and re-learn. No
  vLLM `prefix_cache_hits`/`cached_tokens` or Ranvier `cache_hits_total` is read.
- Under random routing the proxy is 1/N (12.5% at 8 backends) regardless of what vLLM holds.
  With 50 prefixes every backend holds every prefix after a few hundred requests, so the real
  RR cache-hit rate is near 100%. `docs/benchmarks/interpreting-benchmark-numbers.md:23-26`
  describes the proxy as if it were reality.
- Within-arm "TTFT improvement (hit vs miss)" (`:2713`, `:2747`) compares "routed same as last
  time" vs "routed differently"; on the RR arm the "miss" population is mostly vLLM-warm.
- Requests with no backend id are excluded from the JSON denominator but land in the Locust
  `TTFT (Cache MISS)` bucket (`:4192`, `is_cache_hit` defaults False) — the two views disagree.
- **Fix:** rename to "route consistency %" everywhere it is printed; derive a real hit rate
  from vLLM `vllm:prefix_cache_hits/queries` deltas over the run, or from the usage block's
  `cached_tokens` where the engine emits it.

### 2. Request counts and req/s are Locust event counts, ~6× real (2× in CI) — Sev 7, CONFIRMED
- **Status:** FIXED 2026-09-30. Both locustfiles log derived rows via `record_derived_sample` (direct `StatsEntry.log`, never `events.request`), so the Aggregated row counts HTTP requests only. `benchmark-baseline.json` is marked stale and must be regenerated.
- `locustfile_real.py:4171-4235`: each success fires POST + `TTFT (Time To First Token)` +
  `TTFT (Cache X)` + `TTFT (<bucket>)` + `TTFT (<bucket> status)` + `Tokens/Second` (a rate
  stuffed into a ms field). Locust logs all of them into the `Aggregated` row.
- `results_parser.py:504-520` reads `total_requests`, `failed_requests`, `failure_rate_pct`,
  `avg_response_time_ms`, `requests_per_sec` from that row (JSON fallback only when the count
  is 0). The "~req/s" column and "Throughput +1–7%" in
  `docs/benchmarks/benchmark-results-current.md:38-56` inherit this. 20 users with 0.5–2 s
  wait cannot produce 47 HTTP req/s; ~8 is plausible.
- CI: `locustfile.py:719-736` fires POST + TTFT, so `benchmark-baseline.json`
  `throughput_rps: 513.58` ≈ 257 real, `total_requests: 30336` ≈ 15k, and
  `failure_rate_percent` (failures only on POST) is understated 2× — the 1% gate is really 2%.
- Arm-to-arm *ratios* survive; absolute rates and the "crossover band" do not.
- **Fix:** record synthetic samples via a custom stats collector or a distinct `request_type`
  the parser filters out; take request/RPS/failure figures from `BENCHMARK_STATS_JSON` or the
  POST rows only.

### 3. Headline P99 TTFT is Locust's *approximated* percentile, reported to 0.1% — Sev 6, CONFIRMED
- `results_parser.py:221-240` regexes the "Response time percentiles (approximated)" table.
  Locust 2.24 `stats.py:398-404` rounds logged times: <1 s to 10 ms, 1–10 s to **100 ms**,
  ≥10 s to 1 s; percentiles are nearest-rank over rounded keys. 13B P99 TTFT sits in 1–10 s,
  so each value carries ±50 ms (≈±2–5%), the same order as the reported IQR widths.
- The locustfile computes exact linear-interpolated percentiles (`:2754-2769`) for hit/miss and
  size buckets but emits **no overall** `ttft_p50/p99_ms` in `BENCHMARK_STATS_JSON`, so
  `aggregate --metric p99_ttft_ms` (`results_parser.py:2211`) only ever sees the coarse number.
- **Fix:** emit overall raw-sample p50/p95/p99 in the JSON; make `aggregate` prefer it; state
  the bucket resolution wherever the Locust-table number is shown.

### 4. "Reliable" verdict at n=3 is not statistically supported — Sev 5, CONFIRMED
- `results_parser.py:1773-1783` `_quartiles` uses `statistics.quantiles(method="inclusive")`;
  with n=3, Q1 = mean(s0,s1), Q3 = mean(s1,s2). Verdict (`:1883-1892`) is "reliable
  IMPROVEMENT" whenever Q3 < 0. Reproduced: pairs (−30%, −20%, **+15%**) → Q3 = −2.5 →
  `reliable=True`.
- `_fmt_stat` (`:1909-1913`) prints `median X [min..max], IQR <width>` and never a Q1…Q3
  range, yet `benchmark-results-current.md:40-43` prints "IQR −15.6…−8.5". Either min/max was
  copied and mislabelled, or the numbers came from the JSON by hand. Cannot tell without the
  report dirs (not in repo).
- Pairs where `_pct_change` returns None (baseline 0) are dropped from `deltas` while
  `n_pairs` still reports the full count (`:1802-1806`).
- **Fix:** label the verdict "consistent direction in all repeats" and require every pair to
  share the sign; print Q1…Q3 explicitly if the docs are going to quote it.

### 5. SQLite routing state lives on the host, is shared by all three nodes, and is never cleared — Sev 7, CONFIRMED
- **Status:** FIXED 2026-09-30. `docker-compose.benchmark-real.yml` mounts a per-node tmpfs at `/var/lib/ranvier` and points `RANVIER_DB_PATH` there; container removal (per arm in `bench.sh`) discards it. The `/tmp` bind mount remains for core dumps only.
- `docker-compose.benchmark-real.yml:141,233,313` bind-mount host `/tmp` into every node
  ("for core dumps") and `:152,242,322` set `RANVIER_DB_PATH=/tmp/ranvier.db`. `bench.sh`
  `restart_ranvier_with_mode` (`:2003-2016`, `stop` + `rm -f` + `up -d`) and `cleanup`
  (`down -v`) remove containers and named volumes, never a bind-mounted host file. No script
  in `scripts/` or `tests/integration/` references `ranvier.db`.
- `src/application.cpp:522-618` `load_persisted_state()` re-registers every persisted backend
  and replays every persisted route via `learn_route_global` at startup, in every mode.
- Consequences: three processes write one `routes` table with no `busy_timeout`
  (`sqlite_persistence.cpp:34-35`); every prefix-mode arm starts with the ART pre-populated
  from the last prefix run on that host (for the seeded churn workload the prefix bytes are
  identical run to run, so ~200 "hits" point at freshly cold-started vLLMs); stale backends
  from a different topology are re-registered before Locust registers the real ones.
- `benchmark-tooling-review-2026-07-05.md:90-91` ("removed between arms — clears
  `/tmp/ranvier.db`") and the checklist's belief that this was handled are both wrong.
- **Fix:** per-node DB path on a container-local (non-bind) path, or `rm -f /tmp/ranvier.db*`
  at script start and in `restart_ranvier_with_mode`, or run benchmarks with persistence
  disabled.

### 6. Prometheus latency breakdown reports a single-shard mean as both P50 and P99 — Sev 8 for those columns, CONFIRMED
- **Status:** FIXED 2026-09-30. Shared `tests/integration/prom_scrape.py` tolerates any label block, sums buckets per `le` and counters across shards, takes max for gauges; unit-tested in `test_prom_scrape.py`. Both locustfiles delegate to it.
- `locustfile_real.py:2858-2860` (and `locustfile.py:167-169`) regex:
  `{metric}_bucket\{le="X"\}` — requires `}` right after `le`. Seastar emits every series
  with a `shard="N"` label and sorts labels, so lines read `_bucket{le="0.001",shard="0"} N`
  (`results_parser.py:569-575` documents this; `test_metrics.py:466-479` locks it in). The
  regex never matches; `get_histogram_percentile` returns None.
- Fallback `get_histogram_avg` (`:2810-2837`) substring-matches `_sum`/`_count` and
  *overwrites* in the loop → mean of the **last shard only** (8 shards under `--cpuset 0-7`).
  `get_ranvier_latency_breakdown` (`:3053-3063`) assigns that to both `*_p50_ms` and
  `*_p99_ms`. The "Routing Decision / Tokenization / Backend Connect P50 P99" table
  (`:3855-3910`), the JSON fields, and `results_parser.py:823-834` all carry it; the two
  columns will always be identical. "ART lookup (derived)" (`:3139-3145`) is mean − mean.
- Even with the regex fixed, appending all shards' buckets into one list and sorting by bound
  (`:2878-2885`) interleaves 8 cumulative series; buckets must be summed per `le` first.
- Same class: `get_metric_value` (`:2790-2807`) returns the first matching line → shard 0
  only. "Prefix Boundary Used/Skipped" (`:3170-3171`) are ~1/8 of the true count.
- **Fix:** match `\{[^}]*le="([^"]+)"[^}]*\}`, sum per `le` across shards and nodes, then
  interpolate; sum counters across shards (the parser already does this correctly).

### 7. `--load-imbalance-factor/--floor` have no effect under the default strategy, and nothing records which strategy ran — Sev 8 for any "raised thresholds" claim, CONFIRMED
- Plumbing is fine (`bench.sh:1417-1424` → compose `:168-169` → `config_loader.cpp:202-207`),
  but `src/config_schema.hpp:80` defaults `hash_strategy = BOUNDED_LOAD`, and
  `router_service.cpp:1311-1346` `compute_load_allowance` uses factor/floor only for
  `JUMP/MODULAR`; `BOUNDED_LOAD` uses `bounded_load_epsilon`. The server's own startup log
  says so (`:1615-1627`).
- Neither the "Effective Routing Config" banner (`bench.sh:1437-1446`) nor `manifest.json`
  (`:1577-1625`) prints `RANVIER_HASH_STRATEGY` or `RANVIER_BOUNDED_LOAD_EPSILON`; a run
  labelled "factor 3.0 / floor 4" is indistinguishable from a default run in the artefacts the
  parser compares. Only the raw `env | grep RANVIER_` dump in `run_*.log` (`:845`) shows it.
- `next-benchmark-checklist.md:104-107` ("shipped defaults never tested at 50 prefixes; D2
  used raised thresholds") is wrong if bounded_load was already the default when D2 ran
  (SUSPECTED — shallow clone cannot date the default).
- Same gap for `RANVIER_CROSS_SHARD_LOAD_SYNC`, `RANVIER_MIN_TOKEN_LENGTH`,
  `RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS`, `RANVIER_ENABLE_MULTI_DEPTH_ROUTING`,
  `RANVIER_DEFAULT_COMPRESSION_RATIO`, `RANVIER_BACKPRESSURE_*` (compose `:151-187` passes
  them through from the host env unrecorded).
- **Fix:** banner and manifest print every `RANVIER_*` the compose forwards; `bench.sh` warns
  or refuses when `--load-imbalance-*` is passed under `bounded_load`/`p2c`.

---

## Tier 2 — The CI regression gate

### 8. CI "P99" is the 98th percentile — Sev 7, CONFIRMED
- Locust 2.24 `PERCENTILES_TO_REPORT = [0.50, 0.66, 0.75, 0.80, 0.90, 0.95, 0.98, 0.99, …]`
  → CSV columns 12–22; column 18 = 98%, 19 = 99%. `run-benchmark.sh:122` documents the
  columns *without* the 98% column and `:132` sets `p99 = $18`; `bisect-benchmark.sh:83-84`
  likewise. `benchmark-baseline.json:26` `p99_latency_ms: 59.00` is therefore a P98 (generated
  by the same script, so baseline vs current are at least consistent). The console-table path
  in `results_parser.py:232-240` maps the columns correctly.
- Combined with finding 2, the gate is roughly the POST **P96** of a TTFT + total-time union.

### 9. Mock backend has a hard-coded 40 ms floor and no cache model — Sev 8, CONFIRMED (floor) / SUSPECTED (share)
- `tests/integration/mock_backend.py:548`: `latency_s = latency_ms/1000 if latency_ms > 0
  else 0.01` → 10 ms sleep after each of 4 canned chunks = 40 ms per request even when the
  knob is "off". Latency is identical regardless of backend, prefix or cache state, so prefix
  routing is neither rewarded nor penalised.
- The 10% budget on a 59 ms baseline is 5.9 ms, roughly Ranvier's entire share; a 2× regression
  in Ranvier's own path can pass while runner jitter can trip it. The baseline note "partial
  tokenization: P99 85→59 ms (−30%)" cannot be attributed in this setup.
- **Fix:** default the mock delay to 0 for overhead benchmarks (or model it explicitly and say
  so); gate on the TTFT row, not the mixed Aggregated row.

### 10. Missing or zero baseline key → silent PASS — Sev 6, CONFIRMED (reproduced)
- `run-benchmark.sh:257-262` `jq -r` prints `null` for a missing key; `:283` `bc` prints a
  syntax error but exits 0 so `set -e` does not fire and `P99_DELTA=""`; `:301`
  `echo " > 10" | bc` → empty; `[[ "" -eq 1 ]]` is false → PASS. Same path for a baseline
  `p99_latency_ms: 0.00` ("Divide by zero", exit 0). `generate_baseline` (`:168-204`) can
  emit exactly that `0.00` from an unparsable row, and hard-codes `duration_seconds: 60`.
- `p99_regression_percent`/`throughput_regression_percent` in the baseline JSON are decorative;
  thresholds come only from CLI flags / workflow env (`:23-24`, `benchmark.yml:60-61`).
- **Fix:** validate parsed numbers non-empty and > 0 before comparing; fail closed.

### 11. `make benchmark` and `bisect-benchmark.sh` never start ranvier2/ranvier3 — Sev 7 (bisect), CONFIRMED
- `docker-compose.test.yml` gates ranvier2/3 behind `profiles: [full]`; `locust`
  `depends_on` both with `service_healthy`. `Makefile:499` `COMPOSE_ARGS` and the `benchmark`
  target (`:572-584`) pass only `--profile benchmark`; `bisect-benchmark.sh:41,54` likewise.
  `benchmark.yml:141-148,184` passes `--profile full` and its comment describes exactly this.
- Compose does not auto-enable dependency profiles, so these paths should fail to start; if
  they ever ran, 2/3 of requests would fail fast and *lower* the aggregated "p99" → GOOD.
- Bisect also flips metric mid-run: with `set -euo pipefail` a Locust exit 1 (its own TTFT
  P99 > 100 ms check, `locustfile.py:638-639`) aborts before extraction and counts as "bad", so
  a commit is judged on TTFT-P99 if that fires, otherwise on aggregated P98. Single 60 s run,
  no repeats.

### 12. `bench.sh` swallows Locust's exit status; a crashed or mismatched arm is recorded as "pass" — Sev 6, CONFIRMED
- `bench.sh:31` `set -e` with no `pipefail`; `:1829` `… 2>&1 | tee … > /dev/null` returns
  tee's 0. Import failure, registration failure, or Locust's `process_exit_code = 1`
  (`locustfile_real.py:3968`) all return 0; `bench-runner.sh:701-703` records `pass` and feeds
  the dir into `--repeat` aggregation (`:960`).
- `verify_routing_mode_matches()` (`locustfile_real.py:546-596`) only logs
  "ROUTING MODE MISMATCH"; nothing greps for it.
- Single-arm runs hard-code the label: `bench.sh:2132` calls `run_benchmark "prefix"`, which
  names the dir, writes `"mode": "prefix"` to the manifest and passes `BENCHMARK_MODE=prefix`
  to Locust, while the server keeps whatever `RANVIER_ROUTING_MODE` the host env had
  (`:1536`). `RANVIER_ROUTING_MODE=hash ./bench.sh` yields artefacts labelled prefix.
- **Fix:** `set -o pipefail`; fail the arm on non-zero Locust exit, on "ROUTING MODE MISMATCH",
  or on missing `BENCHMARK_STATS_JSON`; derive the single-arm label from `X-Routing-Mode`.

---

## Tier 3 — Measurement definitions

### 13. TTFT definition and censoring — Sev 5, CONFIRMED
- Timer starts (`locustfile_real.py:4053`) before `requests.post` opens a **fresh TCP
  connection** (no `Session`; `HttpUser.client` unused) and serialises a 30–40 KB body; it stops
  on the first line starting with `data: ` (`:4131-4136`), which on `/v1/chat/completions`
  is the role-only delta. Identical in both arms, so A/B deltas survive; absolute TTFT and the
  `CLIENT_TOKENIZE` on/off comparison do not (client tokenization runs *outside* the timer,
  server tokenization *inside* it).
- Ranvier SSE error frames (`src/http_controller.cpp:583-597` `write_client_error` on an
  already-200 stream; `:2310`, `:2486-2496`) start with `data: `, so a backend failure or
  "Request timed out" frame is recorded as a **success with a valid TTFT** and
  `completion_tokens=0` (`:4161-4177`), pulling P50 down or P99 up while `failed_requests`
  stays flat.
- Requests that die before the first chunk are dropped from all TTFT lists (`:4237`), capping
  the TTFT tail at the 120 s read timeout; a stream that times out *after* the first chunk had
  a measured TTFT that is discarded (`metrics.ttft_ms` only set on the success path, `:4160`)
  and is mis-counted as "incomplete before TTFT". Failed POSTs *are* in Locust's POST
  percentiles — the two tables are on different populations.
- `results_parser.py:53-56` `TTFT_FLOOR_MS = 100` nulls any bucket TTFT below 100 ms as an
  artifact, while `run_benchmark_comparison.py:41,442` states "cache hits: 50–100 ms TTFT" as
  the *expected* result. On an 8B model a genuine cached hit can sit below the floor; the guard
  can erase exactly the effect being measured.
- **Fix:** stop the timer on the first chunk with non-empty content; treat a first `data:`
  frame containing `"error"` as a failure; keep the TTFT sample when it was measured; use a
  keep-alive session or document the cold-connect cost; drop or lower the floor.

### 14. Stress-mode prefix pool is generated from the unseeded global RNG — Sev 5, CONFIRMED
- `locustfile_real.py:2449-2471` draws `random.randint(LARGE_PREFIX_MIN, MAX)` per prefix and
  `random.shuffle`s RAG chunks (`:2401-2402`); only the churn universe seeds (`:2516`). Each
  Locust process (warm-up, RR arm, prefix arm) builds a different pool: different size-bucket
  membership, different total prefill volume, and warm-up primes prefixes whose bytes do not
  match the main run's — contradicting `bench.sh:2076-2079` "identically primed".
- **Fix:** seed a `random.Random(SEED)` for the pool exactly as churn does and forward the seed.

### 15. Counters are cumulative and include warm-up; percent denominators are main-run only — Sev 3, CONFIRMED
- `bench.sh:1856-1880` scrapes once after Locust exits (correctly after `--stop-timeout`), never
  differences against a start snapshot. `--warmup` runs after the per-arm restart (`:2085`), so
  `load_aware_fallbacks_total`, `residency_route_downgrades_total` and per-backend `routed_total`
  include warm-up traffic while `results_parser.py:1625` divides by main-run requests.
- **Fix:** snapshot before and after the main run and diff (the locustfile already does this for
  sync-error counters, `:3230-3270`).

### 16. Knobs and labels that do not mean what they say — Sev 3–4, CONFIRMED
- `SHARED_PREFIX_RATIO` only governs the 20% "medium" slice of the default `stress`
  distribution (`locustfile_real.py:3476-3528`): 70% large/xlarge are always shared, 10% short
  never are. Effective shared fraction ≈ 0.70 + 0.2·ratio; the manifest prints the knob as if it
  applied.
- `tokens_per_second` (`:2688-2690`) = completion tokens ÷ **sum** of per-request total time
  (which includes TTFT) — a per-stream mean, ≈ users× smaller than cluster throughput; the
  parser prints it under "Throughput:".
- "Round-robin" baseline is weighted-random through Ranvier in RANDOM mode
  (`config_loader.cpp:167-175` → `router_service.cpp:2596-2607`), which also skips tokenization
  (`http_controller.cpp:1377-1384`). Fine for isolating routing policy; wrong for any "Ranvier
  adds only X ms" claim, and there is no client-direct arm.
- Prefix-size buckets: large prefixes are sized by `estimate_tokens(prefix_text)` (`:2461`),
  others by `estimate_tokens(str(messages))` (`:3369-3377`, Python dict repr inflates it);
  real `prompt_tokens` from the usage block are available but unused.
- `results_parser.py:1703` prints cache-hit change as `(+{x:.1f}%)` — always "+", and it is
  percentage points. `run_benchmark_comparison.py:273-281` calls any non-identical P99
  "improved/regressed"; `:408-420` averages large and xlarge deltas unweighted and labels ≥50%
  "SIGNIFICANT"; `:441-444` "50–90% (3–10× faster)" — 50% is 2×.

### 17. Micro-benchmarks — Sev 4–7
- `tests/bench/hot_prefix_bench.cpp:86-87`: `acc ^= hash_prefix(tokens.data(), size, 1)` over
  loop-invariant memory with no stores in the loop; Release LICM may hoist the hash, leaving
  ns/op near zero. SUSPECTED. Fix: an `asm volatile` memory barrier per iteration or mutate one
  token per iteration. Clock/warm-up/iteration count are fine; single run, no variance.
- `epp_microbench.py:51,66-82`: same prompt for warm-up and every timed call → every decision is
  a warm-tokenizer ART hit; timed region includes per-call stub construction and Python protobuf
  work. Output does not say so. Envoy is correctly excluded and stated.
- `bench-inline-vs-sidecar.sh`: C−B (ext_proc overhead) is sound. Mock ignores `stream: false`
  and always streams with 4×10 ms sleeps (40 ms floor under every arm); mock replies
  `Connection: close` so each proxy reconnects per request — cancels in C−B, not in B−A/C−A.
- KV-event harness (`benchmarks/cache_event_generator/harness.py`) models a per-prefix LRU, but
  `src/router_service.cpp:5386-5397` wipes **all** routes of the backend on one `evicted`
  event and counts `evictions_applied` once. The "push" leg therefore measures "drop every
  route for the backend, then hash-route", not per-prefix eviction. Harness counts
  `events_posted += len(batch)` regardless of POST success (`:301-317`); "staleness window" is
  POST round-trip from batch start, not emission-to-apply (`:186-199`). Its delta bookkeeping,
  per-leg restart with persistence disabled, and FNV-1a replica are correct.

### 18. Latent — Sev 2–3
- Distributed Locust would zero the whole custom summary: `_benchmark_stats` is process-local,
  `on_test_stop` returns early for workers (`locustfile_real.py:3733`), no
  `report_to_master`/`worker_report` hooks; `hash()`-based prefix keys differ per process.
- `locustfile.py:392` `agents_detected` requires `"{" not in key`; every Seastar series has a
  label block, so it is structurally 0. Baseline `priority_distribution`/`intent_distribution`
  are all 0 although the mock workload rotates User-Agents and intents; "expected on mock
  backends" (`benchmark-baseline.json:52`) is not a valid explanation for header-derived
  counters. SUSPECTED scrape bug.
- `benchmark.yml:207-208` stores `locust_exit_code` and nothing reads it; the "Comment on PR"
  step (`:281`) is dead (no `pull_request` trigger). The `regression_detected` fail-closed path
  (`:274-278`) is correct.
- `bench.sh:407` stale-container preflight filters `name=ranvier-benchmark`; containers are
  `ranvier-bench1..3`. Dead code.
- Warm-up uses the main run's `SPAWN_RATE` with users forced to 2 (`bench.sh:1905-1908`). Harmless.

---

## Documentation carrying superseded or unsupported figures

- `README.md:58-63` "Performance Characteristics" table: "Cache Hit Rate 58–98%", "Ranvier P50
  Overhead ~7 ms", "Radix Tree Lookup <50 μs" — all from the 5-prefix campaign the same README
  calls not citable, in a section with no caveat. The `<50 μs` figure depends on
  `hot_prefix_bench` (finding 17).
- `docs/guides/benchmark-reproduction.md:72-73,86-87`: "Ranvier targets −60% to −85% vs
  baseline", "Expect +4–22%" — presented as what a reproducer should expect.
- `docs/benchmarks/kv-cache-prefix-routing-benchmark.md:69-74,275-285`: "58–98%", "−60% to
  −85%", "P99 −78% to −85%" with no superseded banner.
- `docs/benchmarks/benchmark-methodology.md:770-792`: a full table of dated Jan/Feb 2026 run
  results and "P99 −79% to −85% (valid)" in the document that says it "is kept free of dated
  run results on purpose".
- `docs/benchmarks/benchmark-results-current.md`: "cache-hit rate" (finding 1), "req/s"
  (finding 2), "IQR a…b" (finding 4), "reliable" (finding 4).
- `CHANGELOG.md:397,471-472`: historical; acceptable if left as release-note history.
- `locustfile_real.py:40-41,79-82` docstring defaults: `mixed` / 0.7 / 5; code is `stress` /
  0.9 / 50.
- `.dev-context/claude-locust-sync-map.md` ("last verified 2026-02-20"): metric names all still
  match `src/`; but it does not mention the `shard` label, which is the coupling that actually
  breaks metric reads (finding 6).

---

## Checked and found correct

- Sign conventions: `format_change` (`results_parser.py:1208-1272`) and `_pct_change` divide by
  the baseline; negative on lower-is-better = BETTER; −13.3% is a real decrease in the prefix arm.
- Aggregation is per-metric median across repeats, and the A/B verdict is the median of
  per-pair deltas aligned by repeat index (`bench-runner.sh:929-947`), not a "median run".
- `results_parser.py` console-table column mapping (50/75/90/95/99 → groups 1/3/5/6/8) is right
  for Locust 2.24; the CSV path is the one that is off by one (finding 8).
- Per-arm parameters are identical (duration, users, spawn rate, stop timeout, max tokens,
  model, distribution, ratio, client timeouts). No per-arm timeout asymmetry.
- Ranvier containers are removed between arms and each arm's Prometheus counters start at zero
  (subject to warm-up inclusion). Arm order is configurable and alternated across repeats.
- Three-node scrape to `prometheus_metrics_node{1..3}.txt`; the parser sums counters across
  shards and nodes, takes max for gauges, records `nodes_scraped`. Gini is the standard formula.
- `manifest.json` captures commit, argv, GPU, vLLM version and workload knobs; the parser warns
  on workload mismatch.
- Clocks: `perf_counter` throughout the Python request path; `steady_clock` in C++; seconds→ms
  conversions consistent.
- `--no-load-aware`, `--cache-residency-threshold`, `--multi-depth`, `--compression-ratio`,
  `--priority-queue`, `NUM_LARGE_PREFIXES`, churn knobs all reach the intended process under
  names the C++ reads. No env-var name drift.
- Prometheus metric names in both locustfiles and the parser exist in `src/` under
  `add_group("ranvier")`; `X-Backend-ID` / `X-Routing-Mode` headers are set on every proxied
  reply; `round_robin` → `random` alias mirrors `config_loader.cpp`.
- Locustfile `_percentile` is correct linear interpolation; FNV-1a/jump-hash constants and byte
  layout match the sync map.
- Incomplete and failed requests are excluded from TTFT percentiles and the hit denominator,
  and the compare output reports incomplete rate prominently.
- Deprecated scripts (`run-multi-gpu-benchmark.sh`, `setup-lambda-benchmark.sh`) are hard
  `exit 1` stubs.

---

## What this means for the July 2026 headline

- **Direction survives.** Same code path in both arms, paired by repeat, alternated order, sign
  conventions right. Prefix routing did lower P99 TTFT at 8B/20u and 13B/30u and raise it at
  13B/10u in those runs.
- **Precision does not.** ±50 ms quantization on 1–10 s values, n=3 "reliable" that tolerates
  one contradicting repeat, and unseeded per-arm workloads mean the −13.3%/−9.1%/+29% figures
  should be quoted as approximate medians with sign agreement stated, not to 0.1%.
- **"3× cache-hit rate" is not a cache-hit measurement.** It is route consistency, and its
  round-robin denominator is 1/N by construction.
- **Every req/s figure is ~6× high.** The "crossover between ~16 and ~28 req/s" is really
  roughly 3–5 HTTP req/s.
- **Arm isolation was weaker than believed.** The routing DB persisted across every arm, leg and
  repeat on the host.

## Fix order (highest value per line of change)

1. Delete or relocate the routing DB before every cluster start (finding 5).
2. Stop feeding synthetic samples into Locust's Aggregated row; take counts from the JSON
   (finding 2). Same change fixes CI's 2× inflation.
3. Fix the histogram/counter regexes to tolerate and sum over `shard` (finding 6).
4. Emit overall raw-sample TTFT percentiles in the JSON and aggregate on them (finding 3).
5. Rename "cache hit rate" to route consistency and add a real hit rate from vLLM counters
   (finding 1).
6. Banner and manifest print hash strategy and epsilon; warn on ineffective flags (finding 7).
7. `set -o pipefail` and fail arms on Locust non-zero / mode mismatch (finding 12).
8. CI: read column 19, gate on the POST row, zero the mock floor, fail closed on empty numbers,
   add `--profile full` to `make benchmark` and bisect (findings 8–11).
9. Seed the stress prefix pool (finding 14); snapshot counters before/after (finding 15).
10. Relabel the n=3 verdict and print Q1…Q3 (finding 4); purge superseded figures from README,
    reproduction guide and methodology doc.
