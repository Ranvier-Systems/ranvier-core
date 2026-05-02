# Per-API-Key Attribution for Metrics and Async Persistence

**Status:** Draft — design review
**Date:** 2026-05-02
**Author:** generated exploration (claude/api-key-attribution-metrics-HMpQI)
**Implementation:** not started; this memo gates the implementation PR

## 1. Problem Statement

Operators today cannot answer two basic observability questions without
out-of-band log scraping:

1. *"Which API key drove this latency p99 spike?"*
2. *"How much usage came from key X over the last 24 hours / month?"*

Ranvier already accepts a list of API keys with metadata (`ApiKey.name`,
`ApiKey.roles`, see `src/config_infra.hpp:105`). On the admin plane it
authenticates and audit-logs by `name`. On the data plane (proxy path,
`/v1/chat/completions`, `/v1/completions`), no key is parsed at all —
requests are rate-limited but anonymous. As a result Prometheus
exposition aggregates across all callers, and the async persistence
queue does not record per-request rows, so historical reporting is
impossible without log archaeology.

This memo proposes making per-API-key attribution a first-class
observable dimension, end to end:

* `ProxyContext` carries per-key fields populated at the existing
  authentication / token-extraction boundary.
* Existing Prometheus counters and histograms gain an `api_key`
  label, bounded in cardinality.
* A new additive SQLite table records per-request attribution rows.
* A read-only admin endpoint exposes per-key aggregates over a query
  window.

## 2. Current-State Survey (Gaps the Memo Must Address)

These are not assumptions — they are findings from the source.

| Gap | Evidence | Implication |
|---|---|---|
| Data plane does not authenticate. | `src/http_controller.cpp:303-308` registers `/v1/*` with `make_rate_limited_handler` only, no `auth_check`. | Cannot derive `ApiKey` on proxy path today. Need a parsing-only resolver. |
| `ProxyContext` has no key fields. | `src/http_controller.hpp:85-179`. | Must add fields. |
| `AuthConfig::validate_token()` returns `pair<bool,string>` (key name only). | `src/config_infra.hpp:180-197`. | Sufficient for attribution; no need to plumb full `ApiKey` through. |
| Persistence schema has only `backends` and `routes`. | `src/sqlite_persistence.cpp:54-90`. | This is a **new** table, not a column add — task description's "request-attribution rows today" is inaccurate; flagging it explicitly. |
| `AsyncPersistenceManager` operations are only route/backend mutations. | `src/async_persistence.hpp:52-81`. | Need a new `LogRequestOp` variant. |
| Existing label patterns: `make_gauge` with a fixed label set + a lambda accumulator (e.g. `prefix_hits_by_compression_tier`, `cache_events_loads_ignored`). | `src/metrics_service.hpp:267-319`. | Per-key labels need a different shape: cardinality is dynamic, so we cannot pre-register one gauge per key at startup without knowing the key set ahead of time. We register labelled accumulators lazily, bounded. |

## 3. Naming Conventions

* Field names: `api_key_id` (stable identifier; the `ApiKey.name` value)
  and `api_key_label` (sanitised label form used in metrics — same
  string, but lowercased / non-alphanumerics → `_`, length-capped).
* No `tenant_id`, `tenant`, `team`, `org`, or `customer` framing
  anywhere. The attributable unit is the API key, full stop.
* The `ApiKey.name` is already the operator-chosen audit identifier
  (e.g. `"production-deploy"`); reuse it. Do not introduce a new
  numeric `key_id` — that adds a join the operator does not need.

## 4. ProxyContext Field Additions

In `src/http_controller.hpp` `ProxyContext`:

```cpp
// Per-API-key attribution (populated by resolve_api_key() on entry to
// handle_proxy, before any routing decisions). Empty means the request
// arrived without authentication on the data plane — see §5.
std::string api_key_id;       // ApiKey::name, or "" if unauthenticated
std::string api_key_label;    // sanitised form of api_key_id, or "_unauthenticated"
                              // / "_invalid" / "_overflow" sentinel
```

Both are plain `std::string` (consistent with `request_id`, `endpoint`,
`user_agent`, etc.). Both are populated once, before routing, and only
read thereafter — no cross-shard mutation.

The label field is computed once (string ops are cheap, but doing it
once at population avoids per-metric-call recomputation) and is what
gets fed into `seastar::metrics::label_instance{"api_key", ...}`.

## 5. Data-Plane Key Resolution

This is the structurally significant change in this memo. The data
plane has no authentication today. Two viable shapes:

