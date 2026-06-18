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

## Complementary release gate (end-to-end A/B)

This microbench gives the actual ns cost. The **release gate** — "is it invisible
under load?" — is the documented telemetry-off-vs-on A/B in BACKLOG §21
("Verification & benchmark gate"): drive identical single-stream load through a
node with `RANVIER_TELEMETRY_SINK_ENABLED` flipped, compare
`router_routing_latency_seconds` / client p50·p99. Expect *no measurable
difference* — the tax this microbench quantifies sits below the end-to-end noise
floor. That A/B requires prefix routing to be active (tokenizer loaded, `mode:
prefix`) so the ON arm exercises `touch()` + the hit-path hash.
