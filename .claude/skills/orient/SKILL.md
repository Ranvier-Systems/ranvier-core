---
name: orient
description: Orientation for a session starting work on this codebase - reading order, where things live, which skill to use for which task, and the non-negotiable constraints. Use at the start of a session, when unsure where to look, or when the user asks how to approach a task here.
argument-hint: [task-at-hand, optional]
---

## ORIENTATION

**Task at hand:** $ARGUMENTS

Ranvier Core is a Layer 7+ LLM traffic controller: C++20 on Seastar (shared-nothing, thread-per-core). It routes inference requests by token prefix to keep GPU KV-caches warm.

---

## NON-NEGOTIABLES (apply to every session, every task)

1. **Static analysis only.** Never run `cmake`/`make`/builds in this sandbox — Seastar deps are too heavy. Verification happens in the developer's Docker container; your job is to emit exact commands (**Deferred Gates**, see `/validate`) and never claim they passed.
2. **The 24 Hard Rules (#0–#23)** in `.dev-context/claude-context.md` are mandatory for all new and modified code. If you have not read them this session, read them before writing C++.
3. **No locks, no `std::shared_ptr`, async-only I/O, bounded containers.** When unsure whether a Seastar API works the way you think, verify against Seastar docs — do not guess.
4. **Do NOT bulk-read** `docs/` or `assets/` (token-heavy). Read specific `docs/internals/*.md` files by name; `grep` `.dev-context/cheatsheet.md` (2500 lines of operational runbook) rather than reading it whole.
5. Historical records in `.dev-context/` (dated audits, investigations) are frozen — quote them, never edit them.

## READING ORDER

1. `CLAUDE.md` (auto-loaded) → `.dev-context/claude-context.md` — architecture, layout, conventions, Hard Rules. **Always.**
2. The skill matching your task (table below).
3. The relevant `docs/internals/<area>.md` deep-dive — authoritative for its area; must be updated in the same PR that changes its area.
4. The source files themselves — read before modifying, always.

`.dev-context/README.md` indexes everything else (pitfalls reference, sync map, investigations, audit records).

## SKILL ROUTER

| You are... | Use |
|------------|-----|
| Planning a multi-file feature | `/plan` |
| Implementing a feature or change | `/implement` |
| Adding a whole new service/subsystem | `/new-service` |
| Triaging a compile error, crash, or failing test | `/debug-build` |
| Responding to a production problem | `/incident` |
| Investigating performance | `/perf`, then `/benchmark` to measure |
| Designing/running a load benchmark or A/B | `/benchmark` |
| Choosing what tests/verification to run | `/validate` |
| Reviewing code before commit | `/review` |
| Writing tests + syncing docs after a change | `/doc` |
| Restructuring without behavior change | `/refactor` |
| Periodic health check of recent work | `/audit` |
| Hostile security/robustness sweep | `/adversarial-audit` |
| Hunting wrong-answer bugs (counter drift, side-index divergence, contract violations) | `/invariant-audit` |
| Turning a bug class into a Hard Rule | `/extract-pattern` |
| Assessing project direction | `/strategic-review` |

## MAP

| Area | Where |
|------|-------|
| Routing core (ART, scoring, learn/evict) | `src/router_service.*`, `src/radix_tree.hpp`, `src/route_scorer.hpp` |
| HTTP ingress/proxy/streaming | `src/http_controller.*`, `src/stream_parser.*`, `src/connection_pool.hpp` |
| Tokenization (3-layer) | `src/tokenizer_service.*`, `src/tokenizer_thread_pool.*` |
| Cluster gossip + DTLS | `src/gossip_*`, `src/dtls_context.*`, `src/crypto_offloader.*` |
| KV-event ingestion (vLLM ZMQ) | `src/kv_event_{decoder,ledger,subscriber}.*` |
| GIE Endpoint-Picker (Envoy ext_proc) | `src/gie_epp_*` |
| Telemetry / usage accounting | `src/telemetry_*`, `src/usage_ledger_*`, `src/gpu_seconds_accounting.hpp` |
| Persistence (SQLite, MPSC) | `src/async_persistence.*`, `src/sqlite_persistence.*`, `src/mpsc_ring_buffer.hpp` |
| Lifecycle & wiring | `src/application.cpp` (`startup()` ~997, `stop_services()` ~1736) |
| Config | `src/config_*.hpp`, `ranvier.yaml.example` |
| Unit tests / fuzz / integration | `tests/unit/`, `tests/fuzz/`, `tests/integration/` |
| Work tracking | `BACKLOG.md` (current), `CHANGELOG.md` |

---

State the task type you've identified and invoke the matching skill. If the task spans several (most features do: plan → implement → review → doc → validate), say the sequence up front.