### Option A — Parse-only attribution (recommended)

Add `HttpController::resolve_api_key(const seastar::http::request&)`
that:

1. Reads the `Authorization` header. If absent → `api_key_id = ""`,
   `api_key_label = "_unauthenticated"`. Request proceeds.
2. Extracts the bearer token. Calls `_config.auth.validate_token(...)`.
   On valid → `api_key_id = key_name`, `api_key_label = sanitise(key_name)`
   (subject to the cardinality bound, §6).
3. On invalid / expired → `api_key_id = ""`,
   `api_key_label = "_invalid"`. Request proceeds (rate limit /
   policy decisions remain out of scope here).

This **does not enforce auth** on the data plane. It is purely
attribution. Deployments that today run with no auth on `/v1/*`
continue to work — they just see all traffic labelled
`api_key="_unauthenticated"`, which is correct.

### Option B — Optional enforcement

Add a config flag `auth.require_data_plane_auth` (default `false`).
When `true`, requests without a valid key are rejected `401`. The
attribution path is identical to Option A.

**Recommendation:** ship Option A in this PR. Option B is a small
follow-up flag once attribution is observable and operators have
visibility into who is calling unauthenticated.

### Hard Rules pertinent to this resolver

* Rule #22 (exception-before-future): the resolver runs synchronously
  inside `handle_proxy` (a coroutine), so any throw becomes a failed
  future cleanly. No `futurize_invoke` wrapping needed.
* Rule #1 (lock-free metrics): the resolver does not lock; the
  cardinality-bound table (§6) uses a per-shard `flat_hash_map` with
  no atomics or mutexes (each shard sees its own table).
* Rule #4 (bounded containers): the cardinality table has an explicit
  `max_label_cardinality`.

## 6. Metric Labels and Cardinality Bound

### 6.1 Label additions

The following existing series gain an `api_key` label:

| Series | File | Today's labels | + |
|---|---|---|---|
| `ranvier_http_requests_total` | `metrics_service.hpp` | (none) | `api_key` |
| `ranvier_http_requests_success` / `_failed` / `_timeout` / `_rate_limited` | same | (none) | `api_key` |
| `ranvier_http_request_duration_seconds` (histogram) | same | (none) | `api_key` |
| `ranvier_router_request_total_latency_seconds` (histogram) | same | (none) | `api_key` |
| `ranvier_request_input_tokens` / `_output_tokens` / `_cost_units` (histograms) | same | (none — currently aggregated counters; if histograms exist add label there) | `api_key` |

We do **not** add the label to backend-internal series
(`backend_active_requests`, `backend_metrics_*`, `circuit_breaker_*`)
— those are per-backend, not per-caller.

### 6.2 Cardinality bound — design

Prometheus cardinality is the operator's primary risk here. A naïve
implementation would create one time-series per (key × backend × shard
× metric × histogram-bucket), and a misconfigured loop on the caller
side could fan that out unboundedly.

**Design:**

* Config struct addition:
  ```cpp
  // src/config_schema.hpp (proposed)
  struct AttributionConfig {
      uint32_t max_label_cardinality = 256;  // distinct api_key label values per shard
      bool persistence_enabled = true;       // gate the new SQLite table
      uint32_t persistence_max_queue = 50000; // backpressure-specific cap (separate from base AsyncPersistenceConfig.max_queue_depth)
      uint32_t admin_query_max_window_hours = 168;  // bound on /admin/keys/usage window
      uint32_t admin_query_max_rows = 10000;
  };
  ```
* Default `max_label_cardinality = 256`. This is intentionally
  conservative: a Prometheus server scraping every 15s sees roughly
  256 × ~10 series × 64 shards ≈ 160k active series for the new
  labels in the worst case. Operators with many keys can raise it.
* Per-shard table: `absl::flat_hash_map<std::string, LabelSlot>`
  where `LabelSlot` holds the registered counter/histogram references.
  Lookups are O(1).
* On first observation of an unseen key on this shard:
  * If `table.size() < max_label_cardinality`: insert a new slot,
    register the per-label metric, attribute the request to it.
  * Else: attribute to the sentinel `_overflow` slot (always
    pre-registered), and:
    * Bump `ranvier_api_key_label_overflow_total` (a counter labelled
      only by `_overflow` — bounded by definition).
    * Emit a single `log.warn` *the first time* overflow happens on
      this shard (use a `std::atomic<bool> _overflow_warned` flag
      per-shard), with the offending `api_key_id` truncated and the
      configured bound. Subsequent overflows are silent except for
      the counter — avoids log floods.
