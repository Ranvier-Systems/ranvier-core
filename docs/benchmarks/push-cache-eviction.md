# Push Cache Eviction Benchmark (Phase 4)

A/B benchmark that quantifies prefill-savings when Ranvier receives
push notifications of backend KV-cache evictions vs. when it falls
back to TTL-based inferred routing.

This benchmark is **synthetic**: it models a fake inference backend's
KV cache as an LRU of prefix hashes and exercises Ranvier's existing
`POST /v1/cache/events` endpoint with the v1 JSON schema. It does not
use vLLM or SGLang. The absolute numbers are indicative — they
measure routing-layer behavior, not GPU prefill latency.

The corresponding real-sidecar work (Phase 3b) lives in a separate
repository and is decoupled from this harness on purpose so both can
land in parallel.

## Layout

```
benchmarks/cache_event_generator/
├── generator.py   # synthetic LRU + event POST tool, --mode {push,silent}
├── harness.py     # drives M requests through Ranvier and writes a result file
├── run.sh         # per-mode runner: starts N mock backends, registers, runs harness
└── run-ab.sh      # full A/B wrapper: manages the Ranvier lifecycle for both legs
```

The fake inference backend is `tests/integration/mock_backend.py`,
reused as-is. `run.sh` spawns `NUM_BACKENDS` instances on consecutive
ports starting at 9001.

## Prerequisites

The benchmark assumes Ranvier is already running locally and that the
Python 3 stdlib is available. No external Python packages.

**Run `run.sh` in the same network namespace as Ranvier** — i.e., on
the same host, or inside the same Docker container. Ranvier's TCP
health check resolves `127.0.0.1:$BACKEND_PORT` inside *its* namespace,
so if Ranvier is in a container and you run `run.sh` on the host, the
mock backend is unreachable from Ranvier and the backend gets
quarantined.

Key facts about Ranvier's network layout (these drive the defaults):

- `:8080` — API/admin/proxy/cache-events (all on the same port)
- `:9180` — Prometheus `/metrics` (separate port)
- Backends are registered via `POST /admin/backends?id=…&ip=…&port=…`
  (NOT via config file).
- `/v1/cache/events` is only registered when `cache_events.enabled=true`,
  which is OFF by default and is NOT toggled by `--local`.

## Running the A/B

### One-command path (recommended)

`run-ab.sh` orchestrates both legs: starts Ranvier with the right
env for each leg, waits for `/health`, runs `run.sh`, stops Ranvier
cleanly, and writes everything — wrapper stdout, both Ranvier logs,
both result files — into a timestamped directory under `results/`.

```bash
./benchmarks/cache_event_generator/run-ab.sh
```

Override the Ranvier binary path and args with env vars:

```bash
RANVIER_BIN=./build/release/ranvier_server \
  RANVIER_ARGS='--config ranvier.yaml' \
  ./benchmarks/cache_event_generator/run-ab.sh
```

Defaults: `RANVIER_BIN=./build/ranvier_server`, `RANVIER_ARGS=--local`.
All `run.sh` env overrides (`REQUESTS`, `CAPACITY`, `NUM_BACKENDS`,
etc.) work here too.

Output layout (one run):

```
results/ab-20260408_001234/
├── run.log              # full wrapper stdout (commands, headers, run.sh output)
├── ranvier-silent.log   # Ranvier stdout/stderr for the baseline leg
├── ranvier-push.log     # Ranvier stdout/stderr for the push leg
├── mock-backend.log     # mock backend logs (both legs append here)
├── result-silent.json
└── result-push.json
```

### Manual path (interactive iteration)

If you want Ranvier to stay up across multiple harness runs — e.g.
while tuning capacity or request counts — skip `run-ab.sh` and drive
`run.sh` directly. Note both legs need `RANVIER_ACCEPT_CLIENT_TOKENS=1`
because the harness always sends `prompt_token_ids` in the request
body so it can compute the exact same hash Ranvier stores:

```bash
# Leg 1 — baseline (cache_events disabled, the default):
RANVIER_ACCEPT_CLIENT_TOKENS=1 ./build/ranvier_server --local
MODE=silent ./benchmarks/cache_event_generator/run.sh

# Leg 2 — push (restart Ranvier with cache_events enabled):
RANVIER_ACCEPT_CLIENT_TOKENS=1 RANVIER_CACHE_EVENTS_ENABLED=1 \
  ./build/ranvier_server --local
MODE=push   ./benchmarks/cache_event_generator/run.sh
```

