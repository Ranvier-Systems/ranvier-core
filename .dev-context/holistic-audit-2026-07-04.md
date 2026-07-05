# Holistic System Audit — 2026-07-04

**Type:** `/audit` (Hard Rules compliance, async integrity, architecture drift, doc sync).
**Scope:** The files churned since the 2026-02-12 adversarial audit that were NOT
covered by the 2026-07-03 invariant audit: `application.{hpp,cpp}`,
`telemetry_service.{hpp,cpp}`, `telemetry_schema.hpp`, `config_loader.cpp`,
`config_schema.hpp`, `config_infra.hpp`, `metrics_service.hpp`,
`gossip_protocol.{hpp,cpp}`, `http_controller.{hpp,cpp}` (post-Feb delta), plus
doc-sync checks. Cross-checked against `adversarial-audit-2026-02-12.md` so resolved
findings are not re-reported (all prior fixes encountered in scope were confirmed
still in place).
**Static analysis only.** The HIGH finding (V1) was independently re-verified against
`gossip_protocol.cpp` / `gossip_transport.cpp` source before this report was recorded.

## Compliance Summary

```
[X] Compliant:        Rules #0, #1, #3, #5, #6, #7, #8, #10, #12, #13, #14, #18, #20, #22
[!] Violations Found: Rules #21 (V1, V4), #16 (V2), #4 (V5), #9 (V6), #11-adjacent thread-safety (V3), #17 (marginal, debt D3/D4)
[?] Not Applicable:   Rules #2 (no hot-path sequential-await loops in scope; startup replay noted as debt), #15 (no FFI in scoped files), #19 (no raw semaphore use in scope), #23 (no long-lived temporary_buffer shares in scope)
```

Doc sync: 2 violations + 1 minor (V7–V9). Architecture drift: 1 minor convention
drift (schema header carrying logic, D1); layer boundaries otherwise intact.

## Violations Detail (ranked by severity)