* Pre-registered sentinels (always present, never count toward the
  bound): `_unauthenticated`, `_invalid`, `_overflow`.
* Boot-time pre-registration: walk `AuthConfig.api_keys` and pre-fill
  the table on each shard up to `max_label_cardinality - 3`. If
  `api_keys.size() > max_label_cardinality - 3`, log a warn at boot
  with the count and the bound, and pre-register the first
  `max_label_cardinality - 3` keys (deterministically — by
  vector order). Excess keys observed at runtime fall to `_overflow`.

### 6.3 Sanitisation

`sanitise(name)`:

* lowercase
* `[^a-z0-9_]` → `_`
* truncate to 64 chars
* if empty after sanitisation: `"_unnamed"`

Operators choose `ApiKey.name`; this is just defensive. Document the
mapping in `docs/internals/`.

### 6.4 Hard Rules

* Rule #1 (lock-free metrics): the per-shard `flat_hash_map` is
  shard-local. No atomic. No mutex. Lookups happen on the producer
  path only (request handling), not on Prometheus scrape — scrape
  reads the per-slot counters/histograms directly.
* Rule #6 (deregister metrics first in `stop()`): the new
  per-key gauges/counters are added to the existing
  `_metrics` group, which is already cleared first in
  `MetricsService::stop()`. No new teardown logic needed if we
  follow the existing pattern.
* Rule #4 (bounded containers): explicit `max_label_cardinality`.

## 7. Persistence: New Request-Attribution Table

### 7.1 Schema (additive migration)

```sql
CREATE TABLE IF NOT EXISTS request_attribution (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    request_id    TEXT    NOT NULL,
    api_key_id    TEXT    NOT NULL,        -- ApiKey.name; "" for unauthenticated
    timestamp_ms  INTEGER NOT NULL,        -- wall-clock ms at request_start
    endpoint      TEXT    NOT NULL,        -- "/v1/chat/completions" etc
    backend_id    INTEGER,                 -- nullable: may be unset on early-fail
    status_code   INTEGER NOT NULL,
    latency_ms    INTEGER NOT NULL,
    input_tokens  INTEGER NOT NULL,
    output_tokens INTEGER NOT NULL,
    cost_units    REAL    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_request_attribution_key_ts
    ON request_attribution(api_key_id, timestamp_ms);
CREATE INDEX IF NOT EXISTS idx_request_attribution_ts
    ON request_attribution(timestamp_ms);
```

### 7.2 Migration shape

* Pure `CREATE TABLE IF NOT EXISTS` + `CREATE INDEX IF NOT EXISTS`.
  Idempotent. No `ALTER TABLE` needed (the table is brand new).
* Older-schema startup: opens the existing DB, runs the migration,
  table appears. No data loss; no backfill.
* Empty DB: existing
  `"Persistence store is empty"` / `"starting with empty state"` log
  paths trigger; the new table is created alongside the old ones.
* Corrupt DB: existing `verify_integrity` failure path clears the
  store and recreates *all* tables, including the new one. No
  separate corruption handling.

This mirrors the patterns asserted in
`tests/integration/test_persistence_recovery.py:452-611`.

### 7.3 Retention and bounding

The SQLite file grows linearly with request volume. Without bounding
this becomes a disk-space risk. Two options for the first PR:

* **Option A (recommended):** add a row-count cap
  `AttributionConfig.max_request_rows = 1_000_000` enforced by a
  lightweight `DELETE FROM request_attribution WHERE id < (SELECT
  MAX(id) - max_request_rows FROM request_attribution)` run from the
  persistence worker at flush time *only when* row count exceeds
  the cap (probe via `SELECT COUNT(*)` once per minute, not per
  flush — bounded; covers Hard Rule #4).
* **Option B:** time-window retention (`DELETE WHERE timestamp_ms <
  now - retention_ms`). More natural semantically but requires the
  worker to know wall-clock time and adds a DML on every flush.

Recommendation: ship Option A; revisit when operators ask for
time-based retention.

### 7.4 New PersistenceOp variant

```cpp
// src/async_persistence.hpp
struct LogRequestOp {
    std::string request_id;
    std::string api_key_id;
    int64_t     timestamp_ms;
    std::string endpoint;
    std::optional<BackendId> backend_id;
    int32_t     status_code;
    int32_t     latency_ms;
    int64_t     input_tokens;
    int64_t     output_tokens;
    double      cost_units;
};

using PersistenceOp = std::variant<
    SaveRouteOp, SaveBackendOp, RemoveBackendOp,
    RemoveRoutesForBackendOp, ClearAllOp,
    LogRequestOp                              // new
>;
```

Enqueued from `handle_proxy`'s terminal path (success, failure, or
timeout — wherever the existing latency-recording bool is flipped).
Single enqueue per request. Bounded by the existing MPSC ring buffer
backpressure (`AsyncPersistenceConfig::max_queue_depth`).