### Why pre-tokenized requests

The harness sends `prompt_token_ids` (vLLM Online Serving schema)
instead of plain `messages.content`. This is what makes the
push-event accounting actually work: the harness needs to send
eviction events keyed on the same `prefix_hash` Ranvier stores in
its reverse index, and that hash is `FNV-1a(token_bytes)` over the
token sequence Ranvier sees. By passing pre-tokenized IDs and
reimplementing `hash_prefix()` in Python (see
`generator.py::ranvier_hash_prefix`), both sides converge on the
exact same 64-bit hash by construction. Otherwise eviction events
would land in `evictions_unknown` because Ranvier's tokenizer would
hash a different byte stream than the harness expects.

`run.sh` handles everything on the benchmark side:

1. Starts `tests/integration/mock_backend.py` on port 9001
2. Waits for `/health` to respond
3. Registers the backend with Ranvier via `POST /admin/backends`
4. Runs `harness.py` in the requested mode
5. Deregisters the backend and kills the mock on exit

Environment overrides (all optional): `REQUESTS`, `CAPACITY`,
`POPULATION`, `ZIPF_S`, `SEED`, `NUM_BACKENDS`, `BACKEND_PORT_BASE`,
`PROXY_URL`, `METRICS_URL`. Defaults are seeded for reproducibility
(`SEED=12648430`, `REQUESTS=10000`, `NUM_BACKENDS=2`).

Results land in `benchmarks/cache_event_generator/results/`:

```
results/
├── mock-backend.log
├── result-silent.json
└── result-push.json
```

## Output schema

Each result file:

```json
{
  "mode": "push",
  "total_requests": 10000,
  "cache_hits": 0,
  "wasted_prefills": 0,
  "wasted_ratio": 0.0,
  "unknown_backend_routes": 0,
  "evictions_modeled": 0,
  "events_posted": 0,
  "per_backend": {
    "1": { "requests": 0, "cache_hits": 0 },
    "2": { "requests": 0, "cache_hits": 0 }
  },
  "ranvier_metrics": {
    "received_total": 0,
    "evictions_applied": 0,
    "evictions_stale": 0,
    "evictions_unknown": 0
  },
  "staleness_ms": { "samples": 0, "p50": 0, "p95": 0, "p99": 0 },
  "config": { "...": "..." }
}
```

### Key metrics

- **`wasted_prefills`** — requests routed to a backend that (per the
  modeled LRU) had already evicted the prefix. The headline number.
- **`wasted_ratio`** — `wasted_prefills / total_requests`.
- **`staleness_ms.p99`** — in `push` mode: wall-clock round-trip from
  emitting an `evicted` event to Ranvier's `POST /v1/cache/events`
  reply. The reply is sent only after `evict_by_prefix_hash_global`
  has finished its cross-shard fanout (see
  `src/http_controller.cpp::handle_cache_events`), so this is a real
  "event applied to RadixTree" measurement, not just network RTT. In
  `silent` mode no events are posted, so the samples list is empty
  and the percentiles are 0; compare against the TTL value from
  `cache_events.stale_ttl_seconds` (or the routing TTL default).
- **`ranvier_metrics.*`** — scraped from `:9180/metrics` before and
  after the run. Useful for cross-checking that posted events were
  actually applied (`evictions_applied`) vs. ignored
  (`evictions_stale`, `evictions_unknown`).

  **Subtlety: `evictions_applied` counts tree state changes, not
  events that "found a route".** Ranvier's
  `evict_by_prefix_hash_local()` calls
  `remove_routes_by_backend(backend, origin)`, which wipes every
  route for that backend in one shot — see
  `src/router_service.cpp::evict_by_prefix_hash_local`. Within a
  single POST, the first event causes the wipe and increments
  `evictions_applied`; events 2..N in the same batch find an empty
  subtree, return 0 routes removed, and increment
  `evictions_unknown`. Net result with `--batch-size N`:
  approximately `evictions_applied ≈ events_posted / N`.

  The harness defaults to `--batch-size 1` so each event maps to
  one observable tree write and the cross-check is clean. Override
  with `BATCH_SIZE=10` (or whatever) to measure the realistic
  batched-sidecar regime, but read the metric accordingly: every
  batch contributes one `applied` and `(N-1)` `unknown`.

