#!/usr/bin/env python3
"""
Phase 4 benchmark harness: prefill-savings A/B for push vs. silent
cache-eviction notifications.

Drives M requests through Ranvier's proxy path with synthetic
prefix hashes, in lockstep with the generator's LRU model. For each
request, the harness consults the model to decide whether the routed
backend "would have" the prefix cached at request time. A miss is a
"wasted prefill."

Outputs a small JSON result file with:
  - total / wasted / wasted_ratio
  - p50/p95/p99 staleness window (ms)
  - events posted / applied / ignored (from /metrics scrape)

This is benchmark infrastructure. Reads metrics out of Ranvier's
Prometheus endpoint; does not require GPU or real backends.

Run via run.sh; see docs/benchmarks/push-cache-eviction.md.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from generator import (
    DEFAULT_BATCH_SIZE,
    EvictionRecord,
    LRUCacheModel,
    build_payload,
    hash_to_hex,
    now_ms,
    ranvier_hash_prefix,
    tokens_for_prefix_id,
    zipf_request_stream,
)

# Hard Rule #4: bounded.
MAX_REQUESTS_HARD_CAP = 10_000_000


@dataclass
class HarnessConfig:
    ranvier_proxy_url: str      # API/admin/cache-events port (default 8080)
    ranvier_metrics_url: str    # Prometheus /metrics port (default 9180)
    cache_events_url: str
    mode: str
    capacity: int                # per-backend LRU capacity
    population: int
    requests: int
    zipf_s: float
    batch_size: int
    seed: int
    backend_ids: List[int]       # IDs of all registered backends
    request_token_count: int
    output: str
    proxy_timeout_s: float = 5.0


@dataclass
class HarnessResult:
    mode: str
    total_requests: int = 0
    cache_hits: int = 0
    wasted_prefills: int = 0
    unknown_backend_routes: int = 0  # requests Ranvier didn't label with X-Backend-ID
    eviction_count: int = 0
    events_posted: int = 0
    metrics_received: int = 0
    metrics_evictions_applied: int = 0
    metrics_evictions_stale: int = 0
    metrics_evictions_unknown: int = 0
    per_backend_requests: Dict[int, int] = field(default_factory=dict)
    per_backend_hits: Dict[int, int] = field(default_factory=dict)
    staleness_samples_ms: List[float] = field(default_factory=list)

    def to_dict(self) -> Dict[str, object]:
        wasted_ratio = (
            self.wasted_prefills / self.total_requests
            if self.total_requests
            else 0.0
        )
        sl = sorted(self.staleness_samples_ms)

        def pct(p: float) -> float:
            if not sl:
                return 0.0
            k = max(0, min(len(sl) - 1, int(round(p * (len(sl) - 1)))))
            return sl[k]

        return {
            "mode": self.mode,
            "total_requests": self.total_requests,
            "cache_hits": self.cache_hits,
            "wasted_prefills": self.wasted_prefills,
            "wasted_ratio": wasted_ratio,
            "unknown_backend_routes": self.unknown_backend_routes,
            "evictions_modeled": self.eviction_count,
            "events_posted": self.events_posted,
            "per_backend": {
                str(bid): {
                    "requests": self.per_backend_requests.get(bid, 0),
                    "cache_hits": self.per_backend_hits.get(bid, 0),
                }
                for bid in sorted(
                    set(self.per_backend_requests)
                    | set(self.per_backend_hits)
                )
            },
            "ranvier_metrics": {
                "received_total": self.metrics_received,
                "evictions_applied": self.metrics_evictions_applied,
                "evictions_stale": self.metrics_evictions_stale,
                "evictions_unknown": self.metrics_evictions_unknown,
            },
            "staleness_ms": {
                "samples": len(sl),
                "p50": pct(0.50),
                "p95": pct(0.95),
                "p99": pct(0.99),
            },
        }


_METRIC_RE = re.compile(r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{[^}]*\})?\s+([0-9eE\.\-\+]+)")


def scrape_metrics(metrics_url: str) -> Dict[str, float]:
    """Pull /metrics and return a name->value dict with label sets summed.

    Seastar's Prometheus exporter prefixes every metric with `seastar_`
    and attaches per-shard labels, so
    `metrics_service.hpp::add_group("ranvier", {make_counter(
    "cache_events_received_total", ...)})` lands on the wire as
    `seastar_ranvier_cache_events_received_total{shard="0"} 42`.
    We drop the label set from the key and sum across all shards.
    """
    out: Dict[str, float] = {}
    try:
        with urllib.request.urlopen(metrics_url + "/metrics", timeout=2.0) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return out
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        m = _METRIC_RE.match(line)
        if not m:
            continue
        name = m.group(1)  # base name, labels stripped
        try:
            out[name] = out.get(name, 0.0) + float(m.group(3))
        except ValueError:
            continue
    return out


# Seastar's Prometheus exporter prefixes metrics with `seastar_<group>_`.
# The group name is "ranvier" (see src/metrics_service.hpp), so cache-event
# counters come through as `seastar_ranvier_cache_events_*`.
_M_RECEIVED = "seastar_ranvier_cache_events_received_total"
_M_APPLIED = "seastar_ranvier_cache_events_evictions_applied"
_M_STALE = "seastar_ranvier_cache_events_evictions_stale"
_M_UNKNOWN = "seastar_ranvier_cache_events_evictions_unknown"


def post_batch_timed(
    url: str, payload: bytes, timeout_s: float
) -> Tuple[bool, float]:
    """POST and return (ok, elapsed_ms) for staleness measurement.

    The cache_events handler awaits evict_by_prefix_hash_global before
    writing the response, so the response-received time is the point at
    which Ranvier has actually applied the eviction across all shards.
    """
    req = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            resp.read()
            ok = 200 <= resp.status < 300
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        ok = False
    return ok, (time.monotonic() - t0) * 1000.0


def proxy_request(
    proxy_url: str,
    prompt_token_ids: List[int],
    idx: int,
    timeout_s: float,
) -> Tuple[bool, Optional[int]]:
    """Fire one chat-completion through Ranvier and return (ok, backend_id).

    Sends the request as `prompt_token_ids` (vLLM Online Serving schema)
    so Ranvier skips its own tokenizer and routes on the exact tokens
    we pass in. This requires `RANVIER_ACCEPT_CLIENT_TOKENS=1` on the
    Ranvier side. The backend_id is read from the X-Backend-ID header
    that Ranvier sets in the response (see http_controller.cpp:1663).
    """
    body = json.dumps(
        {
            "model": "mock",
            "stream": False,
            "prompt_token_ids": prompt_token_ids,
            # `messages` is kept so the request still looks like a
            # chat-completion to mock_backend.py's path matcher.
            "messages": [{"role": "user", "content": "bench"}],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        proxy_url + "/v1/chat/completions",
        data=body,
        headers={
            "Content-Type": "application/json",
            "X-Request-ID": f"bench-{idx}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            resp.read()  # drain
            ok = 200 <= resp.status < 300
            raw = resp.headers.get("X-Backend-ID")
            backend_id: Optional[int] = None
            if raw is not None:
                try:
                    backend_id = int(raw)
                except ValueError:
                    backend_id = None
            return ok, backend_id
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return False, None


def run_harness(cfg: HarnessConfig) -> HarnessResult:
    if cfg.requests <= 0 or cfg.requests > MAX_REQUESTS_HARD_CAP:
        raise ValueError(
            f"requests must be in (0, {MAX_REQUESTS_HARD_CAP}]"
        )
    if cfg.mode not in ("push", "silent"):
        raise ValueError("mode must be push|silent")

    if not cfg.backend_ids:
        raise ValueError("backend_ids must not be empty")

    rng = random.Random(cfg.seed)
    # One LRU model per backend — each backend has its own KV cache.
    caches: Dict[int, LRUCacheModel] = {
        bid: LRUCacheModel(cfg.capacity) for bid in cfg.backend_ids
    }
    result = HarnessResult(mode=cfg.mode)

    metrics_before = scrape_metrics(cfg.ranvier_metrics_url)
    base_received = metrics_before.get(_M_RECEIVED, 0.0)
    base_applied = metrics_before.get(_M_APPLIED, 0.0)
    base_stale = metrics_before.get(_M_STALE, 0.0)
    base_unknown = metrics_before.get(_M_UNKNOWN, 0.0)

    # Pending eviction events, tagged with the backend they came from.
    pending: Dict[int, List[EvictionRecord]] = {bid: [] for bid in cfg.backend_ids}

    # Progress reporting: print to stderr at most once every 2s. stderr
    # keeps the JSON on stdout clean; plain lines (no \r) stay readable
    # when run-ab.sh tees to a log file.
    progress_start = time.monotonic()
    last_progress = progress_start

    def report_progress(done: int, total: int) -> None:
        nonlocal last_progress
        now = time.monotonic()
        # Always print at the end; throttle otherwise.
        if done != total and now - last_progress < 2.0:
            return
        last_progress = now
        elapsed = now - progress_start
        rate = done / elapsed if elapsed > 0 else 0.0
        eta = (total - done) / rate if rate > 0 else 0.0
        sys.stderr.write(
            f"  progress: {done}/{total} "
            f"({100 * done / total:.0f}%) "
            f"rate={rate:.1f} req/s elapsed={elapsed:.0f}s eta={eta:.0f}s\n"
        )
        sys.stderr.flush()

    def flush_backend(bid: int) -> None:
        batch = pending[bid]
        if not batch:
            return
        if cfg.mode == "push":
            payload = build_payload(batch, bid)
            ok, elapsed_ms = post_batch_timed(
                cfg.cache_events_url, payload, timeout_s=2.0
            )
            result.events_posted += len(batch)
            if ok:
                # Staleness window: wall-clock from event emission to the
                # POST response. handle_cache_events awaits
                # evict_by_prefix_hash_global before replying, so the reply
                # means the eviction has been applied on all shards.
                result.staleness_samples_ms.append(elapsed_ms)
        batch.clear()

    def flush_all() -> None:
        for bid in cfg.backend_ids:
            flush_backend(bid)

    for idx, prefix_id in enumerate(
        zipf_request_stream(cfg.population, cfg.requests, cfg.zipf_s, rng)
    ):
        # Build a deterministic token sequence for this prefix_id and
        # compute the same FNV-1a hash Ranvier will compute server-side.
        # The token list goes into prompt_token_ids, the hash is what
        # we'll send in the eviction event so it lands in Ranvier's
        # reverse index.
        tokens = tokens_for_prefix_id(prefix_id)
        h = ranvier_hash_prefix(tokens)
        ok, chosen_bid = proxy_request(
            cfg.ranvier_proxy_url, tokens, idx, cfg.proxy_timeout_s
        )
        result.total_requests += 1
        report_progress(result.total_requests, cfg.requests)

        if not ok or chosen_bid is None or chosen_bid not in caches:
            # Request failed, or Ranvier routed to a backend we don't model.
            # Count as wasted so it's visible, and skip LRU bookkeeping.
            result.unknown_backend_routes += 1
            result.wasted_prefills += 1
            continue

        result.per_backend_requests[chosen_bid] = (
            result.per_backend_requests.get(chosen_bid, 0) + 1
        )

        # Check if the chosen backend's modeled cache already holds this
        # prefix. Yes -> real cache hit. No -> backend pays full prefill
        # cost -> wasted prefill. The silent/push delta lives here:
        # with push, Ranvier removes stale RadixTree entries, so the next
        # request for an evicted prefix falls through to hash routing
        # and may pick a backend that still has it.
        cache = caches[chosen_bid]
        if cache.contains(h):
            result.cache_hits += 1
            result.per_backend_hits[chosen_bid] = (
                result.per_backend_hits.get(chosen_bid, 0) + 1
            )
        else:
            result.wasted_prefills += 1

        _, evicted = cache.admit(h, cfg.request_token_count)
        if evicted is not None:
            result.eviction_count += 1
            pending[chosen_bid].append(
                EvictionRecord(
                    prefix_hash=evicted[0],
                    timestamp_ms=now_ms(),
                    prefix_token_count=evicted[1],
                )
            )
            if len(pending[chosen_bid]) >= cfg.batch_size:
                flush_backend(chosen_bid)

    flush_all()
    report_progress(cfg.requests, cfg.requests)  # final line

    # Give Ranvier a moment to apply the final batch.
    time.sleep(0.1)
    metrics_after = scrape_metrics(cfg.ranvier_metrics_url)
    result.metrics_received = int(
        metrics_after.get(_M_RECEIVED, 0.0) - base_received
    )
    result.metrics_evictions_applied = int(
        metrics_after.get(_M_APPLIED, 0.0) - base_applied
    )
    result.metrics_evictions_stale = int(
        metrics_after.get(_M_STALE, 0.0) - base_stale
    )
    result.metrics_evictions_unknown = int(
        metrics_after.get(_M_UNKNOWN, 0.0) - base_unknown
    )

    return result


def parse_args(argv: List[str]) -> HarnessConfig:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ranvier-proxy-url", default="http://127.0.0.1:8080")
    p.add_argument("--ranvier-metrics-url", default="http://127.0.0.1:9180")
    p.add_argument(
        "--cache-events-url",
        default="http://127.0.0.1:8080/v1/cache/events",
    )
    p.add_argument("--mode", choices=("push", "silent"), required=True)
    p.add_argument("--capacity", type=int, default=512)
    p.add_argument("--population", type=int, default=4096)
    p.add_argument("--requests", type=int, default=10_000)
    p.add_argument("--zipf-s", type=float, default=1.1)
    p.add_argument("--batch-size", type=int, default=DEFAULT_BATCH_SIZE)
    p.add_argument("--seed", type=int, default=0xC0FFEE)
    p.add_argument(
        "--backend-ids",
        default="1",
        help="Comma-separated list of backend ids registered with Ranvier "
             "(default: '1'). The harness maintains one LRU model per id and "
             "consults the X-Backend-ID header from each response to decide "
             "which backend's cache to account against.",
    )
    p.add_argument("--request-token-count", type=int, default=128)
    p.add_argument("--output", default="bench-result.json")
    a = p.parse_args(argv)
    try:
        backend_ids = [int(x) for x in a.backend_ids.split(",") if x.strip()]
    except ValueError as e:
        p.error(f"--backend-ids must be a comma-separated int list: {e}")
    if not backend_ids:
        p.error("--backend-ids must contain at least one id")
    return HarnessConfig(
        ranvier_proxy_url=a.ranvier_proxy_url.rstrip("/"),
        ranvier_metrics_url=a.ranvier_metrics_url.rstrip("/"),
        cache_events_url=a.cache_events_url,
        mode=a.mode,
        capacity=a.capacity,
        population=a.population,
        requests=a.requests,
        zipf_s=a.zipf_s,
        batch_size=a.batch_size,
        seed=a.seed,
        backend_ids=backend_ids,
        request_token_count=a.request_token_count,
        output=a.output,
    )


def main(argv: Optional[List[str]] = None) -> int:
    cfg = parse_args(argv if argv is not None else sys.argv[1:])
    result = run_harness(cfg)
    payload = result.to_dict()
    payload["config"] = {
        "capacity": cfg.capacity,
        "population": cfg.population,
        "requests": cfg.requests,
        "zipf_s": cfg.zipf_s,
        "batch_size": cfg.batch_size,
        "seed": cfg.seed,
        "backend_ids": cfg.backend_ids,
    }
    with open(cfg.output, "w") as f:
        json.dump(payload, f, indent=2)
        f.write("\n")
    json.dump(payload, sys.stdout, indent=2)
    sys.stdout.write("\n")

    # Loud warning: if nothing was evicted, the workload never exercised
    # the eviction path, and the silent/push A/B cannot show a delta. This
    # is the single most common misconfiguration — small REQUESTS paired
    # with large CAPACITY means the per-backend LRU never fills.
    if result.eviction_count == 0:
        approx_misses_per_backend = (
            result.wasted_prefills // max(1, len(cfg.backend_ids))
        )
        sys.stderr.write(
            "\nWARNING: evictions_modeled=0 — no cache pressure was "
            "generated.\n"
            f"  capacity={cfg.capacity} per backend, "
            f"~{approx_misses_per_backend} misses/backend observed.\n"
            "  The LRU never overflowed, so no events were emitted and "
            "push/silent results will be identical.\n"
            "  Rule of thumb: the number of unique prefixes hitting each "
            "backend must exceed CAPACITY to\n"
            "  drive evictions. Lower --capacity or raise --requests "
            "until capacity < misses/backend.\n"
            "  For a quick smoke test at REQUESTS=1000 NUM_BACKENDS=2, "
            "try CAPACITY=64.\n\n"
        )
    elif cfg.mode == "push" and result.events_posted == 0:
        sys.stderr.write(
            "\nWARNING: mode=push but events_posted=0 — check for network "
            "or config errors.\n\n"
        )
    elif cfg.mode == "push" and result.metrics_received == 0:
        sys.stderr.write(
            "\nWARNING: mode=push posted "
            f"{result.events_posted} events but Ranvier's /metrics scrape\n"
            "  returned 0 for seastar_ranvier_cache_events_received_total.\n"
            "  Either the metrics endpoint isn't reachable at "
            f"{cfg.ranvier_metrics_url}/metrics,\n"
            "  or Ranvier was restarted between before/after scrapes. "
            "Check the Ranvier log.\n\n"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