### 7.5 Hard Rule #14 caveat — must verify before implementation

`LogRequestOp` carries `std::string` fields. The MPSC ring buffer
producers run on N reactor shards; the consumer drains on a single
worker (`async_persistence.cpp:142,234,243,275`). When the consumer
destroys a `PersistenceOp` whose strings were heap-allocated on
shard N, the destructor runs on the consumer's shard — **this is
Hard Rule #14 Bug #1 (cross-shard free).**

This pattern already exists in the codebase: `SaveBackendOp.ip` and
`SaveRouteOp.tokens` (vector). Either:

1. The existing pattern is silently relying on Seastar's
   `do_foreign_free` and is, in fact, fine; or
2. The existing pattern has a latent bug that we are about to widen.

**Action item before implementation:** verify (with the maintainers
or by reading the MPSC ring buffer impl) which case holds. If (2),
the fix is to wrap each string in `seastar::foreign_ptr<std::unique_ptr<std::string>>`
on enqueue and reallocate locally on the consumer. We do not
unilaterally fix the existing ops in this PR — that is a separate
audit — but `LogRequestOp` should follow whatever decision lands.

### 7.6 Hard Rule #7 (no business logic in persistence)

The new persistence code transforms `LogRequestOp` → SQL bind
parameters and inserts the row. It does not validate, sanitise, or
clamp values. The service layer (the metric label sanitisation in §6
and the request-handling code in `handle_proxy`) owns all
validation. Persistence is dumb storage.

## 8. Admin Endpoint: `GET /admin/keys/usage`

### 8.1 Shape

```
GET /admin/keys/usage?from=<unix_ms>&to=<unix_ms>&key=<name>&limit=<n>
Authorization: Bearer <admin_key>
```

* `from`, `to`: required, unix milliseconds. Window must be
  `≤ AttributionConfig.admin_query_max_window_hours`. Reject
  `400 Bad Request` otherwise.
* `key`: optional. If present, scope the result to one
  `api_key_id`. If absent, return aggregates per key.
* `limit`: optional; capped by `AttributionConfig.admin_query_max_rows`.

### 8.2 Response shape

```json
{
  "window": {"from_ms": 1714608000000, "to_ms": 1714694400000},
  "rows": [
    {
      "api_key_id": "production-deploy",
      "request_count": 12483,
      "success_count": 12410,
      "error_count": 73,
      "latency_ms_p50": 240,
      "latency_ms_p95": 870,
      "latency_ms_p99": 2104,
      "input_tokens_sum": 41209830,
      "output_tokens_sum": 8920104,
      "cost_units_sum": 51309.4
    }
  ],
  "truncated": false
}
```

### 8.3 Routing

Register under the existing admin auth check (already RBAC-scoped
via the `auth_check` lambda at `http_controller.cpp:312`). Existing
admin routes use `make_admin_handler(auth_check, ...)` — reuse it.

The `roles` field on `ApiKey` is currently advisory (`config_infra.hpp:110`,
"Future RBAC prep"). For this PR we keep RBAC at the existing
"admin or not" granularity: any key that passes `check_admin_auth`
can call the endpoint. A future RBAC refinement can scope to
`roles: ["metrics-read"]`; out of scope here.

### 8.4 Implementation notes

* The query runs on the persistence worker via a new
  `AsyncPersistenceManager::query_attribution_summary(...)`
  returning `seastar::future<std::vector<AttributionRow>>`.
* SQL: percentile via `percentile_cont` is unavailable in vanilla
  SQLite. Two options:
  * Materialise rows into a temp buffer and compute percentiles in
    C++ (`std::nth_element`). Bounded by `admin_query_max_rows`,
    so memory is bounded.
  * Use ordered windowing (`SELECT ... ORDER BY latency_ms LIMIT 1
    OFFSET N`). Two extra queries per percentile per key. Cleaner
    but slower.

  Recommendation: in-memory percentile from a single ordered SELECT
  capped by `max_request_rows_per_query` (separate, smaller cap, e.g.
  100k). If a key exceeds the cap, return `truncated: true` and a
  best-effort percentile.

### 8.5 Seastar concurrency note