### Sizing the workload to actually see the signal

The push/silent A/B only shows a delta when the modeled LRUs
**actually evict**. If the number of unique prefixes hitting each
backend is smaller than `CAPACITY`, the LRU never fills, no events
are emitted, and push vs. silent are bit-identical by construction.

Rule of thumb: for `NUM_BACKENDS=N` backends each with `CAPACITY=C`
slots, you need roughly

> `misses_per_backend ≈ wasted_prefills / N  >  C`

to drive evictions. A few concrete presets:

| requests | num_backends | capacity | evicts? |
|----------|--------------|----------|---------|
| 1000     | 2            | 512      | no (smoke test defaults under-pressure) |
| 1000     | 2            | 64       | yes, smoke-test friendly |
| 10000    | 2            | 512      | yes, full run |
| 10000    | 1            | 512      | yes, staleness-only (no wasted_prefill delta) |

The harness prints a loud `WARNING: evictions_modeled=0` to stderr
when the run never exercised the eviction path so this footgun
cannot bite silently.

### Measured shape (canonical reference run, 2026-04-08)

Single-shard local Ranvier (`./build/ranvier_server --local`),
defaults: `REQUESTS=10000`, `NUM_BACKENDS=2`, `CAPACITY=512`,
`POPULATION=4096`, `ZIPF_S=1.1`, `BATCH_SIZE=1`, `SEED=12648430`,
`hash_strategy=BOUNDED_LOAD`. Both legs run via
`./benchmarks/cache_event_generator/run-ab.sh`. Run took ~17 minutes
end-to-end (~8 min/leg, gated on `mock_backend.py`'s 50ms/request
streaming sleep, not on Ranvier).

**Push-leg result (the headline numbers for the Phase 4 RFC):**

| metric                        | value      |
|-------------------------------|------------|
| `total_requests`              | 10000      |
| `cache_hits`                  | 8098       |
| `wasted_prefills`             | 1902 (19.02%) |
| `evictions_modeled`           | 878        |
| `events_posted`               | 878        |
| `ranvier_metrics.received_total`     | 878 (100%) |
| `ranvier_metrics.evictions_applied`  | **731 (83.3%)** |
| `ranvier_metrics.evictions_unknown`  | 147 (16.7%) — see notes |
| `ranvier_metrics.evictions_stale`    | 0          |
| **`staleness_ms.p50`**        | **0.84 ms** |
| **`staleness_ms.p95`**        | **1.07 ms** |
| **`staleness_ms.p99`**        | **1.34 ms** |
| staleness samples             | 878 (full coverage, one per event) |

Per-backend traffic split (from the seeded Zipf draw + bounded-load
routing): backend 1 took 5917 requests / 5028 hits (85.0%), backend
2 took 4083 / 3070 (75.2%).

**Silent-leg result** (cache_events disabled, TTL fallback):
identical `wasted_prefills=1902` and identical per-backend split —
expected and explained in the next section.

### Headline finding

The **staleness window** between sidecar emit and Ranvier applying
the eviction across all shards is **0.84 ms p50 / 1.34 ms p99**
across 878 samples. The TTL-based default (`stale_ttl_seconds`) is
seconds-to-minutes depending on configuration. **That's the
~1000× improvement the Phase 4 RFC pitches, substantiated end to
end on real Ranvier code.**

The 16.7% `evictions_unknown` rate is not a benchmark bug — it's
the documented consequence of `remove_routes_by_backend()` wiping
*all* routes for a backend on the first event in a wipe cycle.
Subsequent events for the same backend with no intervening request
to re-populate the tree find an empty subtree and bump
`evictions_unknown`. See the metric description above for the full
walkthrough.

### What this benchmark does NOT measure (and why)

`wasted_prefills` is **identical between silent and push** in every
config we've tried with `hash_strategy=BOUNDED_LOAD` (the default).
This is not a bug — it's a property of Ranvier's routing algorithm:

