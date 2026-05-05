# Adversarial System Audit Report

**Date:** 2026-01-14
**Auditor:** Claude Code (Opus 4.5)
**Scope:** `src/` directory
**Criticality Score:** 6/10

---

## Executive Summary

The codebase demonstrates solid architectural discipline with proper Seastar patterns in most places. However, several structural vulnerabilities exist that would manifest under adversarial conditions or 100x traffic scale. The critical finding is **blocking I/O on the reactor thread** in K8s discovery. The primary attack surface involves **unbounded per-entity maps** that could exhaust memory under adversarial input.

---

## Findings by Audit Lens

### Async Integrity Lens (Seastar Reactor Violations)

| Severity | File:Line | Issue | Recommendation |
|----------|-----------|-------|----------------|
| **CRITICAL** | `k8s_discovery_service.cpp:234` | **Blocking I/O on reactor thread**: `std::ifstream ca_file(_config.ca_cert_path)` performs synchronous file read. This violates the "No Locks/Async Only" rule and will stall the reactor during K8s discovery. | Use `seastar::open_file_dma()` + `seastar::make_file_input_stream()` as done for token loading at lines 204-207. |
| **MEDIUM** | `connection_pool.hpp:99-109` | **Timer callback without gate guard**: `_reaper_timer([this] { reap_dead_connections(); })` captures `this` without RAII gate protection. Destructor cancels timer but already-queued callback creates use-after-free race. | Add `seastar::gate _timer_gate`; callbacks acquire `_timer_gate.hold()` at entry; destructor closes gate before canceling timer. |

### Edge-Case Crash Lens

| Severity | File:Line | Issue | Recommendation |
|----------|-----------|-------|----------------|
| **MEDIUM** | `metrics_service.hpp:443-445` | **Null pointer dereference**: `metrics()` returns `*g_metrics` without null check. If `init_metrics()` not called before first use, immediate segfault. | Add assertion: `assert(g_metrics && "init_metrics() must be called first")` or lazy-init pattern. |

### Architecture Drift Lens

| Severity | File:Line | Issue | Recommendation |
|----------|-----------|-------|----------------|
| **LOW** | - | No significant architecture drift detected. Layering is clean: HttpController -> RouterService -> Persistence via AsyncPersistenceManager. | Maintain discipline; document layer boundaries in header comments. |

### Scale & Leak Lens (Resource Exhaustion Vectors)

| Severity | File:Line | Issue | Recommendation |
|----------|-----------|-------|----------------|
| **HIGH** | `metrics_service.hpp:436-440` | **Memory leak**: `g_metrics = new MetricsService()` with no corresponding `delete`. Thread-local raw `new` leaks on process shutdown. | Add `destroy_metrics()` function; call during shard shutdown. Or use `thread_local std::unique_ptr<MetricsService>`. |
| **HIGH** | `metrics_service.hpp:397` | **Unbounded map growth**: `_per_backend_metrics` grows with each unique BackendId. Under attack with spoofed IDs, unbounded memory consumption. Entries never removed. | Cap at `MAX_TRACKED_BACKENDS` (e.g., 1000). Implement LRU eviction or tie to backend lifecycle. Add `remove_backend_metrics(BackendId)`. |
| **HIGH** | `circuit_breaker.hpp:210` | **Unbounded circuit map**: `_circuits` grows with each BackendId. While `reset(id)` exists, nothing calls it when backends are removed. Memory grows monotonically. | Add `remove_circuit(BackendId)` called from backend removal path. Or expire stale circuits periodically. |
| **MEDIUM** | `connection_pool.hpp:348` | **Map entry leak**: `_pools` map grows per socket_address. While `clear_pool()` removes deque contents, map entries for removed backends remain as empty deques. | In `clear_pool()`, also call `_pools.erase(it)` to fully remove the entry. |
| **MEDIUM** | `gossip_service.cpp:1174-1195` | **Dedup window unbounded per-peer**: `_received_seq_sets` and `_received_seq_windows` grow per peer address. While sliding window evicts old entries, peer count itself is unbounded. | Add `MAX_TRACKED_PEERS` limit. Clean up peer state in `refresh_peers()` when peers are removed. |

