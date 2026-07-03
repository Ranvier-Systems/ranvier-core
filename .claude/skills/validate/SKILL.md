---
name: validate
description: Pick the right verification ladder for a change - unit tests, sanitizers, validation suite, integration tests, fuzzing, lint - and emit exact Deferred Gate commands. Use before merging, when the user asks "how do I verify this", or when preparing the developer's Docker test checklist.
argument-hint: [change-description]
---

## VERIFICATION PLAN

**Change under test:** $ARGUMENTS

> This sandbox is **static analysis only** — none of these commands run here. This skill's output is a *Deferred Gate list*: the exact commands for the developer's Docker container / rig, in order, with expected pass output. Never claim a gate passed; claim it was emitted.

---

## THE LADDER (cheapest first — stop emitting once coverage is sufficient for the change)

| Rung | Command | Catches | When required |
|------|---------|---------|---------------|
| 1. Async lint | `./scripts/lint-seastar-async.sh` | Unmarked `seastar::async(` call sites (Rule #12 — it does NOT offload to a thread pool) | Any change adding/near `seastar::async`; cheap enough to always run |
| 2. Unit tests | `make test-unit` | Logic regressions in reactor-free components | Every change |
| 3. Sanitized units | `make sanitize-test` | UAF, buffer overflows, UB (ASan/UBSan, clang; `Dockerfile.sanitize` image; 60s/test timeout) | Lifetime-sensitive changes: gates, timers, buffers, cross-shard, FFI |
| 4. Validation suite | `make validate` (full) / `make validate-quick` / `make validate-ci` | The four architecture invariants: zero reactor stalls, disk-I/O decoupling (P99 < 10% degradation under stress), no SMP queue overflow, atomic-free RadixTree. Single target: `./validation/validate_v1.sh --test stall` etc. | Hot-path, persistence, gossip, or routing-core changes |
| 5. Integration | `make test-integration` (multi-node Docker Compose; `-fast`/`-full`/`-ci` variants; `make integration-up/-down/-logs`) | Cluster behavior: graceful shutdown, peer discovery, prefix routing, streaming, negative paths | Anything touching HTTP surface, gossip, discovery, persistence recovery |
| 6. Fuzzing | `make fuzz-ci` (short) or `make fuzz-run-radix-tree` / `-request-rewriter` / `-stream-parser` (`FUZZ_TIME=600` to override) | Parser crashes on malicious input (libFuzzer + ASan/UBSan; `Dockerfile.fuzz` image) | Changes to radix tree, request rewriter, stream parser, or any new parser of untrusted bytes |
| 7. Benchmark | see `/benchmark` | Performance regressions (TTFT, cache-hit) | Routing-decision or hot-path changes |
| 8. Deploy checks | `make helm-lint`, `make helm-template` | Chart validity | Helm/deploy changes |

Also available: `make lint` (clang-tidy; needs `build/compile_commands.json`), `make format` (clang-format over src+tests).

**Gotcha:** `make fuzz-run-stream-parser` requires the default-allocator base image (`Dockerfile.base.default-alloc`) — on the production base image, Seastar's per-shard allocator crashes libFuzzer with `munmap_chunk: invalid pointer`. The Makefile detects this and prints the unblock recipe; see `tests/fuzz/README.md`.

**New parser of untrusted input?** Adding a fuzz harness (`tests/fuzz/<name>_fuzz.cpp`, wired into `fuzz_harnesses`) is part of the change, not a follow-up. Untrusted surfaces today: HTTP bodies, gossip packets, KV-event msgpack, Prometheus scrape text, EPP gRPC.

## MAPPING FAILURES BACK

| Failure signature | First suspect | Next step |
|-------------------|---------------|-----------|
| ASan use-after-free in callback/timer | Gate holder scope (Rule #5), lambda coroutine (Rule #16), `do_with` copy (Rule #20) | `/debug-build` |
| Reactor stall detected (validation test 1) | Blocking call or unyielding loop (Rules #12, #17) | `/debug-build`, check lint rung 1 |
| P99 collapse under disk stress (test 2) | Persistence work leaked onto reactor | `docs/internals/async-persistence.md` |
| Atomics found in RadixTree (test 4) | `std::shared_ptr`/atomic crept into tree path (Rule #0) | `/review` the diff |
| Fuzz crash | Parser edge case | Minimize with libFuzzer, fix, keep the corpus file |

---

## OUTPUT FORMAT

### Deferred Gates for this change
```
# In the dev Docker container, in order:
./scripts/lint-seastar-async.sh           # expect: "lint passed", exit 0
make test-unit                            # expect: 100% tests passed
make sanitize-test                        # expect: 100% tests passed, no sanitizer reports
...
```
For each rung: why it's included (which risk in this change it covers) or why it's skipped. If the developer reports a failure, triage with `/debug-build`.
