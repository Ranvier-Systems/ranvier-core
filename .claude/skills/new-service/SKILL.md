---
name: new-service
description: Scaffold a new service or component with correct Seastar lifecycle wiring - sharding shape, start/stop ordering, gates, metrics, config plumbing. Use when adding a new service, background worker, subscriber, or subsystem to the application.
argument-hint: [service-name-and-purpose]
---

I am ADDING A NEW SERVICE/COMPONENT:
$ARGUMENTS

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules. This skill exists because lifecycle wiring is where most latent Seastar bugs enter the codebase (Rules #5, #6, #13, #18).

---

## STEP 1: Choose the Shape

| Shape | When | Exemplars in this codebase |
|-------|------|---------------------------|
| **Pure component** (reactor-free header) | Logic needs no I/O, timers, or futures | `route_scorer.hpp`, `gie_epp_plan.hpp`, `proxy_retry_policy.hpp`, `rate_limiter_core.hpp`, `boundary_detector.hpp` |
| **`seastar::sharded<T>`** | Per-shard state, work on every core | `HttpController`, `TokenizerService`, `TelemetryService`, `ShardLoadBalancer` |
| **Shard-0 singleton** (`std::unique_ptr` member of `Application`) | One instance for the whole process (cluster protocols, watchers, external servers) | `GossipService` (via router), `HealthService`, `K8sDiscoveryService`, `KvEventSubscriber`, `GieEppServer` |
| **Dedicated OS thread + alien** | Wrapping a genuinely blocking library (Rule #12) | `TokenizerThreadPool`, `AsyncPersistence` (MPSC ring -> SQLite worker) |

**Default: pure core + thin Seastar wrapper.** Put every line of logic that *can* be reactor-free into a `_core`/plan header (unit-testable without a reactor), and keep the Seastar service as thin orchestration. This is the single highest-leverage design decision — it's what makes the component testable in this sandbox-constrained project.

If spawning OS threads: pin them off reactor cores via `worker_affinity` (see `src/worker_affinity.hpp`).

## STEP 2: Service Skeleton (Seastar shapes)

Every service with async work needs, at minimum:
- `seastar::future<> start()` and `seastar::future<> stop()`
- A `seastar::gate _gate` guarding every timer callback and background fiber. Holders must be scoped to the **full async chain** — moved into `.finally()` for detached work (Rule #5; the 2026-02-12 audit found three violations of exactly this).
- `seastar::metrics::metric_groups _metrics;` registered in `start()`, and **`_metrics.clear()` as the first line of `stop()`** (Rule #6).
- `stop()` order: deregister metrics → close gate (waits for in-flight callbacks) → cancel timers → tear down members.
- Fire-and-forget fibers: `(void)seastar::with_gate(_gate, ...)` with their own `.handle_exception` that logs at warn (Rules #18, #9).
- Bounded queues/maps with an explicit MAX and an overflow counter metric (Rule #4).

## STEP 3: Wire into Application Lifecycle

Both ends, same PR — a service that starts but isn't in `stop_services()` is a shutdown crash waiting to happen:

1. **Config:** add a config struct (`src/config_infra.hpp` for infra, `src/config_schema.hpp` for Ranvier-specific), parse it in `config_loader.cpp`, document keys in `ranvier.yaml.example`. Gate the whole service behind an `enabled` flag defaulting to the safe value.
2. **Startup:** add a numbered step in `Application::startup()` (`src/application.cpp`, the `.then()` chain starting ~line 997). Comment states *why this position* — what must already be alive (e.g. usage ledger starts after the controller "so set_usage_ledger_sink lands on a live controller").
3. **Shutdown:** add to `Application::stop_services()` (~line 1736). The existing steps carry ordering-precondition comments (see Step 4a3's (a)/(b) analysis) — write yours to that standard: which consumers must already be quiescent, which resources must still be alive.
4. **Draining:** if the service handles requests, participate in the DRAINING phase (see `phase_broadcast_draining` / `phase_drain_requests`).

## STEP 4: Standard Deliverables

- [ ] Unit tests for the pure core (reactor-free; see `/doc` for injection patterns)
- [ ] `CMakeLists.txt` registration (source files + tests; Seastar-dependent tests under `if(Seastar_FOUND)`)
- [ ] New `docs/internals/<name>.md` if this is a real subsystem; update `.dev-context/claude-context.md` Source Code Layout **always**
- [ ] Metrics documented (name, type, labels, cardinality bound)
- [ ] Run `/review` before commit; Deferred Gates for `make test-unit` and `./scripts/lint-seastar-async.sh`

---

## OUTPUT FORMAT

1. Shape decision + rationale (one paragraph)
2. Async flow Mermaid diagram (as in `/implement` Pass 0) if the service has any async work
3. File list, then the code
4. Lifecycle wiring diff for `application.cpp` with ordering comments
5. Deferred Gates for the developer's Docker build
