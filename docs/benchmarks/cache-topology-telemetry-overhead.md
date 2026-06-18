# Cache-Topology Telemetry Hot-Path Microbenchmark

Measures the **per-request CPU tax** that the cache-aware-autoscaler telemetry
(BACKLOG §21) adds to the routing path when enabled: one
`StreamSummary::touch()` plus, on the ART-hit path, one `hash_prefix()`. Reactor-
free, no Docker — the targeted complement to an end-to-end A/B, which can't
resolve a sub-microsecond delta buried under millisecond request latency.

## What it measures (and doesn't)

- **Measures:** `StreamSummary::touch()` in two regimes — *hit-heavy* (small hot
  set, ~all increments: the warm steady state) and *churn/evict* (distinct keys
  once full: the O(K) min-scan worst case) — and `hash_prefix()` across
  representative routing-prefix lengths (16–512 tokens). All in ns/op.
- **Does not measure:** end-to-end request latency, the cross-shard window merge
  (off the hot path, on shard 0), or the disabled-path cost (a single branch —
  telemetry off does no `touch()` and no hit-path hash).

## Running

```sh
make bench-hot-prefix      # Release build of just the bench, then run
```

`make bench-hot-prefix` builds the `hot_prefix_bench` target Release-mode and
runs it; it prints a ns/op table. Not a ctest and not part of `make test` — it's
a human/CI-trend number, with no pass/fail assertion (timing assertions are
flaky). Build constraint: not runnable in the static-analysis sandbox.

## Interpreting the numbers

- The **per-request tax when telemetry is ON** ≈ `touch (hit-heavy)` +
  `hash_prefix(prefix_len)` for the typical prefix length. Both are expected to
  be tens of nanoseconds (touch) to low microseconds (hash of a long prefix) —
  to be weighed against the **µs-scale ART lookup + tokenization** the routing
  path already does. If the tax is a small fraction of that, the §21 design
  premise (telemetry is "free enough" to leave on) holds.
- `churn/evict` is the pathological upper bound on `touch()` (every op evicts);
  real workloads sit far closer to `hit-heavy` once the hot set stabilizes.
- `hash_prefix` cost scales with prefix length (FNV-1a over `prefix_len * 4`
  bytes). It is newly paid on the hit path *only when tracking is on*; the miss
  path always paid it.

## Recorded run (illustrative — not canonical)

First reference run (Release). Reproduce on representative hardware before
quoting — this is a single dev-host sample, not a published figure.

| operation | ns/op |
|---|---|
| `StreamSummary::touch` (hit-heavy) | 2.32 |
| `StreamSummary::touch` (churn/evict) | 71.96 |
| `hash_prefix` (16 tokens) | 32.73 |
| `hash_prefix` (64 tokens) | 211.27 |
| `hash_prefix` (256 tokens) | 975.34 |
| `hash_prefix` (512 tokens) | 2014.80 |

**Takeaway.** `touch()` is negligible — 2.3 ns warm, 72 ns in the
pathological churn/evict case (the O(K=128) min-scan), confirming the design's
one flagged perf risk is a non-issue. The added tax is dominated by the
**hit-path `hash_prefix`**, which is **byte-bound** (~1 ns/byte). At the default
`prefix_token_length = 128` that interpolates to **~0.4 µs** per prefix-routed
hit when telemetry is ON (it was free on hits before; misses already paid it) —
a few percent of a warm routing decision, and **zero** when telemetry is off
(the gated path). Operators who raise `prefix_token_length` for long-context /
RAG workloads push this toward **~1–2 µs** (256–512 tokens), which is where it
starts to matter — see the follow-up below.

## Follow-up: bound the fingerprint hash (BACKLOG §21)

Because the cost is byte-bound and scales with `prefix_token_length`, the
telemetry-on hit-path tax grows for long-context deployments. Mitigation: hash
only a fixed-length prefix (e.g., the first 64 tokens) for the hot-prefix
**fingerprint**, making it constant ~200 ns regardless of `prefix_token_length`.
Semantically sound — two requests sharing the first 64 tokens *do* share that
warm cache, so a shorter fingerprint is arguably truer to "which prefixes are
hot."

Design wrinkle to resolve when implementing: the miss path currently reuses the
routing `prefix_hash`, which also feeds `jump_consistent_hash` (so that hash
can't be shortened). A capped fingerprint must therefore be a **separate** value
computed on both hit and miss — adding a cheap (~200 ns) capped hash to the miss
path in exchange for bounding the hit path. Net win for long-context, neutral at
the default. Deferred (not urgent: the tax is opt-in and small at the default).

## Complementary release gate (end-to-end A/B)

This microbench gives the actual ns cost. The **release gate** — "is it invisible
under load?" — is the documented telemetry-off-vs-on A/B in BACKLOG §21
("Verification & benchmark gate"): drive identical single-stream load through a
node with `RANVIER_TELEMETRY_SINK_ENABLED` flipped, compare
`router_routing_latency_seconds` / client p50·p99. Expect *no measurable
difference* — the tax this microbench quantifies sits below the end-to-end noise
floor. That A/B requires prefix routing to be active (tokenizer loaded, `mode:
prefix`) so the ON arm exercises `touch()` + the hit-path hash.