---

## Structural Fixes for BACKLOG.md

### High Priority (Attack Surface)

- [ ] **k8s_discovery_service.cpp:234** - Replace `std::ifstream` with `seastar::open_file_dma()` for CA cert loading to avoid reactor stall
- [ ] **metrics_service.hpp** - Add `destroy_metrics()` for graceful cleanup; cap `_per_backend_metrics` at MAX_TRACKED_BACKENDS
- [ ] **circuit_breaker.hpp** - Add `remove_circuit(BackendId)` called when backend is removed; or add periodic stale circuit reaper

### Medium Priority (Robustness)

- [ ] **connection_pool.hpp** - Add gate guard to reaper timer; fully erase map entries in `clear_pool()`
- [ ] **metrics_service.hpp:443** - Add null assertion in `metrics()` accessor
- [ ] **gossip_service.cpp** - Add MAX_TRACKED_PEERS limit to dedup structures

---

## Anti-Pattern Candidates for claude-pattern-extractor-prompt.md

### 12. The Blocking-ifstream-in-Coroutine Anti-Pattern

See canonical wording in `.dev-context/claude-context.md` §12. (This audit originally proposed Rule 12; the canonical version was corrected 2026-05-05 to also cover the `seastar::async`/`seastar::thread` false-fix and the dedicated-OS-thread + alien-API pattern for blocking C libraries.)

---

### 13. The Thread-Local-Raw-New Anti-Pattern

See canonical wording in `.dev-context/claude-context.md` §13. (Corrected 2026-05-05: `thread_local std::unique_ptr<T>` is only safe when `~T()` is trivial — its destructor runs at thread-exit, after reactor teardown.)

---

### 14. The Unbounded-Per-Entity-Map Anti-Pattern

**THE PATTERN:** Using `std::unordered_map<EntityId, State>` where entities come from external input (backend IDs, peer addresses, request IDs) without size limits.

**THE CONSEQUENCE:** Adversarial input can create unlimited entities. Each map entry consumes memory. Under attack, memory grows until OOM. Production: 10 backends = fine. Attack: 10 million spoofed IDs = crash.

**THE LESSON:** *Hard Rule: Every per-entity map needs MAX_SIZE + eviction.* Either tie to entity lifecycle (remove when entity removed) or add LRU eviction + overflow counter metric.

**PROMPT GUARD:** "Every map keyed by external IDs must have explicit MAX_SIZE with eviction strategy--LRU, oldest-first, or tied to entity lifecycle."

---

## Positive Findings

The following patterns demonstrate good practice:

1. **async_persistence.hpp** - Excellent documentation of mutex justification with RAII gate pattern for timer safety
2. **gossip_service.cpp** - Proper use of `_timer_gate.hold()` in all timer callbacks (lines 242-246, 696-700, etc.)
3. **gossip_service.cpp** - Security-conscious dedup window that persists across resync to prevent replay attacks (lines 1707-1716)
4. **connection_pool.hpp** - Proper async close pattern using `close_bundle_async()` to prevent assertion failures
5. **radix_tree.hpp** - Uses `std::unique_ptr` (NodePtr) per Seastar shared-nothing model

---

## Methodology

This audit applied four adversarial lenses:

1. **Async Integrity**: Violations of Seastar's reactor model (blocking calls, sequential co_await in loops, discarded futures, deadlocks)
2. **Edge-Case Crashes**: Unhandled external failures (network errors, NULL returns, malformed input, timeouts)
3. **Architecture Drift**: Layer boundary violations (controller calling persistence directly, business logic in wrong layers)
4. **Scale & Leak**: Resource exhaustion vectors (unbounded containers, O(n^2) algorithms, uncleaned callbacks, timer leaks)

Reference: `.dev-context/claude-context.md` "No Locks/Async Only" rules and Anti-Patterns section.
