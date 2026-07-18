# Ranvier Core - v1.0 Production Release TODO

> **Architectural Gap Analysis**
> Generated: 2025-12-27 | Reorganized: 2026-03-22
> Current State: Alpha (stable ~60ms P99 TTFT in Docker testbed)

This document tracks **open** backlog items for Ranvier Core v1.0.
Completed items have been archived in [BACKLOG-ARCHIVE.md](BACKLOG-ARCHIVE.md).

---

## Table of Contents

1. [Core Data Plane](#1-core-data-plane)
2. [Distributed Reliability](#2-distributed-reliability)
3. [Observability](#3-observability)
4. [Infrastructure & Security](#4-infrastructure-security)
5. [Developer Experience](#5-developer-experience)
6. [Integration Tests (End-to-End Validation)](#6-integration-tests-end-to-end-validation)
7. [Strategic Assessment (2026-01-31)](#7-strategic-assessment-2026-01-31)
8. [Benchmark Extensions](#8-benchmark-extensions)
9. [Load-Aware Prefix Routing](#9-load-aware-prefix-routing)
10. [HTTP Controller Review (2026-02-14)](#10-http-controller-review-2026-02-14)
11. [Hot-Path Performance Audit (2026-02-15)](#11-hot-path-performance-audit-2026-02-15)
12. [Request Lifecycle Performance Analysis (2026-02-20)](#12-request-lifecycle-performance-analysis-2026-02-20)
13. [Code Modularity (Low Priority)](#13-code-modularity-low-priority)
14. [Shard 0 Role Isolation Analysis (2026-03-06)](#14-shard-0-role-isolation-analysis-2026-03-06)
15. [Intelligence Layer Roadmap (2026-03-25)](#15-intelligence-layer-roadmap-2026-03-25)
16. [KV-Cache Compression-Aware Routing (2026-04-05)](#16-kv-cache-compression-aware-routing-2026-04-05)
17. [Hard Rules Audit Follow-ups (2026-05-05)](#17-hard-rules-audit-follow-ups-2026-05-05)
18. [Request Lifecycle Crash-Risk Audit Follow-ups (2026-05-08)](#18-request-lifecycle-crash-risk-audit-follow-ups-2026-05-08)
19. [Heterogeneous Backend Support (2026-05-16)](#19-heterogeneous-backend-support-2026-05-16)
20. [Routing Parity & Ecosystem Alignment](#20-routing-parity--ecosystem-alignment-2026-06-07)
21. [Cache-Aware Autoscaling Telemetry (2026-06-18)](#21-cache-aware-autoscaling-telemetry-2026-06-18)
22. [Invariant Audit Findings (2026-07-03)](#22-invariant-audit-findings-2026-07-03)
23. [Holistic Audit Findings (2026-07-04)](#23-holistic-audit-findings-2026-07-04)
24. [Adversarial Audit Findings — Pass A (2026-07-04)](#24-adversarial-audit-findings-pass-a-2026-07-04)
25. [Benchmark Tooling P0 — Re-anchor the Truth (2026-07-06)](#25-benchmark-tooling-p0--re-anchor-the-truth-2026-07-06)

---

## 1. Core Data Plane
### 1.1 SIMD Optimization for Radix Tree Lookups

- [ ] **Implement SIMD key search in Node16**
  _Justification:_ Node16 uses linear search over 16 keys. AVX2/SSE4.2 can compare all 16 keys in a single instruction, reducing lookup from O(16) to O(1) comparisons.
  _Location:_ `src/radix_tree.hpp:84-92`
  _Complexity:_ Medium

- [ ] **Add SIMD-accelerated prefix comparison**
  _Justification:_ Path compression stores multi-token prefixes. Using `_mm_cmpeq_epi32` for batch comparison can speed up prefix matching by 4-8x for long prefixes.
  _Location:_ `src/radix_tree.hpp:227-231` (insert), `548-555` (lookup)
  _Complexity:_ Medium

- [ ] **Evaluate memory-mapped tokenizer vocabulary**
  _Justification:_ Current tokenizer loads vocabulary into heap. Memory-mapping enables zero-copy access and reduces cold-start time for large vocabularies (100k+ tokens).
  _Location:_ `src/tokenizer_service.cpp`
  _Complexity:_ Low

- [ ] **Partial tokenization for routing decisions**
  _Justification:_ Tokenization accounts for ~10.6ms of ~10.62ms total routing decision time (99.9% of overhead). The ART lookup itself is <0.01ms. Currently the full prompt is tokenized, but routing only needs enough tokens to match against the prefix tree depth. Truncating the input to a byte budget before tokenizing could significantly reduce per-request tokenization cost.
  _Nuance:_ The system supports rewriting with tokenized output depending on the endpoint, so partial tokenization must not interfere with downstream token reuse. May require a two-phase approach: partial tokenization for routing, full tokenization deferred to the forwarding path only when needed.
  _Current mitigation:_ Tokenization is offloaded to a dedicated thread pool (not blocking the reactor), so the 10.6ms wall-clock time does not stall the event loop. Real overhead is thread pool queue contention + context switch, much less than 10ms.
  _Benchmark evidence:_ 30m run (b63c165, 2026-02-28) — CodeLlama-13b, 20 users, 8 GPUs. Routing decision P50: 10.62ms (tokenization: 10.61ms, ART: 0.01ms). Despite this overhead, P99 TTFT improved 78.2% (4500ms → 980ms) and cache hit rate improved from 12.5% to 73.9%.
  _Location:_ `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/http_controller.cpp`
  _Complexity:_ Medium
  _Priority:_ P3 — Optimization. Not urgent given thread pool offloading and dominant TTFT improvement.

### 1.2 Zero-Copy SSE Parsing Refinements

- [ ] **Implement scatter-gather I/O for backend responses**
  _Justification:_ Currently SSE chunks are copied between buffers. Using Seastar's `scattered_message` can eliminate copies in the streaming path.
  _Location:_ `src/stream_parser.cpp`, `src/http_controller.cpp:400+`
  _Complexity:_ High

- [ ] **Add chunk coalescing for small SSE events**
  _Justification:_ Many small `data:` chunks cause syscall overhead. Coalescing into larger TCP segments improves throughput under high concurrency.
  _Location:_ `src/stream_parser.hpp`
  _Complexity:_ Medium

### 1.4 Memory Efficiency

- [ ] **Remove unnecessary atomics from ShardLoadMetrics**
  _Justification:_ `ShardLoadMetrics` uses `std::atomic<uint64_t>` for `_active_requests`, `_queued_requests`, and `_total_requests`, but since each shard has its own thread-local instance (`thread_local std::unique_ptr<ShardLoadMetrics>`), atomic operations are unnecessary overhead. With Seastar's shared-nothing model, regular `uint64_t` would suffice since there's no cross-thread access to the same instance.
  _Approach:_ Replace `std::atomic<uint64_t>` with `uint64_t` for all metrics counters. Update accessor methods to remove memory ordering parameters.
  _Location:_ `src/shard_load_metrics.hpp:132-134`
  _Complexity:_ Low

- [ ] **Batch CryptoOffloader statistics updates**
  _Justification:_ `CryptoOffloader` increments multiple atomic counters (`_total_ops`, `_inline_ops`, `_offloaded_ops`, etc.) on every crypto operation. While these are lightweight (relaxed memory order), they add overhead in high-throughput scenarios. More concerning is `_queue_depth` which uses `fetch_add`/`fetch_sub` for every offloaded operation.
  _Approach:_ Use per-operation local counters that batch into atomics periodically (e.g., every 100 ops or via timer). Consider non-atomic counters for same-shard-only statistics, exposing them via snapshot functions.
  _Location:_ `src/crypto_offloader.hpp:181-188`
  _Complexity:_ Medium

- [ ] **Audit codebase for abseil container opportunities**
  _Justification:_ The codebase uses `std::unordered_map` and `std::vector` in several places where abseil alternatives (`absl::flat_hash_map`, `absl::InlinedVector`) would provide better performance. Abseil is already a dependency (used in RadixTree and TokenizationCache).
  _Candidates:_
  - `absl::flat_hash_map`: Replace `std::unordered_map` for better cache locality and ~20-40% faster lookups. Already done for TokenizationCache.
  - `absl::InlinedVector<T, N>`: Replace `std::vector<T>` for small, bounded collections to avoid heap allocation. Good for: token vectors in cache entries (N=64), small config lists, temporary buffers.
  - `absl::flat_hash_set`: Replace `std::unordered_set` where used.
  _Files to audit:_
  - `src/circuit_breaker.hpp` - `_circuits` map
  - `src/rate_limiter.hpp` - `_buckets` map
  - `src/connection_pool.hpp` - `_pools` map
  - `src/gossip_service.cpp` - various peer tracking maps
  - `src/router_service.cpp` - pending routes vectors
  _Note:_ While individual gains are small (microseconds), cumulative effect across hot paths may be measurable. Low complexity since abseil is already linked.
  _Location:_ Multiple files (see candidates above)
  _Complexity:_ Low


---

## 2. Distributed Reliability
### 2.1 Network Partition Handling

- [ ] **Push-based cache eviction notifications from GPU backends**
  _Justification:_ Ranvier infers backend KV cache state from routing history, creating a staleness window of minutes-to-hours. With 1M-token contexts, backends evict more aggressively, causing misrouted requests and wasted prefill. Push notifications cut staleness to ~25ms.
  _Design doc:_ [`docs/architecture/push-cache-eviction-notifications.md`](docs/architecture/push-cache-eviction-notifications.md)
  _Approach:_ HTTP callback endpoint (`POST /v1/cache/events`) + `X-Ranvier-Prefix-Hash` header echoing. Optional sidecar for engines that can't implement directly. New gossip packet type for cluster propagation.
  _Complexity:_ High (4 phases: MVP, cluster propagation, load events + sidecar, upstream engagement)

- [ ] **Residency signal source: vLLM v1 usage gauge fires only at saturation (#527)**
  _Justification:_ The residency downgrade keys off `1 - gpu_cache_usage_perc`, but vLLM v1's
  gauge counts only blocks held by RUNNING requests (cached prefixes are reclaimable and report
  as free), so the signal crosses the 0.2 threshold only under active-demand saturation — far
  later than the intended "prefix likely evicted" condition, and never in lighter regimes.
  Candidate replacement signals: (a) scrape-side prefix-cache hit counters
  (`vllm:gpu_prefix_cache_{queries,hits}` deltas — cheap, per-backend, no engine changes), or
  (b) the per-prefix push eviction events above (exact, heavier). Decide the signal direction
  before further residency threshold tuning; the current signal is benchmarked and is a net tail
  win in the regime where it fires.
  _Benchmark evidence:_ `docs/benchmarks/cache-residency-ab-benchmark.md` — gauge semantics
  measured 2026-06-11 (engine logs `Running: 0 reqs ... usage 0.0%` beside a 91% prefix-cache
  hit rate); A/B at saturation: overall TTFT P99 −57.1% at a 0.2% downgrade rate.
  _Location:_ `src/health_service.cpp` (scrape), `src/vllm_metrics.hpp`
  (`estimated_prefix_retention`), `src/router_service.cpp` (downgrade gate)
  _Complexity:_ Medium (scrape-side hit-rate signal) / High (push-events path)

- [ ] **Revisit `vllm_metrics_timeout` default (200ms plausibly censors the scrape under load)**
  _Justification:_ vLLM's Python `/metrics` endpoint slows precisely when the engine is busy, so
  a 200ms fetch timeout risks failing the health scrape at the very instants the cache-usage
  signal matters (residency #527, capacity headroom, `load_score`'s cache term). The benchmark
  compose now overrides to 1000ms via `RANVIER_HEALTH_VLLM_METRICS_TIMEOUT_MS`; decide whether
  the shipped default should follow. Caveat for the implementer: `scrape_all_vllm_metrics()`
  awaits backends sequentially, so a raised timeout times N slow backends could stretch the 5s
  health cadence — check scrape concurrency (Hard Rule #2) alongside the default change.
  _Benchmark evidence:_ at 1000ms under sustained saturation, 0 scrape failures across both A/B
  legs (3024/3016 successes); 200ms-era failure rates were never directly measured — the
  success/failed/suppressed counters are now printed per leg by `scripts/bench-residency-ab.sh`.
  _Location:_ `src/config_infra.hpp` (default), `src/health_service.cpp`
  _Complexity:_ Low (default change) / Medium (with scrape parallelization)

- [ ] **Add partition healing with route reconciliation**
  _Justification:_ After partition heals, nodes have divergent route tables. Need incremental sync protocol to merge without full state transfer.
  _Location:_ `src/gossip_service.cpp`
  _Complexity:_ High

- [ ] **Implement protocol version negotiation**
  _Justification:_ Rolling upgrades require backward compatibility. Gossip packets have `version` field but no negotiation or feature flags.
  _Location:_ `src/gossip_service.hpp:54`
  _Complexity:_ Medium

### 2.2 Dynamic Cluster Re-balancing

- [ ] **Add weighted route distribution across backends**
  _Justification:_ All routes treated equally. GPUs with more VRAM or faster interconnect should receive proportionally more prefixes.
  _Location:_ `src/router_service.cpp:189-246`
  _Complexity:_ Medium

- [ ] **Implement route migration on backend scale-down**
  _Justification:_ When a backend is removed, its routes are orphaned. Proactive migration to healthy backends preserves cache locality.
  _Location:_ `src/router_service.cpp`, `src/gossip_service.cpp`
  _Complexity:_ High

- [ ] **Add hot-spot detection and load shedding**
  _Justification:_ Popular prefixes can overwhelm a single backend. Detect hot prefixes and replicate routes to multiple backends.
  _Location:_ `src/radix_tree.hpp`, `src/router_service.cpp`
  _Complexity:_ High

### 2.3 Gossip Protocol Reliability

- [ ] **Add anti-entropy protocol for periodic state sync**
  _Justification:_ Gossip only propagates new routes. Nodes that missed announcements have no catch-up mechanism. Periodic Merkle tree comparison ensures convergence.
  _Location:_ `src/gossip_service.cpp`
  _Complexity:_ High

- [ ] **Route-announcement convergence after a divert (cross-node route flapping)**
  _Justification:_ A route re-learn after any divert (residency downgrade, load-aware fallback,
  cost divert #545, backend death) does not converge across the cluster: `learn_route_global()`
  dedups only same-backend routes, each node re-learns and re-broadcasts whichever backend it
  last served, and `ROUTE_ANNOUNCEMENT` carries no version or tie-break — so nodes sustain
  disagreement and the prefix's backend flaps from the client's perspective. Candidate fixes:
  versioned announcements (last-writer-wins), or suppressing re-learn when the served backend is
  warm/healthy. Related but distinct from the anti-entropy item above: that one heals *missed*
  announcements, this one resolves *conflicting* ones.
  _Benchmark evidence:_ Residency A/B (2026-06-11, 8x A100-40GB, commit `30fd329`): 39 residency
  downgrades produced 1,485 client-observed backend flips (38x amplification) over a 30m leg.
  Side effects cut both ways — flapped prefixes end up warm on two backends (replication-like
  tail benefit, visible in the run) at the cost of duplicate KV occupancy and repeated
  announcement traffic. Details: `docs/benchmarks/cache-residency-ab-benchmark.md`
  § "New finding: cross-node route flapping".
  _Location:_ `src/router_service.cpp` (learn_route_global dedup, learn_route_remote),
  `src/gossip_protocol.{hpp,cpp}` (ROUTE_ANNOUNCEMENT wire format)
  _Complexity:_ High (wire-format versioning; Medium if relearn-suppression alone proves sufficient)

---


---

## 3. Observability
### 3.1 Prometheus Metrics Enhancements

- [ ] **Add cluster health metrics**
  _Justification:_ Need visibility into gossip peer health, message rates, and sync lag.
  _Metrics:_ `ranvier_cluster_peers_alive`, `ranvier_gossip_lag_seconds`
  _Location:_ `src/gossip_service.cpp`, `src/metrics_service.hpp`
  _Complexity:_ Low

### 3.3 Structured Logging Improvements

- [ ] **Add structured JSON logging option**
  _Justification:_ Plain text logs are difficult to parse. JSON logs enable integration with ELK/Splunk/Loki.
  _Location:_ `src/main.cpp`, `src/config.hpp`
  _Complexity:_ Low

- [ ] **Add audit logging for admin operations**
  _Justification:_ No record of who registered/removed backends. Required for security compliance.
  _Events:_ `backend_registered`, `backend_removed`, `route_cleared`, `config_reloaded`
  _Location:_ `src/http_controller.cpp`
  _Complexity:_ Low

- [ ] **Implement log sampling for high-volume events**
  _Justification:_ Debug logs at high QPS can overwhelm storage. Add configurable sampling rate.
  _Location:_ `src/logging.hpp`, `src/config.hpp`
  _Complexity:_ Low

---


---

## 4. Infrastructure & Security
### 4.1 Container Security

- [ ] **Add seccomp profile**
  _Justification:_ Restrict syscalls to those required by Seastar. Blocks exploitation of kernel vulnerabilities.
  _Location:_ Create `seccomp-profile.json`
  _Complexity:_ Medium

### 4.2 Transport Security

- [ ] **Add mTLS for backend connections**
  _Justification:_ Backend connections are unencrypted. Sensitive prompt data exposed on network.
  _Location:_ `src/http_controller.cpp`, `src/connection_pool.hpp`
  _Complexity:_ Medium

- [ ] **Implement certificate rotation**
  _Justification:_ Static certificates require restart to rotate. Add file watcher for automatic reload.
  _Location:_ `src/main.cpp`, `src/config.hpp`
  _Complexity:_ Medium

### 4.3 Authentication & Authorization

- [ ] **Add role-based access control (RBAC)**
  _Justification:_ Single admin key grants all permissions. Need separation between "read metrics" and "modify routes".
  _Roles:_ `admin`, `operator`, `viewer`
  _Location:_ `src/http_controller.cpp`
  _Complexity:_ Medium

- [ ] **Integrate external secrets management**
  _Justification:_ API keys stored in plaintext config. Support Kubernetes Secrets, HashiCorp Vault, or AWS Secrets Manager.
  _Location:_ `src/config.hpp`
  _Complexity:_ Medium

### 4.4 Rate Limiting & DoS Protection

- [ ] **Implement per-API-key rate limiting**
  _Justification:_ Current rate limiting is per-IP only. Shared infrastructure (NAT) causes false positives.
  _Location:_ `src/rate_limiter.hpp`, `src/http_controller.cpp`
  _Complexity:_ Medium

- [ ] **Add connection limits per client**
  _Justification:_ Single client can exhaust connection pool. Add `max_connections_per_client` config.
  _Location:_ `src/connection_pool.hpp`, `src/config.hpp`
  _Complexity:_ Low

---


---

## 5. Developer Experience
### 5.1 CI/CD Pipeline

- [ ] **Add fuzzing for gossip protocol parser**
  _Justification:_ Gossip deserialization handles untrusted input. Fuzzing detects buffer overflows and parsing bugs.
  _Tool:_ libFuzzer or AFL++
  _Location:_ `tests/fuzz/`
  _Complexity:_ Medium

- [ ] **Add SAST (Static Application Security Testing)**
  _Justification:_ Automated detection of security vulnerabilities in CI.
  _Tools:_ CodeQL, Semgrep, or Clang Static Analyzer
  _Location:_ `.github/workflows/`
  _Complexity:_ Low

- [ ] **Add dependency vulnerability scanning**
  _Justification:_ Third-party dependencies may have CVEs. Automate detection.
  _Tools:_ Dependabot, Trivy, or Snyk
  _Location:_ `.github/workflows/`
  _Complexity:_ Low

### 5.2 Testing Infrastructure

- [ ] **Add chaos testing for cluster scenarios**
  _Justification:_ Integration tests use clean network. Need to test packet loss, latency, and node failures.
  _Tools:_ Toxiproxy, tc (traffic control), or Chaos Mesh
  _Location:_ `tests/integration/`
  _Complexity:_ Medium

- [ ] **Increase unit test coverage to 80%+**
  _Justification:_ Current coverage unknown. Many edge cases untested.
  _Focus:_ Error paths in gossip, connection pool edge cases, config validation
  _Location:_ `tests/unit/`
  _Complexity:_ Medium

- [ ] **Add property-based testing for Radix Tree**
  _Justification:_ Current tests use fixed inputs. Property-based testing finds edge cases automatically.
  _Tool:_ RapidCheck (C++) or custom generators
  _Location:_ `tests/unit/radix_tree_test.cpp`
  _Complexity:_ Medium

### 5.3 Client SDKs & Documentation

- [ ] **Add runbook for common operational tasks**
  _Justification:_ No troubleshooting guide. Operators need documentation for: scaling, debugging, disaster recovery.
  _Location:_ `docs/runbook.md`
  _Complexity:_ Low

### 5.5 rvctl CLI Enhancements

The `rvctl` CLI tool (tools/rvctl) provides operator-friendly access to Ranvier's Admin API. Several endpoints and quality-of-life features are not yet exposed.

- [ ] **Refactor rvctl into package structure when >4000 lines**
  _Justification:_ Currently ~3300 lines as single file for easy deployment. If significant features are added, refactoring improves maintainability. Current section comments provide navigation.
  _Threshold:_ >4000 lines or >50 functions
  _Structure:_ `rvctl_lib/{cli.py, client.py, config.py, commands/, completions/}`
  _Location:_ `tools/rvctl`
  _Complexity:_ Medium
  _Priority:_ Low (defer until threshold reached)

- [ ] **Add unit tests for rvctl command functions**
  _Justification:_ CLI commands lack automated testing. Mock-based tests would catch regressions.
  _Location:_ `tests/unit/test_rvctl.py` (new)
  _Complexity:_ Medium
  _Priority:_ Low

- [ ] **Add `rvctl doctor` command for connectivity troubleshooting**
  _Justification:_ Operators need quick diagnosis of connection issues. Command would check: API reachability, auth validity, metrics endpoint, DNS resolution.
  _Usage:_ `rvctl doctor`
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ Low

### 5.6 Build System

- [ ] **Add Windows/macOS cross-compilation support**
  _Justification:_ Contributors on non-Linux need Docker for development. Native builds improve DX.
  _Note:_ Seastar is Linux-only; may require abstraction layer.
  _Location:_ `CMakeLists.txt`
  _Complexity:_ High

- [ ] **[P3] Pin Seastar to a specific commit in `Dockerfile.base` and `Dockerfile.base.default-alloc`**
  _Justification:_ Both Dockerfiles currently do `git clone --depth 1 https://github.com/scylladb/seastar.git` with no SHA pin. The production base and the default-allocator base are independent Docker images that can be rebuilt at different times, so they can silently end up tracking different upstream Seastar commits — production on one SHA, the fuzz/sanitizer unblock base on another. Any divergence between the two that affects test-vs-production parity (allocator-adjacent code, header signatures, ABI) would only surface as a confusing test failure rather than as a config diff. The same goes for the production base rebuilding against a different SHA than a prior production deployment, which is a release-reproducibility concern independent of §18.
  _Approach:_ Pin to a tested commit in both Dockerfiles (mirror the pattern already in place for `tokenizers-cpp` at `Dockerfile.base:73-76`, including the "To update: see ..." comment pointing at this ticket). Bump the pin deliberately when there's a reason (Seastar fix needed, security patch). Add a short bump-checklist subsection to this ticket once the initial pin lands.
  _Location:_ `Dockerfile.base:50`, `Dockerfile.base.default-alloc:91-92` (Seastar `git clone` steps).
  _Complexity:_ Low.
  _Surfaced by:_ §18 P3 "Unblock Seastar-dependent fuzzing" — the default-allocator base added there made the existing drift risk in `Dockerfile.base` more visible, since now two Dockerfiles independently clone Seastar HEAD.

---


---

## 6. Integration Tests (End-to-End Validation)

**Status: CLOSED (2026-05-17).** All 21 sub-tickets resolved across 12 `tests/integration/` suites.

Closure narrative — the end-to-end coverage roadmap carved out by the v2.0.0 Strategic Assessment (§7) on 2026-01-31 and driven to completion between January and May 2026, covering shared fixtures, mock-backend fault injection, compose profiles, Make targets, and seven sub-areas (HTTP pipeline, routing, resilience, observability, lifecycle/persistence, edge cases) — extracted on 2026-05-17 to a dedicated audit doc to keep this backlog focused on active work:

- [`docs/audits/integration-tests-2026-01-31.md`](docs/audits/integration-tests-2026-01-31.md)
  — per-ticket resolutions and end-state summary (this section's history).
- [`BACKLOG.md §7`](#7-strategic-assessment-2026-01-31)
  — the v2.0.0 Strategic Assessment that framed end-to-end coverage as a launch blocker.

The section heading and anchor (`#6-integration-tests-end-to-end-validation`) are preserved here as a stable pointer for the table of contents and the cross-references in the audit doc.

---

## 7. Strategic Assessment (2026-01-31)

**Status: CLOSED (2026-05-21).** Both tracking items resolved.

- §6 (integration test coverage) — closed 2026-05-17, see [`docs/audits/integration-tests-2026-01-31.md`](docs/audits/integration-tests-2026-01-31.md).
- The lone §8.6 action item (track `config.hpp` complexity, split at >2000 LOC) was preempted by an actual split: `src/config.hpp` is now a 19-line facade re-exporting `config_schema.hpp` (572 LOC, structs) and `config_loader.{hpp,cpp}` (1941 LOC, YAML parsing), with `config_infra.hpp` (450 LOC) additionally separating infrastructure from product config. The >2000 LOC trigger on the monolithic header can never fire.

The section heading and anchor (`#7-strategic-assessment-2026-01-31`) are preserved as a stable pointer for the table of contents and the §6 closure narrative.

---

## 8. Benchmark Extensions
### 9.1 Production Prompt Traces

- [ ] **Define trace format (JSON/JSONL) for production prompt patterns**
  _Justification:_ Synthetic prompts don't capture real-world prompt distribution. Production traces enable realistic performance validation.
  _Deliverables:_
  - Define schema capturing prompt content, timestamps, user sessions
  - Support prefix annotation for shared system messages
  - Include metadata: model, temperature, max_tokens
  _Location:_ `tests/integration/data/traces/` (new), schema doc
  _Complexity:_ Low

- [ ] **Modify locustfile_real.py to load and replay traces**
  _Justification:_ Current Locust test uses synthetic prompts with fixed distributions. Trace replay enables historical traffic patterns.
  _Approach:_ Add `TraceLoader` class, `--trace-file` parameter, timestamp-based replay scheduling
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add --trace-file option to bench.sh**
  _Justification:_ Simplify trace-based benchmarking via CLI.
  _Location:_ `scripts/bench.sh`
  _Complexity:_ Low

- [ ] **Create tool to anonymize/sanitize production logs into trace format**
  _Justification:_ Production logs contain sensitive data. Anonymization tool enables safe trace collection.
  _Approach:_ PII detection, content hashing, prefix preservation, configurable anonymization rules
  _Location:_ `scripts/anonymize_traces.py` (new)
  _Complexity:_ Medium

### 9.2 Cache Pressure Scenarios

- [ ] **Add --unique-prefixes N option to control prefix diversity**
  _Justification:_ Current benchmarks use 5 unique prefixes. Real deployments may have thousands. Need to test cache behavior at scale.
  _Location:_ `tests/integration/locustfile_real.py`, `scripts/bench.sh`
  _Complexity:_ Low

- [ ] **Create "cache-pressure" prompt distribution**
  _Justification:_ Test behavior when unique prefixes exceed KV cache capacity. Validates eviction policies and degraded performance.
  _Approach:_ Generate N unique prefixes > vLLM block capacity, measure cache hit rate degradation curve
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add metrics to detect cache evictions (if vLLM exposes this)**
  _Justification:_ Understanding cache eviction rate helps tune cache size and routing policy.
  _Approach:_ Query vLLM `/metrics` for `vllm:cache_evictions_total` or similar, add to benchmark report. See also: [Push-based cache eviction notifications](docs/architecture/push-cache-eviction-notifications.md) for a design that goes beyond metrics to actively update Ranvier's routing state.
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Low

- [ ] **Document expected behavior when cache overflows**
  _Justification:_ Operators need guidance on capacity planning and expected degradation.
  _Location:_ `docs/benchmarks/benchmark-guide-8xA100.md`
  _Complexity:_ Low

### 9.3 Larger Models

- [ ] **Update bench.sh to support tensor-parallel vLLM across multiple GPUs**
  _Justification:_ 70B+ models require tensor parallelism. Current bench.sh assumes single-GPU vLLM instances.
  _Approach:_ Add `--tensor-parallel N` flag, adjust vLLM launch command, GPU allocation logic
  _Location:_ `scripts/bench.sh`
  _Complexity:_ Medium

- [ ] **Add recommended configurations for 70B (8x A100) and 405B (multi-node)**
  _Justification:_ Users need reference configurations for large model deployments.
  _Deliverables:_ Sample configs, memory requirements, expected TTFT ranges
  _Location:_ `docs/benchmarks/benchmark-guide-8xA100.md`, `docs/benchmarks/benchmark-guide-405B.md` (new)
  _Complexity:_ Medium

- [ ] **Adjust expected TTFT thresholds based on model size**
  _Justification:_ Current thresholds calibrated for 8B-13B models. Larger models have different latency profiles.
  _Location:_ `tests/integration/locustfile_real.py`, benchmark documentation
  _Complexity:_ Low

- [ ] **Document memory requirements and GPU configurations**
  _Justification:_ Operators need clear guidance on hardware requirements per model size.
  _Location:_ `docs/deployment/hardware-requirements.md` (new)
  _Complexity:_ Low

### 9.4 Traffic Variability

- [ ] **Add traffic shapes to locustfile: ramp-up, spikes, diurnal patterns**
  _Justification:_ Real traffic is not steady-state. Bursty traffic tests cache warm-up and backpressure behavior.
  _Approach:_ Custom `LoadTestShape` classes for different patterns
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add --traffic-pattern option (steady, bursty, ramp, spike)**
  _Justification:_ CLI option to select traffic shape without code changes.
  _Location:_ `scripts/bench.sh`, `tests/integration/locustfile_real.py`
  _Complexity:_ Low

- [ ] **Measure cold-start impact when traffic spikes after idle periods**
  _Justification:_ Production systems experience traffic spikes after quiet periods. Cache is cold, need to measure warm-up time.
  _Approach:_ Add idle period before spike, track cache hit rate over time, report warm-up duration
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add metrics for cache warm-up time and hit rate over time**
  _Justification:_ Time-series cache metrics help operators understand warm-up behavior and set appropriate scaling policies.
  _Approach:_ Periodic cache hit rate sampling (every 10s), export as CSV alongside benchmark report
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium


---

## 9. Load-Aware Prefix Routing
### 10.1 Prerequisites

- [ ] Confirm vLLM `/metrics` endpoint exposes queue depth (optional enhancement - not required)

### 10.4 Post-Implementation Checklist

- [ ] Run `validation/validate_v1.sh` (reactor stall detection)
- [ ] Run `tests/integration/test_load_aware_routing.py`
- [ ] Run benchmark: `scripts/bench.sh --users 30` (verify >35% TTFT improvement)
- [ ] Consider follow-up: hot prefix replication (Option 2 from issue)

> **Implementation Complete**: All 8 steps completed in PRs #222, #223, #224 (2026-02-05)


---

## 10. HTTP Controller Review (2026-02-14)
### 15.5 Optimize Per-Chunk Flush in SSE Streaming Loop

- [ ] **Reduce flush frequency in `stream_backend_response()` via timer-based coalescing**
  _Justification:_ Every `bundle.in.read()` iteration triggers `write()` + `flush()` to the client. At typical LLM token rates (30-100 tok/s), each token arrives as a separate TCP segment, so this is effectively **one `sendmsg()` syscall per token** at steady state. Natural TCP batching only helps during bursts (multiple tokens arriving in one segment, which `StreamParser::push()` concatenates into a single `res.data`). At high concurrency (many concurrent streams), the per-token syscall overhead becomes measurable.

  **Previous attempt (2026-02-14, reverted in #280):** Flushed only on first write (TTFT) and `[DONE]`, relying on Seastar's 8KB `output_stream` buffer for intermediate events. This caused a **3.3x TTFT regression** because small SSE events (10-50 bytes each) never fill the 8KB buffer, stalling delivery for seconds until enough tokens accumulate. The regression was caught in production load testing and reverted.

  **Why "just skip flushes" fails:** Seastar's `output_stream` buffers 8KB by default. At 30 bytes/token, that's ~270 tokens (~9 seconds at 30 tok/s) before the buffer auto-flushes. SSE requires real-time delivery, so any approach that removes intermediate flushes without a bounded-latency fallback will break streaming.

  **Why "reactor tick coalescing" is not straightforward:** Seastar provides no primitive to check "is there more work pending in this reactor tick." `seastar::yield()` / `coroutine::maybe_yield()` are control-flow primitives, not introspection tools. You cannot peek at a future's readiness without consuming it, so "read-ahead to decide whether to flush" requires restructuring the entire loop.

  1. On entering the streaming loop, arm a periodic timer (~10ms interval, matching `lowres_clock` resolution).
  2. In the read loop, call `co_await client_out.write(res.data)` **without** `flush()`.
  3. The timer callback flushes whatever has accumulated: `(void)client_out.flush().handle_exception(...)` with a `seastar::gate` holder per Rule #5.
  4. Flush **immediately** on `res.done` (terminal event) and on stream errors/cleanup.
  5. Disarm the timer on loop exit (before `client_out.close()`).

  - The timer is **per-request** (created/destroyed with each `stream_backend_response()` call). This is fine — Seastar's `timer_set` is optimized for frequent create/cancel cycles. See existing patterns: `router_service.cpp:2058` (batch flush), `connection_pool.hpp:500` (reaper timer).
  - The timer callback is synchronous but `flush()` is async. Use the fire-and-forget pattern with gate guard: `(void)flush().finally([holder = std::move(holder)] {})`.
  - Must handle the race between timer-initiated flush and loop-initiated close. The gate ensures the flush completes before stream teardown.
  - Must handle `flush()` failure in the timer callback (client disconnect). Log and set a flag the main loop checks, or let the next `write()` catch the broken pipe.
  - **10ms worst-case latency** is acceptable for SSE token streaming (human-imperceptible). This coalesces 1-3 tokens per flush at typical rates, reducing syscalls by ~2-3x.

  - **TTFT test:** Measure time from first backend token to client receipt. Must be <15ms above baseline (currently ~1ms with per-read flush).
  - **Stall test:** Stream 100+ tokens at 30 tok/s, verify no token is delayed >20ms from backend receipt to client delivery.
  - **Burst test:** Send 10 tokens in <1ms from backend, verify they arrive at client in one batch (not 10 separate deliveries).
  - **Disconnect test:** Client disconnects mid-stream, verify timer is cleaned up and no use-after-free.
  - **Shutdown test:** Server drains while streams are active, verify gate prevents timer callback after stream teardown.



---

## 11. Hot-Path Performance Audit (2026-02-15)
### 20.7 Jump Consistent Hash May Reduce Cache Affinity vs Modular Hash

- [ ] **Evaluate whether jump consistent hash improves or degrades prefix cache locality**
  _Justification:_ The modular hash was replaced with jump consistent hash (`router_service.cpp:517-532`, called at line 1221) for better distribution uniformity when backends are added/removed. Jump consistent hash (Lamping & Veach, 2014) provides minimal disruption on topology changes but produces a different mapping than the previous modular hash. This means existing prefix→backend affinities were reshuffled. The benchmark regression may partly reflect degraded cache locality if the new hash distributes related prefixes differently across backends. This is speculative — the hash should be equally good for cache affinity in steady state — but worth validating.
  _What to change:_ Run a controlled A/B test comparing jump consistent hash vs the previous modular hash with identical workloads. If the steady-state cache hit rate and XLarge TTFT improvement are equivalent, the hash change is not a factor. If they differ, investigate whether the hash function's output distribution interacts poorly with the specific prefix patterns in the benchmark workload.
  _Location:_ `src/router_service.cpp:517-532` (implementation), `1221` (call site)
  _Complexity:_ Low (testing only)
  _Priority:_ P3 — Investigation; unlikely to be a factor but easy to rule out

### 20.8 Client-Token Path Redundant JSON Parse

- [ ] **Optimize `extract_prompt_token_ids()` to avoid full JSON parse**
  _Justification:_ When clients send pre-tokenized `prompt_token_ids`, the `extract_prompt_token_ids()` function parses the entire request body with RapidJSON (`doc.Parse(body.data(), body.size())`) just to extract the token array. For large requests with long system prompts, this is a full JSON parse of potentially tens of KB. The body is also parsed again downstream for forwarding/rewriting, making this parse redundant. Benchmark shows 0.35ms on the client-token path — 34x faster than server-side tokenization (12ms) but still the dominant cost on this path.
  _What to change:_ Options ranked by impact:
  (a) **Share the parse**: Parse the body once early in the request handler, pass the `rapidjson::Document` to both `extract_prompt_token_ids()` and downstream rewriting. Eliminates one full parse.
  (b) **Targeted extraction**: Use a SAX-style or on-demand parser to scan only for the `"prompt_token_ids"` key without parsing the entire document.
  (c) **SIMD token validation**: Replace the per-element bounds-checking loop (lines 731-755) with SIMD range checks — process 8-16 token IDs per instruction instead of one-by-one branch-heavy iteration.
  _Location:_ `src/request_rewriter.hpp:693-760` (implementation), `src/http_controller.cpp:801` (call site)
  _Complexity:_ Low (option a), Medium (options b/c)
  _Priority:_ P3 — Performance; 0.35ms is already fast but becomes significant at high concurrency with client-side tokenization


---

## 12. Request Lifecycle Performance Analysis (2026-02-20)
### 21.4 Avoid Heap Copy of Text for Tokenizer Cache Insertion

- [ ] **Eliminate redundant `std::string` copy captured in thread-pool tokenization continuation**
  _Justification:_ At line 325, the thread-pool tokenization path captures `text_copy = std::string(text)` in the `.then()` continuation lambda solely for cache insertion after tokenization completes. For a typical 4KB request body, this is a guaranteed heap allocation on every cache-miss tokenization dispatched to the thread pool. With 80–90% cache hit rates for system messages, this primarily affects user messages — but those are exactly the requests that take the full 5–13ms tokenization path, adding allocation pressure to an already-expensive operation.
  _What to change:_ (a) Examine the cache's `insert()` and `lookup()` signatures. If the cache uses hash-based lookup internally, compute the hash before dispatch (on the reactor, which already has the `string_view`) and pass only the hash + a pre-hashed key to the continuation. (b) If the cache API requires the full string for insertion (stores the key), consider having the thread pool worker do the cache insertion (it already has a local copy of the text for FFI per Rule #15). Ensure the thread pool worker's cache access is shard-safe. (c) Alternatively, return the text copy from the thread pool submission alongside the tokens, reusing the copy already made for FFI safety. The goal is to avoid a second copy in the continuation.
  _Location:_ `src/tokenizer_service.cpp:324-339`
  _Complexity:_ Low
  _Priority:_ P2 — Performance; ~4KB heap allocation per cache-miss tokenization

### 21.5 Reduce `sstring` Reallocations in StreamParser

- [ ] **Pre-size the `_accum` buffer or switch to a chunk chain to reduce reallocations during streaming**
  _Justification:_ `StreamParser::push()` at line 57 appends each network chunk to `_accum` (a `seastar::sstring`). After exceeding the 15-byte SSO threshold (virtually every HTTP response), each append may trigger a reallocation + copy. A 100KB response in 50 chunks causes ~6–8 reallocations. The compaction at line 114 adds another `memmove` when read position exceeds 50% of the buffer, which is correct but doubles the copy work after each compaction cycle.
  _What to change:_ (a) During `parse_headers()` (line 133), if a `Content-Length` header is present, extract it and pre-size `_accum` to at least that value. This eliminates all reallocations for non-streaming (non-chunked) responses. (b) For chunked responses (no Content-Length), pre-size `_accum` to `StreamParserConfig::initial_output_reserve` (4096) on first push. (c) Note: `seastar::sstring` does not have `reserve()`. You may need to switch `_accum` to `std::string` (which does have `reserve()`), or use a `seastar::temporary_buffer<char>` chain that avoids copying entirely. (d) If switching to a chain, `compact_if_needed()` is replaced by dropping consumed chunks. Measure tradeoff: the chain avoids copies but complicates parsing when a token (e.g., `\r\n`) spans two chunks.
  _Location:_ `src/stream_parser.cpp:57`, `src/stream_parser.hpp`
  _Complexity:_ Medium
  _Priority:_ P3 — Performance; 6–8 reallocations per response, each O(N) copy

### 21.8 Build Backend HTTP Headers Without Intermediate String Allocations

- [ ] **Replace `sstring` concatenation with `fmt::format` or pre-reserved append for HTTP header construction**
  _Justification:_ At lines 425–437, backend request headers are built via repeated `sstring` concatenation (`+` operator). Each `+` allocates a new `sstring`, copies left and right operands, and frees the old buffer. For 6 concatenation steps, that is 5 intermediate allocations. This runs on every proxied request.
  _What to change:_ (a) Replace with `fmt::format()` (Seastar includes fmt) for a single allocation. (b) Handle the conditional `traceparent` header (line 433–435) — either include it in the format with a conditional, or append it separately. (c) Append `"Connection: keep-alive\r\n\r\n"` (line 437) in the same operation. (d) Note: `fmt::format` returns `std::string` — verify that `bundle.out.write()` accepts it or convert to `sstring`. No behavior change — same header order, same values.
  _Location:_ `src/http_controller.cpp:425-437`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; 5 temporary sstring allocations per request

### 21.9 Skip JSON Parse When Client Provides Tokens and Prefix Boundary

- [ ] **Avoid `extract_text_with_boundary_info()` JSON parse when client provides both tokens and prefix boundary**
  _Justification:_ At line 833, `extract_text_with_boundary_info()` is called unconditionally for all non-client-token requests. At line 953–957, it is called again if `text_extraction` was not populated (client-tokens path) and prefix boundary detection is enabled. The client-tokens path can still trigger a ~500µs JSON parse for boundary detection even though the client already provided both `prompt_token_ids` and `prefix_token_count`.
  _What to change:_ (a) In the client-tokens path, if `_config.enable_prefix_boundary` is enabled, eagerly call `extract_text_with_boundary_info()` after determining `used_client_tokens=true`, and cache the result in `text_extraction`. (b) Alternatively, add a fast path: if the client provides both `prompt_token_ids` AND `prefix_token_count` (already checked at line 935), skip the JSON parse entirely — both pieces of information needed for routing are already available. (c) The goal: when client provides both fields, zero JSON parsing should occur.
  _Location:_ `src/http_controller.cpp:795-957`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; ~500µs JSON parse wasted when client provides all needed data

### 21.10 Capture `lowres_clock::now()` Once Per Streaming Loop Iteration

- [ ] **Cache `lowres_clock::now()` at the top of the streaming loop to eliminate duplicate call**
  _Justification:_ At lines 476 and 487 in `stream_backend_response()`, `lowres_clock::now()` is called twice within 10 lines. `lowres_clock` updates at ~100Hz (10ms resolution), so consecutive calls return the same value. This repeats for every chunk in the streaming loop (potentially hundreds of chunks per response).
  _What to change:_ Add `auto now = lowres_clock::now();` at the top of the while loop body (before line 476). Replace both `lowres_clock::now()` calls with `now`. No behavioral change.
  _Location:_ `src/http_controller.cpp:474-488`
  _Complexity:_ Trivial
  _Priority:_ P4 — Cleanup; negligible performance impact, improves code clarity

---


---

## 13. Code Modularity (Low Priority)
### 22.1 Extract BackendRegistry Interface from RouterService

- [ ] **Decouple HealthService and K8sDiscoveryService from RouterService**
  _Justification:_ HealthService and K8sDiscoveryService take `RouterService&` directly. They only use `get_all_backend_ids()`, `get_backend_address()`, and `set_backend_status()`. Extracting a `BackendRegistry` interface makes both services independently testable without constructing a full RouterService.
  _What to change:_ Define a `BackendRegistry` abstract class with the three methods above. Have RouterService implement it. Change HealthService and K8sDiscoveryService constructors to accept `BackendRegistry&`.
  _Location:_ `src/health_service.hpp`, `src/k8s_discovery_service.hpp`, `src/router_service.hpp`
  _Complexity:_ Low
  _Priority:_ **P2** — Promoted; prerequisite for §15 Tier 2 (vLLM metrics ingestion)

### 22.2 Split config_schema.hpp into Infrastructure and Product Configs

- [x] **Separate generic infrastructure configs from routing-specific configs**
  _Justification:_ `config_schema.hpp` is 618 lines mixing infrastructure configs (ServerConfig, PoolConfig, HealthConfig, TlsConfig, AuthConfig, etc.) with Ranvier-specific configs (RoutingConfig, AssetsConfig). Splitting reduces cognitive load and makes infrastructure configs independently reusable.
  _What to change:_ Move infrastructure config structs to `config_infra.hpp`. Keep RoutingConfig and AssetsConfig in `config_schema.hpp`. RanvierConfig includes both headers.
  _Location:_ `src/config_schema.hpp`
  _Complexity:_ Low
  _Priority:_ **P1** — Promoted; prerequisite for §15 Tier 1 (cost estimation + priority configs)

### 22.3 Split MetricsService into Helpers and Ranvier-Specific Counters

- [x] **Extract generic histogram/counter patterns from Ranvier-specific metrics**
  _Justification:_ `metrics_service.hpp` is 620 lines mixing reusable patterns (MetricHistogram class, bucket definitions, per-backend metrics map) with Ranvier-specific counters (tokenization, ART, prefix boundary). Splitting makes the generic patterns reusable and reduces file size.
  _What to change:_ Move MetricHistogram, bucket helpers, and the bounded per-backend metrics pattern to `metrics_helpers.hpp`. Keep Ranvier-specific counters in `metrics_service.hpp`.
  _Location:_ `src/metrics_service.hpp`
  _Complexity:_ Low
  _Priority:_ **P1** — Promoted; prerequisite for §15 Tier 1 (per-priority metrics)

### 22.4 Template ShardedConfig on Config Type

- [ ] **Make ShardedConfig generic instead of hardcoded to RanvierConfig**
  _Justification:_ Trivial change (`ShardedConfig<T>` with backward-compatible alias `using ShardedConfig = BasicShardedConfig<RanvierConfig>`). Follows the existing pattern used by ConnectionPool, CircuitBreaker, and RateLimiter.
  _Location:_ `src/sharded_config.hpp`
  _Complexity:_ Trivial
  _Priority:_ P4 — Consistency with existing template patterns

### 22.5 Deduplicate router_service.cpp Compilation Across Test Targets

- [x] **Stop recompiling router_service.cpp (and its gossip/telemetry companions) once per test executable**
  _Justification:_ `router_service.cpp` is compiled five times per cold build — once into `libranvier_core.a` and once each for `router_service_test`, `cache_eviction_test`, `prefix_hash_index_lifecycle_test`, and `application_test`-class targets that list it as a source. Each instance costs ~1–2 GB of cc1plus RSS (Seastar + Abseil + coroutine headers), so a parallel cold build can OOM-kill the compiler on memory-constrained containers (observed 2026-07-03: `c++: fatal error: Killed signal terminated program cc1plus` during `make test`; retry succeeded once most objects existed). Every new RouterService-adjacent test suite makes this worse.
  _What to change:_ Link Seastar-dependent test executables against `ranvier_core` (or introduce a CMake `OBJECT` library for the shared test source set: router_service, telemetry_service, node_slab, gossip_*, crypto_offloader, dtls_context) instead of relisting the .cpp files per target. Verify no per-target compile-definition differences before consolidating; zero behavioral change intended (`/refactor` scope).
  _Location:_ `CMakeLists.txt` (test target definitions)
  _Complexity:_ Low–Medium (mechanical, but touches every Seastar-dependent test target)
  _Priority:_ P3 — Developer-experience/build-capacity; promote if cold-build OOMs recur in CI or dev containers
  _Completed:_ 2026-07-03 — Introduced a `router_test_support` CMake `OBJECT` library (router_service, telemetry_service, node_slab, gossip_{service,protocol,consensus,transport}, crypto_offloader, dtls_context) shared by `router_service_test`, `cache_eviction_test`, and `prefix_hash_index_lifecycle_test`, so the 9-file set compiles once for those three targets instead of three times (router_service.cpp: 5→3 total compiles). `application_test` deliberately keeps its own source list: it may define `RANVIER_DEBUG_METRICS` (which `gossip_service.cpp` keys off), so sharing a def-free object set would change its build under `-DRANVIER_DEBUG_METRICS=ON`. The sanitizer loop was extended to instrument the new object library so ASan/UBSan coverage of the shared TUs is unchanged. Zero behavioural change (`/refactor` scope).

---


---

## 14. Shard 0 Role Isolation Analysis (2026-03-06)
### 23.1 Exclude Shard 0 from P2C Candidate Pool

- [ ] **When cross-shard dispatch is implemented, exclude shard 0 from P2C candidate selection**
  _Justification:_ Shard 0 runs all control plane services (gossip consensus, K8s discovery, health probes). Under peak data plane load, these compete for reactor time, risking gossip heartbeat deadline misses and delayed health state propagation. Excluding shard 0 from P2C candidates protects control plane latency with minimal data plane capacity loss (~6% on 16-core).
  _Prerequisite:_ Backlog item 15.1 Option A (implement actual cross-shard dispatch via `smp::submit_to`). Until then, this change has no effect since P2C is advisory-only.
  _What to change:_ In `shard_load_balancer.hpp`, modify `select_shard_p2c()` candidate generation to exclude shard 0: change `std::uniform_int_distribution<uint32_t> dist(0, _shard_count - 1)` to `dist(1, _shard_count - 1)`. Alternatively, add a configurable `_excluded_shards` set for flexibility. Also update `select_shard_async()` candidate generation to match.
  _Location:_ `src/shard_load_balancer.hpp` (lines 114, 164-169)
  _Complexity:_ Low
  _Priority:_ P2 — Deferred until cross-shard dispatch ships

### 23.2 Hard Shard 0 Isolation (Future — Evidence-Gated)

- [ ] **If shard 0 reactor utilization consistently exceeds 80% while other shards are below 50%, isolate shard 0 from data plane entirely**
  _Justification:_ In large clusters (8+ nodes, 20+ backends), control plane work becomes non-trivial: gossip protocol traffic scales with cluster size, health probes scale with backend count, K8s watch reconnects can spike during topology changes. If shard 0 becomes a bottleneck, full isolation prevents cascading degradation.
  _Evidence trigger:_ Monitor `seastar_reactor_utilization` on shard 0 vs other shards under production load. Only proceed if sustained asymmetry is observed.
  _What to change:_ In `application.cpp:697-699`, conditionally skip data plane route registration on shard 0. Keep `/health` endpoint accessible on shard 0 (or move to all shards). May require Seastar `SO_INCOMING_CPU` tuning to prevent OS from dispatching TCP connections to shard 0.
  _Location:_ `src/application.cpp` (lines 697-699), `src/http_controller.cpp` (route registration)
  _Complexity:_ Medium
  _Priority:_ P3 — Only if evidence warrants; premature on clusters <8 nodes or <16 cores
  _Tradeoff:_ Permanently loses 1/N data plane capacity (25% on 4-core, 12% on 8-core, 6% on 16-core). Shard 0 would be mostly idle between control plane bursts — wasted core on small deployments.

---

## 15. Intelligence Layer Roadmap (2026-03-25)

Prioritized implementation plan derived from [VISION.md](docs/architecture/VISION.md).
Chassis refactors (§13) are interleaved where they prevent rework on shared files.

### Tier 1: Foundation (do now)

- [x] **[P1] Split config_schema.hpp** (§13 item 22.2, promoted from P4)
  _Why now:_ Tier 1 features add CostEstimationConfig + PriorityQueueConfig. Splitting infra vs product configs first avoids a second move.
  _Location:_ `src/config_schema.hpp`
  _Complexity:_ Low

- [x] **[P1] Split metrics_service.hpp** (§13 item 22.3, promoted from P4)
  _Why now:_ Tier 1 features add per-priority metrics. Same logic.
  _Location:_ `src/metrics_service.hpp`
  _Complexity:_ Low

- [x] **[P1] Cost Estimation + Priority Tiers (VISION 1.1+1.2 merged)**
  _Effort:_ ~2.5 weeks
  _Scope:_ ProxyContext cost fields, input_tokens as initial cost proxy, PriorityLevel enum, PriorityQueue, X-Ranvier-Priority header, per-priority metrics. Heuristic decay deferred to v1.1.
  _Files:_ `src/http_controller.{hpp,cpp}`, `src/config_schema.hpp`, `src/metrics_service.hpp`
  _Dependencies:_ config + metrics splits above
  _Complexity:_ High

- [x] **[P1] Intent Classification (VISION 1.4)**
  _Effort:_ ~1 week
  _Scope:_ RequestIntent enum (AUTOCOMPLETE/CHAT/EDIT), FIM detection, wire-format inspection, intent-based routing hints, per-intent metrics.
  _Files:_ `src/http_controller.{hpp,cpp}`, `src/config_schema.hpp`, `src/config_loader.cpp`, `src/metrics_service.hpp`, `src/application.cpp`
  _Dependencies:_ 1.1+1.2
  _Complexity:_ Medium
  _Completed:_ 2026-03-28 — §15 Tier 1 fully complete.

### Tier 2: Cloud Intelligence

- [x] **[P2] Extract BackendRegistry interface** (§13 item 22.1, promoted from P4)
  _Why now:_ vLLM metrics ingestion wires into HealthService. Decoupling from RouterService first avoids deepening the coupling.
  _Location:_ `src/health_service.hpp`, `src/k8s_discovery_service.hpp`, `src/router_service.hpp`
  _Complexity:_ Low
  _Completed:_ 2026-03-31

- [x] **[P2] vLLM Metrics Ingestion (VISION 2.1)**
  _Effort:_ ~2 weeks
  _Scope:_ VLLMMetrics struct, /metrics scraping, Prometheus text parsing.
  _Files:_ `src/health_service.{hpp,cpp}`, `src/metrics_service.hpp`
  _Dependencies:_ BackendRegistry interface
  _Complexity:_ High
  _Completed:_ 2026-03-31

- [x] **[P2] Load-Aware Backend Selection (VISION 2.2)**
  _Effort:_ ~2 weeks (~30% infra exists from load-aware prefix routing)
  _Scope:_ P2C alternative selection, routing decision logic, thundering herd prevention.
  _Dependencies:_ 2.1
  _Complexity:_ High
  _Completed:_ 2026-03-31

- [x] **[P2] Cost-Based Routing (VISION 2.3)**
  _Effort:_ ~1.5 weeks
  _Scope:_ Per-backend cost budget, small-request fast lane, budget reservation/release.
  _Dependencies:_ 1.1+1.2, 2.2
  _Complexity:_ Medium
  _Completed:_ 2026-03-31 — §15 Tier 2 fully complete.

- [ ] **Re-benchmark: prefix + priority + load-aware vs baseline**

### Tier 3: Local Product

- [x] **[P2] Local Mode Config (VISION 1.3)**
  _Effort:_ ~1 week
  _Scope:_ LocalModeConfig, RANVIER_LOCAL_MODE env, conditional startup.
  _Files:_ `src/config_schema.hpp`, `src/application.cpp`
  _Complexity:_ Low

- [x] **[P2] Local Backend Discovery (VISION 3.1)**
  _Effort:_ ~1.5 weeks
  _Scope:_ Port scanning, semantic liveness, server type detection.
  _Dependencies:_ 1.3
  _Complexity:_ Medium

- [x] **[P2] Agent-Aware Request Handling (VISION 3.2)** ✓ Done (2026-03-29)
  _Effort:_ ~2 weeks
  _Scope:_ AgentRegistry, agent identification, pause/resume API.
  _Dependencies:_ 1.2, 1.4
  _Complexity:_ High

- [x] **[P3] Request Queuing with Pause/Resume (VISION 3.3)** ✓ Done (2026-03-29)
  _Effort:_ ~2 weeks | Risk: High
  _Scope:_ RequestScheduler pause-aware dequeue, per-agent queue depth limits, admin API stats.
  _Dependencies:_ 3.2
  _Complexity:_ High

### Tier 4: Polish (parallel, after Tier 3)

- [ ] **[P3] Single-Binary Local Distribution (VISION 4.1)**
  _Complexity:_ Medium

- [x] **[P3] Local Dashboard UI (VISION 4.2)** ✓ Done (2026-04-04)
  _Complexity:_ High
  _Delivered:_ Single-page dashboard at localhost:9180/dashboard with backend status, queue depths, agent table with pause/resume, and throughput stats. HTML embedded in binary via CMake. CORS gated behind DashboardConfig (auto-enabled in local mode).

- [x] **[P3] Documentation & Examples (VISION 4.3)** ✓ Done (2026-04-04)
  _Complexity:_ Low
  _Delivered:_ Getting Started (Local), Cloud Deployment Guide, IDE Integration Guide, Benchmark Reproduction Guide. README updated with Quick Start and Documentation sections.

### §15 Completion Note (2026-04-04)

**The Intelligence Layer Roadmap is complete.** All four tiers delivered:
- **Tier 1 (Foundation):** Cost estimation, priority tiers, intent classification — 2026-03-28
- **Tier 2 (Cloud Intelligence):** vLLM metrics, load-aware routing, cost-based routing — 2026-03-31
- **Tier 3 (Local Product):** Local mode, backend discovery, agent-aware handling, pause/resume — 2026-03-29
- **Tier 4 (Polish):** Documentation & examples, local dashboard UI — 2026-04-04

Remaining Tier 4 item (4.1 single-binary distribution) is deferred — not blocking the v1.0 release.

### Dashboard v2 (future improvements)

**Next iteration:**
- [ ] **[P2] Rolling request rate** — replace average-over-uptime req/s with a sliding window (60s ring buffer in `/dashboard/stats`). Current metric flatlines quickly and doesn't reflect real-time throughput.
  _Complexity:_ Low (add ring buffer to stats handler, ~50 lines)
- [ ] **[P2] Error breakdown** — split "Errors: N" into timeouts, connection errors, circuit breaker rejections. MetricsService already tracks these separately (`_requests_timeout`, `_requests_connection_error`, `_circuit_opens`); just expose via `/dashboard/stats`.
  _Complexity:_ Low (add fields to stats JSON, update dashboard JS)
- [ ] **[P3] Backend detail** — show circuit breaker state (closed/open/half-open) and active connections per backend. HealthService already tracks vLLM GPU metrics that could surface here too.
  _Complexity:_ Medium (need to expose circuit breaker state via admin API or new endpoint)

**Later:**
- [ ] **[P3] Cross-shard stats aggregation** — `/dashboard/stats` currently returns shard-0 counters only (correct for `--smp 1`). Multi-core needs `smp::invoke_on_all` to sum across shards, requiring an async handler instead of the sync `function_handler`.
  _Complexity:_ Medium (async handler pattern change + cross-shard summation)
- [ ] **[P4] WebSocket push** — replace 5s polling with push to eliminate the blind spot for transient queue depth spikes. Seastar has WebSocket support.
  _Complexity:_ High (new protocol, connection lifecycle management)
- [ ] **[P3] Request log tail** — last N requests with latency, status, backend, agent. Needs a bounded ring buffer per shard.
  _Complexity:_ Medium (bounded ring buffer + new endpoint + dashboard panel)
- [ ] **[P4] Model inventory per backend** — local discovery detects server type; could fetch `/api/tags` from Ollama to show available models on each backend.
  _Complexity:_ Medium (discovery service changes + dashboard UI)

### Chassis items deferred (no urgency)

- [ ] **[P4] Template ShardedConfig** (§13 item 22.4) — trivial, do whenever
- [ ] **[P4] Generalize gossip message types** — only when 2nd product committed

---

## 16. KV-Cache Compression-Aware Routing (2026-04-05)

Design exploration for making Ranvier's routing decisions aware of heterogeneous backend KV-cache capacity — motivated by Google's TurboQuant (~6x KV-cache compression, ICLR 2026), but applicable to any fleet where backends differ in effective cache capacity.

**Full proposal:** [`docs/architecture/kv-cache-compression-integration.md`](docs/architecture/kv-cache-compression-integration.md)

| Priority | Item | Complexity | TurboQuant-specific? |
|----------|------|------------|---------------------|
| P0 | Compression-aware load scoring (`load_score()` formula) | Low | No |
| P1 | Effective capacity in cost-based routing | Low | No |
| P1 | Fleet-wide cache efficiency metrics | Low | No |
| P2 | Capacity-aware hash fallback selection | Medium | No |
| P2 | Compression-aware route TTL | Low | No |
| P3 | Tiered compression signaling (Ranvier → backend) | High | Partially |

Core enabler: `compression_ratio` field in `BackendInfo` (static config initially, scraped later).

---

## 17. Hard Rules Audit Follow-ups (2026-05-05)

**Status: CLOSED (2026-05-17).** All sub-tickets resolved.

Closure narrative — per-rule fix tickets for the Rules 2/12/13/20 doc-correction pass on 2026-05-05, plus two P3 optional follow-ups (CI grep guardrail for `seastar::async(` and a concurrency cap on the gossip-broadcast hot path) that closed alongside the audit on 2026-05-17 — extracted on 2026-05-17 to a dedicated audit doc to keep this backlog focused on active work:

- [`docs/audits/hard-rules-audit-followups-2026-05-05.md`](docs/audits/hard-rules-audit-followups-2026-05-05.md)
  — per-finding resolutions (this section's history).
- [`.dev-context/claude-context.md`](.dev-context/claude-context.md)
  — canonical Hard Rules wording the audit followed (Rules 2/12/13/20 corrections were made there on 2026-05-05).

The section heading and anchor (`#17-hard-rules-audit-follow-ups-2026-05-05`) are preserved here as a stable pointer for the cross-references in the audit doc.

---

## 18. Request Lifecycle Crash-Risk Audit Follow-ups (2026-05-08)

**Status: CLOSED (2026-05-16).** All sub-tickets resolved.

Closure narrative — per-finding fix tickets, sanitiser-suite bring-up
(§18 P2), Seastar-default-allocator fuzz unblock (§18 P3), and all
spin-out CI/workflow tooling — extracted on 2026-05-17 to a dedicated
audit doc to keep this backlog focused on active work:

- [`docs/audits/request-lifecycle-empirical-followups-2026-05-08.md`](docs/audits/request-lifecycle-empirical-followups-2026-05-08.md)
  — empirical follow-ups (this section's history).
- [`docs/audits/request-lifecycle-crash-audit.md`](docs/audits/request-lifecycle-crash-audit.md)
  — static crash-risk audit that produced the findings.

The section heading and anchor (`#18-request-lifecycle-crash-risk-audit-follow-ups-2026-05-08`)
are preserved here as a stable pointer for the cross-references in the
audit doc.

---

## 19. Heterogeneous Backend Support (2026-05-16)

**Status: CLOSED (2026-05-17).** All sub-tickets resolved.

Closure narrative — the per-sub-section design, ticket plan, and individual resolution blocks (`BackendType` enum + lifecycle plumbing, ART-learn gate, vLLM `/metrics` scrape opt-out, static-config YAML schema with env-var-resolved API keys, K8s annotation + Secret resolution path, operator documentation) — extracted on 2026-05-17 to a dedicated audit doc to keep this backlog focused on active work. Also captures the post-closure framing-accuracy correction (Cerebras KV-cache vs managed-API-opacity rationale).

- [`docs/audits/heterogeneous-backend-support-2026-05-16.md`](docs/audits/heterogeneous-backend-support-2026-05-16.md)
  — per-ticket resolutions, design notes, and the framing correction (this section's history).
- [`docs/guides/hybrid-fleets.md`](docs/guides/hybrid-fleets.md)
  — operator-facing walkthrough produced by §19.5 (YAML + K8s paths, credential rules, rotation procedure, observability metrics).
- [`docs/internals/prefix-affinity-routing.md`](docs/internals/prefix-affinity-routing.md#backend-type-applicability)
  — internal per-`BackendType` applicability table produced by §19.5 (which types learn / scrape / forward `prompt_token_ids`, with codebase pointers to the enforcement sites).

The section heading and anchor (`#19-heterogeneous-backend-support-2026-05-16`) are preserved here as a stable pointer for the cross-references in `src/types.hpp`, `src/router_service.cpp`, `src/k8s_discovery_service.hpp`, `src/sqlite_persistence.cpp`, and the unit tests.

---

## 20. Routing Parity & Ecosystem Alignment (2026-06-07)

**Status: CLOSED (2026-06-14).** All actionable items (§20.1 P0.x, §20.2 P1.x) shipped and merged. §20.3 was P2 / out of scope.

Closure narrative — the per-item completion notes for the unified weighted scorer (P0.2), disaggregated prefill/decode pool roles (P0.3), native vLLM KV-event mode (P0.1, two PRs), OpenTelemetry GenAI semantic conventions (P1.6), the usage-ledger sink seam (P1.5), and the GIE Endpoint-Picker ext_proc mode (P1.4, two PRs) — plus the cross-cutting engineering decisions, lessons, and deferred follow-ups — were extracted on 2026-06-14 to a dedicated audit doc to keep this backlog focused on active work.

- [`docs/audits/routing-parity-ecosystem-alignment-2026-06-14.md`](docs/audits/routing-parity-ecosystem-alignment-2026-06-14.md)
  — per-item end state, cross-cutting decisions/lessons, and deferred follow-ups (this section's closeout record).
- [`docs/architecture/routing-direction-2026.md`](docs/architecture/routing-direction-2026.md)
  — the planning rationale and ecosystem context the work derived from.

Deferred follow-ups (recorded in the retrospective; would be promoted to new backlog sections if prioritised): GIE EPP 429 request-shedding, the inline-vs-sidecar ext_proc overhead benchmark, and per-model chat-template selection. The §20.3 P2 candidates (semantic/embedding cache, KV-offload awareness, guardrails hook, multi-provider/model routing) remain out of scope.

The section heading and anchor (`#20-routing-parity--ecosystem-alignment-2026-06-07`) are preserved here as a stable pointer for the in-source cross-references in `src/route_scorer.hpp`, `src/router_service.cpp`, `src/types.hpp`, `src/k8s_discovery_service.hpp`, `src/sqlite_persistence.cpp`, `src/gie_epp_server.hpp`, `src/http_controller.cpp`, and `src/application.cpp` (and the §20-referencing internals docs).

---

## 21. Cache-Aware Autoscaling Telemetry (2026-06-18)

Export the prefix→replica and per-prefix-hotness signals an external cache-aware
autoscaler needs to (a) drain the *coldest* replica rather than a hot one, and
(b) avoid reaping the last warm holder of a hot prefix.

**Status: P0–P3 CLOSED (P0–P2 2026-06-19; P3 / Phase 5 2026-06-20), incl. both P2 carve-outs.**

P0–P2 shipped and merged across three layers: the observability foundation
(bounded `StreamSummary` top-K + per-backend prefix hit/attempt gauges); the
node-level view (resident-route gauge, hot-prefix top-K as a `TelemetryService`
window aggregate + concentration gauges, and the auth-gated `GET /v1/cache/topology`
JSON); and the cluster-wide sole-holder path (`HOT_PREFIX_DIGEST` 0x07 gossip →
shard-0 `CacheTopologyIndex` with peer-death eviction + TTL age-out and per-window
self-apply, surfaced as `holders`/`sole_held` per JSON entry plus the
`ranvier_sole_held_hot_prefixes` and `hot_prefix_sole_held_request_share` gauges).
Both P2 carve-outs also shipped: the DEGRADED-quorum freeze (during split-brain
`sole_held` never reads `false` — fail-safe toward "do not reap") and the
`enable_cache_topology` dark-launch flag (default off; when off the cluster
sole-holder surface is omitted and the endpoint reverts to its P1 shape). The
decision spike (K=128; gauges-not-counters; reuse `TelemetryService` rather than
a new aggregator) and the per-slice as-built record were folded into the
architecture doc on closeout to keep this entry focused on active work.

P3 (Residency-verified holders, Phase 5) shipped and merged in 4 sub-phases (PRs
#587–#590): a shard-0 `RouterService::verified_resident_subset` seam intersects our
hot top-K with the native-KV `prefix_hash_index` residency mirror (5a); the subset
rides `HOT_PREFIX_DIGEST` v2 as a bitmap tail alongside the unchanged membership
digest (B-as-superset, 5b); `CacheTopologyIndex` records a two-state `(member,
verified)` holder tier (5c); and `verified_holders`/`verified_sole_held` surface in
the JSON plus the `ranvier_sole_held_verified_hot_prefixes` gauge, with the
DEGRADED-quorum freeze applied to the verified tier too (5d).

With P3 landed, `verified_sole_held` is the **accuracy-gated** signal an autoscaler
can consume for *automated* reaping: it counts nodes that verify a prefix RESIDENT
in their own KV cache (native trust), not merely route it, closing the
route-membership false-negative. The membership `sole_held` is unchanged and stays
the broader operator-observability signal. Both remain gated by the
`enable_cache_topology` dark-launch flag and the split-brain freeze.

**Scope boundary:** Ranvier *exports* telemetry only. The autoscaler (reap/pin
policy, replica lifecycle) lives in the external platform — reap-gating semantics
in particular are the scaler's decision, not Ranvier's.

- [`docs/architecture/cache-aware-autoscaler-telemetry.md`](docs/architecture/cache-aware-autoscaler-telemetry.md)
  — design rationale, signal inventory, the sole-holder asymmetry + 5-phase plan,
  and the **Implementation notes (as-built)** closeout record (spike findings,
  the `TelemetryService` course-correction, Hard-Rule watch).
- [`docs/benchmarks/cache-topology-telemetry-overhead.md`](docs/benchmarks/cache-topology-telemetry-overhead.md)
  — routing-overhead microbench (`touch()` 2.3 ns warm / 72 ns evict @ K=128;
  `hash_prefix` ~0.4 µs @128 tok, ~2 µs @512).

### Still open

| Priority | Item | Notes |
|----------|------|-------|
| Release gate | End-to-end A/B (telemetry on vs off) | Not yet run (not runnable in sandbox). Single-stream through one ingress; primary signal is `ranvier_router_routing_latency_seconds` p50/p99 delta **< 1%** with no p99 spike at the shard-0 window cadence. Wrap as `make bench-cache-topology` (per `bench-epp`); promote method + recorded floor to the benchmark doc when first run. |
| Optional | `cluster.residency_filter_membership` knob (Phase 5 follow-up) | Deferred, default off. Filters the membership digest to resident hashes before broadcast (the old "A"); needs no wire change since the full set is already transmitted. Only worth it for homogeneous native-KV fleets that want smaller digests. |

Deferred design note: `StreamSummary` keeps its O(K) min-scan eviction (72 ns @
K=128, far under the Rule #17 quota); upgrade to the O(1) bucket-list Space-Saving
variant only if K rises by orders of magnitude.

Bounding the fingerprint hash (originally scoped "P3, Low" — **reclassified
2026-06-19 after investigation: NOT a quick win**). The fingerprint *is* already
bounded on the fallback path (`prefix_len = min(len, prefix_token_length)`, default
128 ≈ 0.4 µs); the only uncapped cost is the `prefix_boundary` branch
(`router_service.cpp`), which hashes the full declared/detected prefix — and that
is **intentional**: it hashes the entire system message so requests sharing it
co-locate for cache reuse. A cap there is correctness-sensitive, not free: (1) it
degrades routing precision (distinct prompts sharing the first N tokens collide);
(2) the KV-event ledger's boundary-identity invariant requires any cap to be a
`block_alignment` multiple applied consistently, else native-KV route-learning
stops matching routing lookups; (3) lookup, both learn paths, and the
`X-Ranvier-Prefix-Hash` header must use the same effective depth or hits silently
become misses. If ever pursued, do it as an opt-in `prefix_hash_max_tokens` config
(default = current/unbounded), not a default change. The ~2 µs figure only arises
when an operator runs a large `prefix_token_length`/system prompt — an explicit
precision-vs-cost choice. See the architecture doc's as-built notes.

Section anchor (`#21-cache-aware-autoscaling-telemetry-2026-06-18`) is the stable
pointer for future in-source `// BACKLOG §21` cross-references.

---

## 22. Invariant Audit Findings (2026-07-03)

**Findings: CLOSED (2026-07-04).** All eight (I-1 HIGH unpruned prefix_hash_index
through I-8 LOW counter drift) fixed, merged, and verified; the one hardening
follow-up (V-1) is open below. Semantic-correctness pass over the routing hot path;
this audit also seeded the living invariant catalog `.dev-context/invariants.md`.

Detail lives in the frozen audit report, kept out of this backlog so it stays focused:

- [`.dev-context/invariant-audit-2026-07-03.md`](.dev-context/invariant-audit-2026-07-03.md)
  — findings I-1…I-8 with fix prompts, the "checked and found sound" record, and the
  2026-07-04 verification pass (per-finding disposition, PRs, tests; catalog flips).
- [`.dev-context/invariants.md`](.dev-context/invariants.md) — the invariant catalog
  (T/R/P/L/A entries) this audit seeded; the status source of truth going forward.

Open follow-up:

- [ ] [LOW] Harden: the I-1 index rebuild preserves ALL entries of fresh-native-stream backends without provenance or cap — stale learn-derived entries for a continuously-fresh backend persist until the stream lapses. Enforce `MAX_ENTRIES_PER_BACKEND` during preservation (drop + overflow counter), or add a per-entry provenance bit. (V-1, verification pass 2026-07-04 appended to the audit report)

The section heading and anchor (`#22-invariant-audit-findings-2026-07-03`) are
preserved as the stable pointer for in-source `// BACKLOG §22` cross-references.

---

## 23. Holistic Audit Findings (2026-07-04)

**Violations: CLOSED (2026-07-04).** All nine (V1 HIGH gossip-ACK dangling ref through
V9 doc-sync) fixed and merged; the nine tech-debt items below are separate and mostly
open. Hard Rules / async-integrity / drift pass over the post-February churn not
covered by the invariant audit (application, telemetry, config, metrics_service,
gossip_protocol, http_controller delta).

Detail lives in the frozen audit report, kept out of this backlog so it stays focused:

- [`.dev-context/holistic-audit-2026-07-04.md`](.dev-context/holistic-audit-2026-07-04.md)
  — compliance summary, V1–V9 findings, the "checked and found sound" record, and the
  2026-07-05 Resolution note (per-violation disposition, PRs, tests).

Technical debt (from the same audit — folded into whatever PR next touches each file;
`ApiKey::is_expired()` re-parse was resolved together with V3):

- [ ] [TECH-DEBT] telemetry_schema.hpp carries non-trivial logic (merge_hot_prefixes, sole-held math, JSON emitter, lines 284-431) in a *_schema.hpp; move helpers out (from audit 2026-07-04)
- [ ] [TECH-DEBT] http_controller handle_proxy detects non-streaming via `"stream":false` substring sniff — use the single-parse extraction (from audit 2026-07-04)
- [ ] [TECH-DEBT] `GossipProtocol::next_seq_num` at-capacity eviction sweeps up to MAX_DEDUP_PEERS=10000 entries synchronously on the broadcast path; amortize (Rule #17 marginal) (from audit 2026-07-04)
- [ ] [TECH-DEBT] `TelemetryService::snapshot_and_reset` iterates up to the 65536 max_buckets ceiling with no yield; mirror the emit merge loop's yield or lower the ceiling (from audit 2026-07-04)
- [ ] [TECH-DEBT] `Application::load_persisted_state` replays routes with sequential `co_await learn_route_global` per record (startup-only; O(routes × shards) cold start); batch (from audit 2026-07-04)
- [ ] [TECH-DEBT] application.cpp includes `<fstream>` with no use — stale include invites Rule #12 misuse (from audit 2026-07-04)
- [ ] [TECH-DEBT] reload/init paths capture whole config structs by value into invoke_on_all lambdas (cross-shard free, accepted D4 posture) — consider a foreign_ptr broadcast helper (from audit 2026-07-04)
- [ ] [TECH-DEBT] `setup_signal_handlers` fire-and-forget reload broadcast lacks a gate; route through `_lifecycle_gate` (Rule #18 hygiene) (from audit 2026-07-04)

Section anchor (`#23-holistic-audit-findings-2026-07-04`) is the stable pointer for
future in-source `// BACKLOG §23` cross-references.

---

## 24. Adversarial Audit Findings — Pass A (2026-07-04)

**Status: CLOSED (2026-07-05).** All findings (A1/A2 decoder timestamp UB, S1 EPP
ingress backpressure, S2 decoder reserve) fixed and merged. Edge-case/scale sweep of
the unfuzzed untrusted surfaces (vLLM KV-event msgpack decoder, its ZMQ subscriber,
the build-gated GIE EPP gRPC bridge).

Detail lives in the frozen audit report (findings + per-finding resolution note),
kept out of this backlog so it stays focused on active work:

- [`.dev-context/adversarial-audit-2026-07-04.md`](.dev-context/adversarial-audit-2026-07-04.md)
  — findings A1/A2/S1/S2, the "checked and found sound" record, and the
  2026-07-05 Resolution note (per-finding disposition, PRs, tests).

Durable artifact: `tests/fuzz/kv_event_decoder_fuzz.cpp` — the KV-event decoder's
first fuzz coverage, wired into `fuzz-ci` / `fuzz-run-all` and the CMake
`fuzz_harnesses` target, running on every push.

The section heading and anchor (`#24-adversarial-audit-findings-pass-a-2026-07-04`)
are preserved as the stable pointer for in-source `// BACKLOG §24` cross-references.

---

## 25. Benchmark Tooling P0 — Re-anchor the Truth (2026-07-06)

Source: [`.dev-context/benchmark-tooling-review-2026-07-05.md`](../.dev-context/benchmark-tooling-review-2026-07-05.md),
recommended plan **P0 — Re-anchor the truth (cheap; mostly no GPU time)**. Addresses
findings F1 (headline anchored to deprecated 5-prefix workload), F2 (5-prefix trap live
everywhere except `bench.sh`), and the doc half of F6 (deprecated scripts still shipped).

**Scope note — no Seastar / hot-path surface.** Every step touches Python (locust harness),
Bash (wrapper scripts), or Markdown (docs) only. No C++, no cross-shard `submit_to`, no new
container/timer/gate, no `router_service`/`http_controller`/`request_rewriter` change → the
`claude-locust-sync-map.md` server↔locust contract is **not** affected (we change default
*values*, not the request shape). No Deferred Gate build is required for correctness; the
verification ladder is lint + a dry-run of the affected commands, not `make` (see below).

**Verified pre-conditions (2026-07-06):**
- CI (`benchmark.yml:197`) and `make benchmark` (`Makefile:588`) run the **mock**
  `locustfile.py`, which has **no** `NUM_LARGE_PREFIXES` → changing `locustfile_real.py`'s
  default is **CI-safe** and cannot shift the gated mock p99/throughput baseline. Only
  real-GPU runs are affected.
- Both deprecated scripts already print a `[DEPRECATED]` banner but still **execute**
  (`run-multi-gpu-benchmark.sh:48`, `setup-lambda-benchmark.sh:43`); the latter
  heredoc-generates a repo-root `run-benchmark.sh` (`setup-lambda-benchmark.sh:113-114`)
  that name-collides with `tests/integration/run-benchmark.sh`. Both are referenced in
  `scripts/README.md` and `tests/integration/README.md`.

### Prerequisites
- [x] Read `locustfile_real.py`, `run_benchmark_comparison.py`, `bench.sh`,
  `bench-residency-ab.sh`, the two deprecated scripts, and the guide's section map.
- [x] Confirm the mock-CI decoupling above.
- [ ] Two open decisions to confirm before implementing (see **Decisions** at end).

### Implementation Steps

#### Step 1: Kill the 5-prefix default in the locustfile (single source of truth)
- **Files:** `tests/integration/locustfile_real.py`
- **Description:** `NUM_LARGE_PREFIXES` default `5→50` (line 592); `SHARED_PREFIX_RATIO`
  default `0.7→0.9` (line 634). Add a comment on each mirroring the pigeonhole rationale
  already in `bench.sh:1665-1670` (with 8 backends, 5 prefixes ≤ backend count pins all
  prefixes under pure affinity → manufactured false regression; 50 ≥ backend count is
  representative). This makes the locustfile the authoritative default so every direct
  `locust`/harness invocation stops silently reproducing the deprecated workload.
- **Concerns:** None Seastar. Churn knobs (200/24/20/8/seed 42) are already the intended
  defaults — leave them; they become authoritative once wrappers stop shadowing them (Step 3).
- **Done:** `NUM_LARGE_PREFIXES = int(... "50")`, `SHARED_PREFIX_RATIO = float(... "0.9")`.
- **Status:** [x] Done (2026-07-06)

#### Step 2: Kill the 5-prefix default in the comparison driver
- **Files:** `tests/integration/run_benchmark_comparison.py`
- **Description:** `--num-prefixes` default `5→50` (line 639) + help text; fix the module
  docstring example (line 38, `--num-prefixes 5`) and the reporting fallbacks
  (`config.get('num_prefixes', 5)` line 314). The driver already forwards
  `NUM_LARGE_PREFIXES` (line 702) but does **not** set `SHARED_PREFIX_RATIO` → it now inherits
  the locustfile's 0.9 automatically (desired). Decision: do **not** add a `--prefix-ratio`
  flag here — inheriting keeps the single source of truth (revisit under P1/P2 if a sweep
  needs it).
- **Concerns:** `make benchmark-comparison` uses this driver against real vLLM only — no CI gate.
- **Done:** default 50 everywhere in the file; help/docstring/report consistent.
- **Status:** [x] Done (2026-07-06)

#### Step 3: Make `bench.sh` a pass-through, not a second source of defaults
- **Files:** `scripts/bench.sh`
- **Description:** Today `bench.sh` *always* injects `-e NUM_LARGE_PREFIXES=${…:-50}` and the
  churn/ratio knobs with hardcoded defaults that **duplicate** the locustfile (lines
  1671/1675 main path, 1838/1842 warm-up path; `SHARED_PREFIX_RATIO` at 1609/1716/1851).
  Change to inject `-e VAR=…` **only when the caller explicitly set the var** (`[[ -n "$VAR" ]]`),
  otherwise omit it and let the locustfile default apply. Keep the effective-config banner
  (1441-1446) — relabel its fallback from `50 (bench default)` to `50 (locust default)` and
  keep the pigeonhole warning (1442-1443) working against the resolved value. This removes the
  drift class where the two files' defaults silently diverge.
- **Concerns:** Preserve `>&2` discipline and the `$()`-capture return pattern in the touched
  `run_benchmark()` / warm-up blocks (do not de-duplicate the warm-up block here — that's F6/P2).
  The banner keeps ONE labeled fallback constant with a `# must match locustfile_real.py` comment
  (accepted minor duplication, for auditability).
- **Done:** grep shows no unconditional `-e NUM_LARGE_PREFIXES=`/`-e CHURN_*=`/`-e SHARED_PREFIX_RATIO=`
  with a hardcoded default; banner still prints effective values.
- **Status:** [x] Done (2026-07-06)

#### Step 4: Same pass-through treatment for `bench-residency-ab.sh`
- **Files:** `scripts/bench-residency-ab.sh`
- **Description:** The residency A/B wrapper `export`s churn knobs with hardcoded fallbacks
  (77-81 → 210-214). Make each `export` conditional on the var being set (or on an explicit
  `--churn-*` flag), so the locustfile stays authoritative; keep the run-summary echo
  (417/661) reporting the effective values. (Do **not** touch `setup-lambda-benchmark.sh:229`
  — that script is removed in Step 7.)
- **Concerns:** Keep the byte-identical-universe A/B guarantee (CHURN_SEED must still be pinned
  across the ON/OFF legs — pin it in the script, since seed reproducibility is a correctness
  property of the A/B, not a default to defer to the locustfile).
- **Status:** [x] Done (2026-07-06)

#### Step 5: Extract `benchmark-methodology.md` from the guide
- **Files:** NEW `docs/benchmarks/benchmark-methodology.md`; edit
  `docs/benchmarks/benchmark-guide-8xA100.md`
- **Description:** Move the *how-to* sections **verbatim** out of the 2,536-line guide: Quick
  Start, Warm-up Mode, Benchmark Parameters Reference, Prompt Distribution, Prefix Ratio, Token
  Buckets, Custom Prompt Files, Client Tokenization, Test Scenarios, Validation Checklist,
  Expected Metrics Reference, Live Monitoring, Troubleshooting, Cleanup & Restarts, Exporting
  Results, Recommended Test Sequence (guide lines ~156-475, 764-885, 2061-2536). Fold in the
  `/benchmark` skill gotchas and a pointer to `interpreting-benchmark-numbers.md`. Correct the
  `--prompt-dist` naming (`large-prefix`, document `churn`) while moving — but keep the
  numbers-fixing to Step 7 so this stays a clean move.
- **Concerns:** Pure content move; verify no internal anchor (e.g. `#sse-flush-regression-c70f0c1`)
  is orphaned — retarget cross-links in Step 7.
- **Status:** [x] Done (2026-07-06)

#### Step 6: Archive the lab-notebook into `docs/benchmarks/history/`
- **Files:** NEW `docs/benchmarks/history/` (e.g. `benchmark-history-8xA100.md`); edit
  `docs/benchmarks/benchmark-guide-8xA100.md`
- **Description:** Move **verbatim, append-only** the dated instance tables and closed
  investigations: "Detailed Results by Instance", "Real Benchmark Results" (Instances 1-9),
  "Recommended Next Benchmarks", "Benchmark Validity Status", "Post-Fix Re-Run Plan", "SSE
  Flush Regression (c70f0c1)" (guide lines ~47-155, 886-2060). This is the historical record;
  it stays quotable-with-context but out of the load-bearing docs.
- **Concerns:** Append-only archive — do not edit the moved numbers (their invalidity is the
  point). Preserve heading anchors referenced by investigations in `.dev-context/`.
- **Status:** [x] Done (2026-07-06)

#### Step 7: Author `benchmark-results-current.md` + convert the guide into an index
- **Files:** NEW `docs/benchmarks/benchmark-results-current.md`; edit
  `docs/benchmarks/benchmark-guide-8xA100.md`; update cross-links in
  `docs/benchmarks/kv-cache-prefix-routing-benchmark.md` and any `.dev-context/*` that deep-link
  the guide's moved anchors.
- **Description:**
  - `benchmark-results-current.md`: only numbers valid on **current defaults** (50 prefixes,
    20ms flush, residency 0.2), each stamped commit + manifest. Until the re-baseline campaign
    (P0/P1 items 4-6) runs, the representative-workload headline honestly reads **"TBD."**
  - Convert `benchmark-guide-8xA100.md` into a thin **index/landing page** that links to
    methodology / results-current / history (keeps the well-known filename and inbound links
    alive). Rewrite the **TL;DR** to state plainly that every pre-June headline (-80…-85%) was
    measured on the now-deprecated **5-prefix** workload and is not citable on current defaults;
    remove/relocate the stale May 22 "unresolved" note (the thrashing was closed 2026-05-26 as a
    workload artifact).
  - **Reconcile the 10ms/20ms flush contradiction:** 20ms **shipped**
    (`src/config_schema.hpp:182`, `docker-compose.benchmark-real.yml:171`); mark the "10ms
    confirmed as the correct default" line (guide ~1102) as superseded in the history archive and
    state 20ms as current in results-current/methodology.
- **Concerns:** This is the one judgment-heavy step; keep prose edits surgical and cite commits.
- **Status:** [x] Done (2026-07-06)

#### Step 8: Neutralize the two deprecated scripts
- **Files:** `scripts/run-multi-gpu-benchmark.sh`, `scripts/setup-lambda-benchmark.sh`;
  `scripts/README.md`, `tests/integration/README.md`
- **Description:** Per the **Decisions** below, either delete both or replace their bodies with
  a 3-line stub that prints the `bench.sh` pointer and `exit 1` (kills the silent-fallthrough and
  the `run-benchmark.sh` name-collision either way). Update the two READMEs that reference them.
- **Concerns:** Confirm no Makefile target invokes them (verified: none).
- **Status:** [x] Done (2026-07-06)

### Decisions (resolved 2026-07-06)
1. **Deprecated scripts — delete vs. `exit 1` stub?** → **`exit 1` stub** with a `bench.sh`
   pointer (more discoverable than a missing file; still removes the `run-benchmark.sh`
   name-collision).
2. **Guide file — keep `benchmark-guide-8xA100.md` as an index vs. remove/redirect?** →
   **kept as a thin index** at the same path, so all inbound links stay valid.

**Implemented** across three commits on `claude/benchmark-tooling-p0-ui6dtp`:
code (steps 1–4), doc restructure + script stubs (steps 5–8), and this status update.
Follow-on P0/P1 (re-baseline campaign + statistics/manifest/3-node machinery) remains open.

### Post-Implementation
- [ ] `/review` — Hard Rules compliance (light here: no C++; check doc-sync + naming consistency).
- [ ] `/doc` — sync `scripts/README.md`, `tests/integration/README.md`, the `/benchmark` skill,
  and `.dev-context/claude-locust-sync-map.md`/`README.md` index to the new doc layout.
- [ ] **Verification ladder (no `make` build needed):** `python -m py_compile
  tests/integration/{locustfile_real,run_benchmark_comparison}.py`; `bash -n
  scripts/{bench,bench-residency-ab}.sh` (+ the stubs if used); `bench.sh --help`/a
  `--dry-run`-equivalent grep to confirm the banner prints effective 50/0.9 with knobs unset;
  markdown link-check across `docs/benchmarks/` for orphaned anchors. See `/validate`.
- [x] **Item 4/6 — standard matrix + variance rule** (ran 2026-07-13 on 8×A100, commit
  `817a1b5`): median-of-3 across the 4 configs. Headline is **load-dependent** — 8B/20u −13.3%,
  13B/30u −9.1% (both reliable wins), 13B/20u no reliable effect, 13B/10u **+29% (reliable
  regression)** — monotonic in cluster throughput. Cache hit +3× everywhere, decoupled from P99.
  Written up in `docs/benchmarks/benchmark-results-current.md`.
- [ ] **Item 5 — threshold leg** (shipped `2.0/2` vs raised `3.0/4`) NOT yet run; the 30–47%
  load-aware fallback rates seen across the matrix make it the highest-value remaining GPU run.
  Runbook `benchmark-rebaseline-campaign.md` §2; run files under `docs/benchmarks/rebaseline/`.

### P1 progress (follow-on branches)
- [x] **Item 7 — `--repeat` + aggregation** (branch `claude/benchmark-p1-repeat-aggregate`,
  2026-07-06): `results_parser.py aggregate` computes median/IQR across repeat runs with a
  pre-registered verdict (INSUFFICIENT DATA / NO RELIABLE EFFECT when the IQR spans zero /
  signed IMPROVEMENT-REGRESSION by the median) plus an affinity-thrash hot-spot flag;
  `bench-runner.sh --repeat N` runs each config N× and aggregates per config. 12 pure-Python
  unit tests. This is the prerequisite for a trustworthy re-baseline (items 4-6) — run it
  **before** the GPU campaign so results carry variance stats, not a hand-picked best run.
- [x] **Item 8 — run manifests** (branch `claude/benchmark-p1-manifest`, 2026-07-06):
  `bench.sh` writes `manifest.json` (commit, argv, effective routing config, workload knobs,
  hardware, vLLM version) into every report + warm-up dir; `results_parser.py compare`/
  `aggregate` load it and print a loud `WORKLOAD MISMATCH` warning (advisory, non-blocking)
  when the workload knobs differ across the runs being combined. 11 pure-Python unit tests.
- [x] **Item 9 — 3-node scrape + distribution/Gini/diversion counters** (branch
  `claude/benchmark-p1-scrape-3node`, 2026-07-06): `bench.sh` scrapes every ranvier node to
  `prometheus_metrics_node{N}.txt` (no more `break`-after-first — the F5 single-node undercount
  is fixed) plus a concatenated file for back-compat; `results_parser.py parse_prometheus_report`
  sums across nodes and promotes the per-backend distribution, its Gini, both diversion counters,
  and `nodes_scraped` to first-class fields (surfaced in `compare`). 10 pure-Python unit tests.
- [x] **Item 10 — A/B fairness** (branch `claude/benchmark-p1-ab-fairness`, 2026-07-06):
  `bench.sh --order rr-first|prefix-first` picks the A/B arm order; the warm-up is now a
  `run_warmup(mode)` function run **per-arm after each mode restart** (both arms identically
  primed); the vLLM-cache-carry-over caveat (Ranvier-only restart) is logged and written into
  the compare report header. `bench-runner.sh` **alternates the order across `--repeat`** runs
  (rr-first/prefix-first) for `--compare` configs that don't pin `--order`, cancelling order
  bias for free. Verified via `bash -n`, `--order` validation, stubbed arm-order/report-mapping
  logic test, and dry-run alternation (incl. pinned-order preserved, non-compare untouched).

**P1 "statistical + capture machinery" (items 7–10) is complete.** The benchmark tooling now
does repeats + median/IQR verdicts, self-describing manifests with mismatch guards, full-cluster
telemetry with Gini, and fair A/B ordering — the prerequisites for a trustworthy re-baseline.
**Next: the P0/P1 50-prefix re-baseline campaign (items 4–6), which needs 8×A100 GPU time** and
produces the first citable headline under the representative workload (fills the current TBD in
`benchmark-results-current.md`). A **turnkey runbook** — copy-paste commands, committed run
files, pre-registered decision rules, GPU budget, and validity gates — is ready at
[`docs/benchmarks/benchmark-rebaseline-campaign.md`](docs/benchmarks/benchmark-rebaseline-campaign.md)
with run files under `docs/benchmarks/rebaseline/`. Nothing blocks it but the GPU box.

---

## 26. Kimi (Moonshot) Model Support (2026-07-18)

Added the `kimi` chat-template format and the tokenizer tooling needed to route
Kimi K2 (and, once public, K3) with correct prefix-cache alignment.

**Done:**
- `ChatTemplateFormat::kimi` — per-role tokens (`<|im_user|>`/`<|im_assistant|>`/
  `<|im_system|>`) + `<|im_middle|>`/`<|im_end|>`, generation prompt, no BOS.
  Verified byte-exact against Moonshot's `chat_template.jinja`.
- Default-system injection for system-less conversations (in `RequestRewriter`),
  including the trailing newline the reference template leaves — a bug the parity
  harness caught before it shipped as a real cache-misalignment.
- Convert→validate tokenizer pipeline under `tests/tokenizer_parity/`:
  `convert_kimi_tokenizer.py` (tiktoken→fast) + `kimi_tokenizer_parity.py`
  (authoritative vs Ranvier-path IDs). Verified 5/5 parity on Kimi-K2-Instruct.
- Gated FFI parity test `kimi_tokenizer_parity_test.cpp`
  (`-DRANVIER_BUILD_KIMI_PARITY_TEST=ON`, skips without the artifacts).

**Deploy contract (Kimi on Ranvier):**
1. `chat_template_format: kimi`.
2. Kimi ships **no** fast `tokenizer.json` — a converted one is mandatory
   (`convert_kimi_tokenizer.py`), and must pass the parity harness.
3. Pin **the same** `tokenizer.json` on Ranvier (`tokenizer_path`) *and* the
   serving backend, or the two sides tokenize differently and cache never aligns.

**Open:**
- D — spike on a live K3-on-vLLM fleet: confirm KV events flow and prefix caching
  actually hits under Kimi Delta Attention (hybrid linear attention). Needs GPU.
  Runbook ready: [`docs/benchmarks/kimi-k3-prefix-caching-spike.md`](docs/benchmarks/kimi-k3-prefix-caching-spike.md)
  (pre-registered decision rules, K2 control, 3 gated questions).
- A — benchmark the TTFT / cache-hit win for a Kimi workload. Needs GPU.
- Re-run the convert→validate pipeline against **K3** when weights/tokenizer ship
  (K2 is today's proxy; confirm the template tokens/default prompt didn't shift).
- Optionally wire the gated parity test into a CI runner that has the artifacts.

**Known limitations (documented in code):**
- Multimodal `content` arrays (K3 vision) are dropped from routing/tokenization
  (`request_rewriter.hpp`) — image requests route on their text turns only.
- Tool-result `## Return of {id}` framing is not reproduced.

---

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)


---