The admin handler runs on whichever shard handled the HTTP request.
The `AsyncPersistenceManager` is a `seastar::sharded<>` service — the
SQLite database itself is owned by a single shard (consumer). The
admin handler must `seastar::smp::submit_to(persistence_owner_shard,
...)` to run the read, then return results. This is the same pattern
the existing admin routes already use for cluster-wide reads.

* No cross-shard reads of in-memory state are required (the metrics
  per-key map is shard-local; this endpoint queries SQLite only).
* The submitted lambda must follow Hard Rule #14: extract the
  query parameters by value before submit_to, reallocate result
  vector locally on the calling shard, and return via `foreign_ptr`
  if data is large. For the bounded result set (≤ 10k rows), a
  plain `std::vector<AttributionRow>` returned through `submit_to`
  with copy on the caller side is acceptable (consistent with how
  `dump/cluster` etc. already work — confirm before implementation).
* Hard Rule #16 (lambda coroutine fiasco): if the implementation
  uses a coroutine inside `.then()`, wrap with
  `seastar::coroutine::lambda()`. Prefer a coroutine top-to-bottom
  to avoid the trap.

## 9. Test Plan (sketch — not implemented in this memo)

* `tests/integration/test_metrics.py`:
  * Start server with two configured `api_keys` (`alice`, `bob`).
  * Send chat completions with each key and one without auth.
  * Scrape `/metrics`. Assert
    `ranvier_http_requests_total{api_key="alice"} > 0`,
    `...{api_key="bob"} > 0`,
    `...{api_key="_unauthenticated"} > 0`.
  * Histogram `_bucket` lines also carry `api_key="alice"` etc.
* `tests/integration/test_persistence_recovery.py`:
  * Test 06 — `request_attribution` table created on fresh DB.
  * Test 07 — older-schema DB (manually create a DB with only
    `backends`/`routes` via SQL, then start server) → server adds
    the new table on boot, no errors, no data loss.
  * Test 08 — corrupted DB still passes the existing recovery
    path and the new table is recreated.
* `tests/integration/test_attribution_cardinality.py` (new):
  * Configure `max_label_cardinality = 4` and 6 keys.
  * Send traffic with all 6.
  * Assert only 4 + 3 sentinels distinct `api_key` label values
    appear.
  * Assert `ranvier_api_key_label_overflow_total > 0`.
  * Assert exactly one warn log line about overflow per shard.

## 10. Out of Scope (explicit non-goals)

This memo is strictly attribution. Specifically out of scope:

* Per-key rate limiting or quota enforcement.
* Per-key authentication enforcement on the data plane (Option B
  in §5 — flagged for follow-up).
* Per-key cost accounting beyond the existing `cost_units` field.
* `roles`-based RBAC for the new admin endpoint beyond the existing
  admin gate.
* Time-based retention on the new SQLite table (row-count cap only
  — see §7.3).
* Anything cluster-wide: the new SQLite table is single-node.
  Cluster aggregation is a separate, larger problem.

## 11. Open Questions for Reviewer

1. **Hard Rule #14 (§7.5):** is the existing `SaveBackendOp.ip` /
   `SaveRouteOp.tokens` cross-shard free pattern intentional and
   safe, or latent? `LogRequestOp` should match whatever the
   answer is.
2. **Cardinality default (§6.2):** is 256 per shard the right
   conservative default, or should it be lower (e.g. 64)?
   Justification welcome from anyone who has run Prometheus at
   scale on this codebase.
3. **Data-plane auth posture (§5):** ship Option A only in this
   PR, defer Option B to a follow-up flag PR — does that match the
   maintainers' preference, or should both ship together?
4. **Admin endpoint percentiles (§8.4):** in-memory vs. multi-query.
   Slight implementation-difficulty trade-off; either is fine.

## 12. Implementation Sequence (post-review)

This memo blocks the implementation PR. Once approved:

1. Add `AttributionConfig` + plumb config loader.
2. Add `ProxyContext` fields and `resolve_api_key()` resolver.
3. Add per-shard label table in `MetricsService`; wire into the
   request-handling call sites.
4. Add SQLite migration + `LogRequestOp` + worker-side INSERT.
5. Add the admin endpoint.
6. Tests as described in §9.

Each step is a discrete commit. The PR description (operator-facing,
OSS audience) frames this as "per-API-key attribution for
observability and historical reporting" — accurate to what the code
does. If the broader internal roadmap depends on this work, the
maintainers should decide separately whether to mention that in the
PR body; this memo does not pre-commit to that framing.
