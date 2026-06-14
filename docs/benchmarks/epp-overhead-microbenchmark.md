# GIE EPP Overhead Microbenchmark

Measures the **absolute per-request overhead of one ext_proc routing decision**
against a running Ranvier GIE Endpoint-Picker (EPP): the gRPC `Process`
round-trip plus Ranvier's routing-decision and `alien::submit_to` reactor
bridge. This is the "number not currently published anywhere" proof point from
BACKLOG §20.2 P1.4. It is deliberately **not** the full inline-vs-sidecar A/B
(see *Scope* below).

## What it measures (and doesn't)

- **Measures:** end-to-end latency, as seen by a local gRPC client, of a single
  `ExternalProcessor/Process` call that resolves to an endpoint (or 503). With a
  persistent channel and warmup, the timed quantity is dominated by Ranvier's
  decision path (tokenize → `route_request` → `get_backend_address`) plus the
  cross-thread bridge, not connection setup.
- **Does not measure:** the *delta* vs the inline data plane, or a real gateway's
  forwarding cost. The EPP picks an endpoint; it does not proxy. A full
  inline-vs-sidecar comparison needs a GIE-conformant gateway (Envoy + ext_proc)
  in front of real backends and is a separate, heavier benchmark.

## Prerequisites

- Docker (+ Compose) and a `WITH_GIE_EPP=ON` build (built on demand by
  `docker-compose.epp-test.yml`, which runs the `Dockerfile.gie-epp` builder
  stage + a mock backend on subnet 172.29/16).
- Python with `grpcio` + `grpcio-tools`
  (`pip install -r tests/integration/requirements.txt`).

No GPU/vLLM required — the mock backend only needs to be *registered* so the
picker has something to return; it is never actually called.

## Running

```sh
make bench-epp                              # 2000 timed reqs, prefix-aware (body)
make bench-epp BENCH_EPP_ARGS="--requests 5000 --warmup 500"
make bench-epp BENCH_EPP_ARGS="--no-body"   # headers-only (load/hash) path
```

`scripts/bench-epp-overhead.sh` brings up the EPP node, registers a backend,
runs `tests/integration/epp_microbench.py` against `localhost:9002`, and tears
the node down. The driver reports mean / p50 / p90 / p99 / min / max in
milliseconds, plus how many requests resolved to an endpoint.

## Interpreting the numbers

- Run **prefix-aware** (default, sends a tokenized body) and **`--no-body`**
  (headers-only load/hash) and compare: the gap is roughly the tokenize +
  ART/residency cost over bare load/hash selection.
- The absolute figure is the per-request tax a sidecar EPP deployment pays on
  top of the gateway's own forwarding. Weigh it against the prefix-cache TTFT
  wins (see `kv-cache-prefix-routing-benchmark.md` /
  `cache-residency-ab-benchmark.md`) when deciding inline vs. sidecar.
- This is a single-node, mock-backend, localhost measurement: it isolates
  Ranvier's decision overhead, not production network or gateway costs. Treat it
  as the floor, and see `interpreting-benchmark-numbers.md` for general caveats.

## Example run (dev baseline — illustrative, not canonical)

First end-to-end run, recorded as a sanity-check reference. **This is a floor,
not a published figure:** a single-shard Ranvier (`--smp 1`) plus the mock
backend in Docker on a macOS dev laptop, client over loopback
(`localhost:9002`). It isolates Ranvier's routing-decision + bridge overhead
with no production network, real backend, or gateway in the path — reproduce on
representative hardware before quoting numbers.

| mode | requests | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|---|
| prefix-aware (tokenized body) | 5000 | 0.475 | 0.523 | 0.578 | 0.711 | 2.897 |
| headers-only (load/hash)      | 2000 | 0.440 | 0.490 | 0.527 | 0.610 | 0.768 |

All times in milliseconds; `endpoint set` == request count with 0 errors in both
modes. A 2000-request prefix-aware run matched the 5000-request one (mean 0.474,
p99 0.755 ms), so the figures are stable across run sizes.

**Takeaway:** per-request EPP overhead is **sub-millisecond** (p50 ~0.5 ms, p99
~0.7 ms), and prefix-aware routing adds only **~35 µs at the median** (~0.1 ms at
p99) over bare load/hash — the tokenize + ART/residency lookup is cheap relative
to the ext_proc gRPC round-trip + reactor bridge, which dominate. For the
inline-vs-sidecar decision, the overhead to weigh is the indirection itself, not
Ranvier's routing intelligence.

## Scope / follow-ups

The full inline-vs-sidecar A/B (Envoy ext_proc gateway, real vLLM backends,
identical load through both paths) is the remaining published proof point and
needs cluster hardware. This microbenchmark is the tractable first number and
reuses the same `tests/integration/epp_client.py` the integration test uses.