1. On an ART hit, Ranvier routes to the learned backend.
2. On an ART miss (e.g., right after push removes a stale entry),
   Ranvier falls through to `bounded_load_select`, which computes
   `jump_consistent_hash(prefix_hash, N)` as the primary bucket.
3. The primary bucket is **the same backend** the ART had just
   pointed at — ART entries are learned from the original
   consistent-hash decision in the first place.
4. Under balanced load with 2 backends, the primary is always
   under the bounded-load cap, so no sequential probe kicks in.
5. Net: silent and push produce bit-identical routing decisions.

So under the default routing, push notifications provide:
- **Staleness reduction** (proven: ~1ms p99) — immediate value for
  any code path that reads `ranvier_router_cache_hits` or makes
  scheduling decisions from the RadixTree.
- **Telemetry accuracy** — Ranvier's `cache_hits`/`cache_misses`
  counters actually reflect backend cache state instead of
  reporting stale ART claims as hits.
- **Multi-shard propagation** (Phase 5, out of scope here).

Push notifications would show a `wasted_prefills` delta only with:
- `hash_strategy=P2C` (uses request_id salt → nondeterministic
  across identical prefixes), or
- Asymmetric backend load (triggers bounded-load sequential probe
  to the secondary), or
- Backend failures (changes `live_backends` composition).

None of those apply to the balanced, single-shard, 2-backend
default. Forcing any of them into the harness would measure the
hash strategy, not the push feature.

## Troubleshooting

**`curl: (7) Failed to connect to 127.0.0.1 port 8081`** — Ranvier
has no port 8081. Admin + proxy + cache events all live on 8080;
metrics lives on 9180. The harness defaults are already set to these.

**`No backends registered`** in Ranvier logs even after a successful
`POST /admin/backends`** — the mock backend isn't listening on the
registered port, so Ranvier's health check (TCP connect every 5s,
`failure_threshold=3`) marked it dead within ~15s. `run.sh` starts
the mock first and waits for `/health` before registering, so this
should not happen when you use the script. If you're registering
by hand, start `mock_backend.py` first.

**`POST /v1/cache/events` returns 404 in `push` mode** — you forgot
to set `RANVIER_CACHE_EVENTS_ENABLED=1`. The endpoint is only
registered when `cache_events.enabled` is true.

**Every event ends up in `evictions_unknown`** — either Ranvier
isn't running with `RANVIER_ACCEPT_CLIENT_TOKENS=1`, or the
harness's `tokens_for_prefix_id()` no longer matches Ranvier's
`hash_prefix()` (e.g. someone changed `block_alignment` in config).
Re-run with `run-ab.sh`, which sets the env var automatically and
keeps both sides in sync.

**`run.sh` exits silently with no output** — almost always means
Ranvier is unreachable at `$PROXY_URL`. `run.sh` now prints a
diagnostic before the curl, so you should see "Cannot reach Ranvier
at …" instead of an empty exit. If it happens anyway, check whether
Ranvier is running in a container while you're on the host (see the
note at the top of Prerequisites).

## Limitations

- The LRU model is a credible proxy but not a substitute for a real
  vLLM PagedAttention KV cache. Real backends evict at the page
  level, not the prefix level.
- The fake backend always returns 200 OK and does not actually run
  prefill, so we cannot measure end-to-end latency. We measure only
  routing-layer observables.
- Staleness is sampled once per POSTed batch (the HTTP RTT *includes*
  the apply time because `handle_cache_events` awaits the eviction
  before replying). The sample count equals the number of batches
  posted, which is `evictions / batch_size`.
- **`wasted_prefills` only has a silent/push delta with
  `NUM_BACKENDS>=2`.** With a single backend there is no routing
  choice, so the count is purely a function of that backend's local
  LRU trace, which is deterministic given `SEED`. The default is
  `NUM_BACKENDS=2`; run with `NUM_BACKENDS=1` only to sanity-check
  the staleness path.
- The harness trusts the `X-Backend-ID` header echoed by
  `mock_backend.py` to identify which backend Ranvier routed to. If
  the response is missing that header the request is counted under
  `unknown_backend_routes` and treated as wasted.
- This is single-shard, single-host. Multi-shard gossip propagation
  is out of scope for the benchmark — that's Phase 5.