### V1 — HIGH — Dangling reference through the gossip ACK path
- **File:Line:** `src/gossip_protocol.cpp:1073` (`send_ack`), used at `:1086`, `:1092`; callers at `:859`, `:867`, `:904`, `:920`, `:936`; compounded by `src/gossip_transport.cpp:153` (`GossipTransport::send`)
- **Rule:** #21 (Coroutine-Reference-Parameter)
- **Issue:** `send_ack(const seastar::socket_address& peer, uint32_t)` is a coroutine taking `peer` by const reference. `handle_packet()` (NOT a coroutine — it returns futures directly) passes its stack local `src_addr` (`gossip_protocol.cpp:719`) and **returns** the future (e.g. `return send_ack(src_addr, evict_pkt->seq_num);` at :859). `handle_packet`'s frame — including `src_addr` — is destroyed the moment it returns, while `send_ack`'s coroutine frame retains the dangling reference. `GossipTransport::send` (also a coroutine taking `const socket_address&`) re-reads `peer` **after** `co_await encrypt_with_offloading(...)` (DTLS path, gossip_transport.cpp:179-190). With DTLS enabled, every ACK reads dead stack memory for the destination address — misdirected ACKs or garbage; in plaintext mode the exposure is the failure-path log at :1092. **Verified against source 2026-07-04.**
- **Fix:** Change `send_ack` (and `GossipTransport::send`'s DTLS path) to take `seastar::socket_address peer` **by value** — small, trivially copyable. Audit all `handle_packet` branches that `return send_ack(src_addr, ...)` or store its future.

### V2 — MEDIUM — Unwrapped lambda coroutine passed to `max_concurrent_for_each`
- **File:Line:** `src/gossip_protocol.cpp:986-998` (`refresh_peers`, SRV branch)
- **Rule:** #16 (Lambda-Coroutine-Fiasco)
- **Issue:** `co_await seastar::max_concurrent_for_each(srv_records, 16, [this, &discovered_addresses](const auto& srv) -> seastar::future<> { ... co_await ... })` — a lambda coroutine passed to a continuation-based loop API without `seastar::coroutine::lambda()`. Hard Rule #16 lists the `parallel_for_each` family and says "when in doubt, always wrap." The adjacent comment (:976-985) argues Rule #2 and DNS-resolver safety but never addresses functor lifetime.
- **Fix:** Wrap with `seastar::coroutine::lambda(...)`, or hoist the body into a named member coroutine taking `srv` by value.

### V3 — MEDIUM — `std::gmtime` static-buffer race on the request auth path (prior finding E9, still open)
- **File:Line:** `src/config_infra.hpp:120` (`ApiKey::is_expired()`)
- **Rule:** thread-safety (Rule #11-adjacent; flagged E9/LOW in the 2026-02-12 audit at then-`config_schema.hpp:195`; never fixed, code has since moved)
- **Issue:** `std::tm now_tm = *std::gmtime(&now_time_t);` — `gmtime` returns a pointer to a shared static buffer. `is_expired()` runs inside `AuthConfig::validate_token()` on the request path on every shard concurrently; two shards racing corrupt each other's parsed dates → wrong expiry decisions.
- **Fix:** `std::tm now_tm{}; gmtime_r(&now_time_t, &now_tm);`

### V4 — MEDIUM — Coroutine reference parameter on `broadcast_route`
- **File:Line:** `src/gossip_protocol.cpp:465` (`broadcast_route(const std::vector<TokenId>& tokens, BackendId)`)
- **Rule:** #21
- **Issue:** Coroutine takes `tokens` by const reference. Currently every read (:474, :488, :494, :500-502) happens before the first suspension (:507), so it is latent — but any refactor that touches `tokens` after the `parallel_for_each` is a UAF. The newer siblings (`broadcast_hot_prefix_digest`, `HotPrefixDigestCallback`) were deliberately made by-value with `// Rule #21` annotations (gossip_protocol.hpp:90, :280); this older signature was never brought in line.
- **Fix:** Take `std::vector<TokenId> tokens` by value; callers `std::move` in.

### V5 — LOW — Unbounded containers: cluster peers and API keys (prior finding S11, still open) + DNS-discovered peer set
- **File:Line:** `src/config_loader.cpp:1356-1361` and `:408-421` (`cluster.peers`, YAML + env), `:1236-1254` (`auth.api_keys`), `src/gossip_protocol.cpp:966-1046` (`refresh_peers` → `discovered_addresses` / `*_peer_addresses` / `_peer_address_set`)
- **Rule:** #4 (Unbounded-Buffer)
- **Issue:** Every other config list gained a MAX + truncation warning, but `cluster.peers` and `auth.api_keys` still `push_back` unbounded. Downstream, `refresh_peers()` accepts however many addresses DNS returns — a compromised/misconfigured DNS server can inflate the peer table, `_peer_address_set`, and every per-peer map keyed off it (those maps are individually capped at `MAX_DEDUP_PEERS`, which contains the worst case, but the address vector/set themselves are not).
- **Fix:** `MAX_PEERS` (e.g. 256) enforced in the loader and in the `refresh_peers()` merge (truncate + warn + counter); `MAX_API_KEYS` (e.g. 1000) in the loader.

### V6 — LOW — Rule #9 debug-level catch logging in gossip_protocol (prior systemic finding, partially open)
- **File:Line:** `src/gossip_protocol.cpp:453-454, 460-461` (stop() shutdown catches), `:545, :632, :1092, :1217` (peer send-failure `handle_exception` at debug)
- **Rule:** #9 (every catch logs at warn minimum)
- **Issue:** The 2026-02 "promote all trace/debug catch blocks to warn" item was not completed here. The shutdown-path debug catches match the since-adopted "expected shutdown noise at debug" posture (cf. application.cpp:1613, which annotates it) — but the per-peer send failures are genuine network errors silently below warn, with no counter metric distinguishing them from success.
- **Fix:** Promote send-failure logs to warn (rate-limited), or add an explicit `// Rule #9: expected UDP loss, counted in <metric>` justification plus a send-failure counter; annotate the shutdown catches the way application.cpp does.

### V7 — DOC SYNC — `docs/internals/gossip-protocol.md` missing HOT_PREFIX_DIGEST
- **File:Line:** packet table (~line 24) ends at `CACHE_STATE 0x06`; zero occurrences of `HOT_PREFIX` vs `src/gossip_protocol.hpp:51, 208-232`
- **Issue:** The `HOT_PREFIX_DIGEST` (0x07) packet — wire format, v2 verified-bitmap tail, MAX_HASHES bound, unreliable-broadcast semantics, and the `hot_prefix_digests_sent/received` counters — is entirely undocumented in the authoritative wire-format deep-dive.
- **Fix:** Add a section for packet 0x07 (format table, forward-compat contract, bitmap tail encoding) and the two metrics rows.

### V8 — DOC SYNC — `docs/internals/per-api-key-attribution.md` schema stale
- **File:Line:** doc schema block ~lines 244-254 vs `src/sqlite_persistence.cpp:116` (+ migration at `:130-135`)
- **Issue:** The documented `request_attribution` CREATE TABLE omits the `tokens_estimated INTEGER NOT NULL DEFAULT 1` column (added 2026-06-15), and the doc nowhere explains the actual-vs-estimated usage preference implemented in the terminal attribution block (`http_controller.cpp:2540-2566`). The rationale lives only in `docs/architecture/response-usage-accounting.md`.
- **Fix:** Add the column + one paragraph on actual-usage preference.

### V9 — DOC SYNC (minor) — claude-context.md gossip packet list stale
- **File:Line:** `.dev-context/claude-context.md:141-143` (layout entry for `gossip_protocol.{hpp,cpp}`)
- **Issue:** Packet enumeration omits `CACHE_STATE` and `HOT_PREFIX_DIGEST`. The Source Code Layout file list itself is fully in sync (all 100 current `src/` files present).
- **Fix:** One-line update to the packet enumeration.

## Technical Debt Items

Recorded in BACKLOG.md §23 (audit 2026-07-04): telemetry_schema.hpp logic-in-schema
drift; http_controller `"stream":false` substring sniff; `next_seq_num` at-capacity
synchronous sweep (Rule #17 marginal); `snapshot_and_reset` no-yield at the 65536
validation ceiling; startup route replay serial `co_await` per record; stale
`<fstream>` include in application.cpp; config-struct by-value cross-shard captures
(accepted D4 posture, reconsider); signal-handler fire-and-forget without gate;
`ApiKey::is_expired()` re-parsing dates per call (fixing also resolves V3).

## Checked and Found Sound

- **Prior-audit fixes hold** in every scoped file re-encountered: heartbeat/discovery/retry timer gate holders span their async chains (gossip_protocol.cpp:350-362, 406-424, 1164-1171); `_peer_seq_counters`/`_pending_acks` bounded with overflow counters; persistence business validation lives in `Application::load_persisted_state` (Rule #7 clean); `secure_compare` no longer early-returns on length mismatch; config hot-reload uses `load_config_async` DMA with the old Rule #12 violation fixed and documented.
- **TelemetryService emitter is a model Rule #5/#18/#14 implementation:** gate at callback entry, re-arm before backpressure drop, holder moved into `.finally()`; `stop()` deregisters metrics first, cancels, then closes the gate; cross-shard snapshots ride `foreign_ptr` with shard-0-local copies and a yielding merge loop; bucket map hard-capped with overflow sentinel.
- **`CacheTopologyHandler`** hops to shard 0, builds JSON there, copies bytes back via foreign_ptr — correct Rule #14.
- **Shutdown ordering in `stop_services()`** (application.cpp:1736-1963) is coherent: KV subscriber/GIE EPP drained before RouterService teardown; controller before telemetry/metrics; per-shard thread_local cleanup while the reactor is up (Rule #13); shutdown timeout always resolves `_stop_signal`.
- **MetricsService:** fully lock-free (Rule #1), bounded per-backend and api-key-label maps with overflow sentinels, three metric groups deregistered before state clear (Rule #6), thread_local raw pointer paired with `stop_metrics()` wired from shutdown (Rule #13).
- **Gossip wire handling:** all deserializers length-check before reads; `HotPrefixDigestPacket` caps count on serialize AND deserialize, rejecting over-cap before allocating; forward-compat (`len >=`, type-not-version rejection) as documented; `is_duplicate` window bounded; `process_retries` yields every `kYieldInterval` peers.
- **config_loader:** every env `std::stod/stoul` wrapped with warn (Rule #10); YAML throws funnel to the `YAML::Exception` handler; `validate()` covers scoring weights, kv_events cross-field constraints, NaN/inf rejection; `std::ifstream` confined to the documented pre-reactor path.
- **http_controller post-Feb delta:** usage-ledger sink per-shard owned, `record()` try/catch-warn, `flush()` awaited in `stop()` after `_request_gate.close()`; attribution terminal block computes one value set behind a single null-check; `pool_role` admin param validated with 400 rejection; GenAI semconv attributes gated and honest about omitting estimated usage.
- **Source Code Layout** in claude-context.md lists every current `src/` file.

---

*Resolution note appended 2026-07-05 — post-fix record. Static analysis only; unit / sanitizer / integration gates ran in the developer's Docker container via CI. The findings above are the frozen point-in-time analysis; this note records their disposition so the BACKLOG §23 entry can collapse to a pointer (mirrors §20's closeout pattern and the §22 verification pass).*

## Resolution (2026-07-05)

All nine violations (V1–V9) fixed and merged; the nine tech-debt items are tracked open in BACKLOG §23.

| # | Disposition |
|---|-------------|
| V1 (HIGH) | FIXED — `send_ack` / `GossipTransport::send` take `socket_address` by value (Rule #21); gossip coroutine-lifetime sweep (PRs #606, #608). |
| V2 | FIXED — `refresh_peers` SRV lambda wrapped / hoisted (Rule #16). |
| V3 | FIXED — `ApiKey::is_expired()` no longer calls `gmtime`; expiry parsed once at config load into a `time_point` (also closes the per-call re-parse tech-debt item) (V3/V5/V6 batch, PR #607). |
| V4 | FIXED — `broadcast_route` takes tokens by value (Rule #21). |
| V5 | FIXED — `cluster.peers` / `auth.api_keys` bounded in the loader; `refresh_peers` DNS merge capped + counter. |
| V6 | FIXED — gossip per-peer send-failure catches counted + annotated (Rule #9). |
| V7 | FIXED — `gossip-protocol.md` gained the HOT_PREFIX_DIGEST (0x07) section (PR #609). |
| V8 | FIXED — `per-api-key-attribution.md` gained the `tokens_estimated` column + actual-vs-estimated section (PR #609). |
| V9 | FIXED — claude-context.md gossip packet enumeration updated inline. |

Tech-debt: `ApiKey::is_expired()` re-parse resolved with V3; the remaining eight items (telemetry_schema logic-in-schema, `"stream":false` sniff, `next_seq_num` sweep, `snapshot_and_reset` no-yield, startup route replay, stale `<fstream>`, config by-value cross-shard captures, signal-handler gate) remain open in BACKLOG §23, to be folded into PRs that next touch those files.
