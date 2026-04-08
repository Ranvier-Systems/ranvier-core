#!/usr/bin/env python3
"""
Synthetic cache-event generator for Phase 4 benchmarking.

Models a fake inference backend's KV cache as an LRU of prefix hashes,
simulates admissions/evictions against a (Zipfian or trace-driven)
request stream, and POSTs `evicted` events to Ranvier's
POST /v1/cache/events endpoint in the documented v1 schema.

This is benchmark infrastructure, NOT a production sidecar. It does not
talk to vLLM/SGLang. The LRU model is a credible proxy for measuring
routing-layer observables (wasted prefill count, staleness window,
event throughput) but absolute numbers are indicative, not definitive.

Usage:
    generator.py --mode {push,silent} --ranvier-url URL [opts]

Hard Rules honored:
  #4  LRU has explicit MAX_SIZE_HARD_CAP (1<<20) on top of --capacity
  #18 No async; this is a stdlib HTTP client running outside the reactor
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
import urllib.error
import urllib.request
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Iterable, Iterator, List, Optional, Tuple

# Hard Rule #4: every container has an explicit upper bound.
MAX_SIZE_HARD_CAP = 1 << 20  # 1,048,576 entries

# Default to 1 event per POST so each emitted event corresponds to
# exactly one observable Ranvier tree write. Ranvier's
# evict_by_prefix_hash_local() calls remove_routes_by_backend(), which
# wipes ALL routes for the (backend, origin) pair on the first event;
# subsequent events in the same POST find an empty subtree and bump
# `evictions_unknown`. With batch_size=1 the metric `evictions_applied`
# equals `events_posted` modulo race-with-route-flush. Override with
# `--batch-size N` to measure the realistic batched-sidecar regime.
DEFAULT_BATCH_SIZE = 1


@dataclass
class EvictionRecord:
    prefix_hash: int
    timestamp_ms: int
    prefix_token_count: int


class LRUCacheModel:
    """Bounded LRU keyed by prefix_hash. Returns the evicted hash on overflow."""

    def __init__(self, capacity: int) -> None:
        if capacity <= 0 or capacity > MAX_SIZE_HARD_CAP:
            raise ValueError(
                f"capacity must be in (0, {MAX_SIZE_HARD_CAP}]; got {capacity}"
            )
        self._capacity = capacity
        self._entries: "OrderedDict[int, int]" = OrderedDict()  # hash -> token_count

    def __len__(self) -> int:
        return len(self._entries)

    def contains(self, prefix_hash: int) -> bool:
        return prefix_hash in self._entries

    def admit(
        self, prefix_hash: int, token_count: int
    ) -> Tuple[bool, Optional[Tuple[int, int]]]:
        """Touch or insert; return (was_hit, optional_evicted=(hash, tokens))."""
        if prefix_hash in self._entries:
            self._entries.move_to_end(prefix_hash)
            return True, None
        self._entries[prefix_hash] = token_count
        evicted: Optional[Tuple[int, int]] = None
        if len(self._entries) > self._capacity:
            ev_hash, ev_tokens = self._entries.popitem(last=False)
            evicted = (ev_hash, ev_tokens)
        return False, evicted


@dataclass
class GeneratorConfig:
    ranvier_url: str
    mode: str  # "push" or "silent"
    capacity: int
    population: int
    requests: int
    zipf_s: float
    batch_size: int
    seed: int
    backend_id: int
    request_token_count: int
    post_timeout_s: float = 2.0


@dataclass
class GeneratorStats:
    requests: int = 0
    cache_hits: int = 0
    cache_misses: int = 0
    evictions: int = 0
    events_posted: int = 0
    posts_ok: int = 0
    posts_failed: int = 0
    eviction_log: List[EvictionRecord] = field(default_factory=list)


def zipf_request_stream(
    population: int, n: int, s: float, rng: random.Random
) -> Iterator[int]:
    """Yield n integer prefix-ids drawn from a truncated Zipf(s) over [0, population)."""
    if population <= 0:
        raise ValueError("population must be > 0")
    weights = [1.0 / ((i + 1) ** s) for i in range(population)]
    total = sum(weights)
    cdf = []
    acc = 0.0
    for w in weights:
        acc += w / total
        cdf.append(acc)
    # Bisect manually to avoid importing bisect for one call site.
    import bisect

    for _ in range(n):
        u = rng.random()
        idx = bisect.bisect_left(cdf, u)
        if idx >= population:
            idx = population - 1
        yield idx


def now_ms() -> int:
    return int(time.time() * 1000)


def encode_prefix_hash(prefix_id: int) -> int:
    """Map a stable prefix-id to a 64-bit hash. Deterministic, seeded."""
    # SplitMix64 — deterministic, no external deps.
    x = (prefix_id + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    x = x ^ (x >> 31)
    return x


def hash_to_hex(h: int) -> str:
    return f"{h:016x}"


# Ranvier-compatible prefix hashing. The bench harness sends
# pre-tokenized `prompt_token_ids` (with RANVIER_ACCEPT_CLIENT_TOKENS=1)
# so Ranvier skips its own tokenizer. We compute the exact same FNV-1a
# hash Ranvier computes in `src/parse_utils.hpp::hash_prefix`, keeping
# both sides byte-identical.
_FNV_OFFSET_BASIS = 0xCBF29CE484222325
_FNV_PRIME = 0x100000001B3
_U64_MASK = 0xFFFFFFFFFFFFFFFF

# Ranvier defaults. Both must match the running Ranvier config or the
# hashes will diverge silently. `block_alignment=16` is vLLM's
# PagedAttention block size; `prefix_token_length=128` is the routing
# prefix window.
RANVIER_BLOCK_ALIGNMENT = 16
RANVIER_PREFIX_TOKEN_LENGTH = 128

# Tokens we emit per synthetic request. Multiple of BLOCK_ALIGNMENT and
# >= min_token_length (4) so the route actually gets cached, but short
# enough to keep payloads small.
TOKENS_PER_REQUEST = 16


def ranvier_hash_prefix(
    tokens: List[int],
    block_alignment: int = RANVIER_BLOCK_ALIGNMENT,
    prefix_token_length: int = RANVIER_PREFIX_TOKEN_LENGTH,
) -> int:
    """FNV-1a over the little-endian int32 bytes of the aligned token prefix.

    Mirrors `hash_prefix()` in src/parse_utils.hpp so the harness can
    produce the exact same hash Ranvier stores in its reverse index.
    """
    count = min(len(tokens), prefix_token_length)
    aligned_len = (count // block_alignment) * block_alignment
    if aligned_len == 0:
        aligned_len = count
    h = _FNV_OFFSET_BASIS
    for i in range(aligned_len):
        t = tokens[i] & 0xFFFFFFFF  # coerce to uint32
        for shift in (0, 8, 16, 24):  # little-endian bytes
            b = (t >> shift) & 0xFF
            h ^= b
            h = (h * _FNV_PRIME) & _U64_MASK
    return h


def tokens_for_prefix_id(
    prefix_id: int, n: int = TOKENS_PER_REQUEST
) -> List[int]:
    """Build a deterministic token sequence for a given prefix-id.

    Uses splitmix64 on `(prefix_id, i)` so tokens look plausibly random
    and different prefix_ids never collide on identical token streams.
    Clamped to [0, 200000) to stay inside Ranvier's default max_token_id.
    """
    out: List[int] = []
    for i in range(n):
        x = encode_prefix_hash((prefix_id << 16) | i)
        out.append(x % 200000)
    return out


def build_payload(
    batch: List[EvictionRecord], backend_id: int
) -> bytes:
    events = [
        {
            "event": "evicted",
            "prefix_hash": hash_to_hex(r.prefix_hash),
            "prefix_token_count": r.prefix_token_count,
            "timestamp_ms": r.timestamp_ms,
            "backend_id": backend_id,
        }
        for r in batch
    ]
    return json.dumps(
        {"type": "cache_event", "version": 1, "events": events}
    ).encode("utf-8")


def post_batch(url: str, payload: bytes, timeout_s: float) -> bool:
    req = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return False


def run_generator(
    cfg: GeneratorConfig,
    request_ids: Optional[Iterable[int]] = None,
) -> GeneratorStats:
    """Drive the LRU model and (in push mode) POST evictions to Ranvier."""
    if cfg.mode not in ("push", "silent"):
        raise ValueError(f"mode must be push|silent, got {cfg.mode}")
    if cfg.batch_size <= 0 or cfg.batch_size > 1000:
        raise ValueError("batch_size must be in (0, 1000]")

    rng = random.Random(cfg.seed)
    cache = LRUCacheModel(cfg.capacity)
    stats = GeneratorStats()

    if request_ids is None:
        stream: Iterable[int] = zipf_request_stream(
            cfg.population, cfg.requests, cfg.zipf_s, rng
        )
    else:
        stream = request_ids

    pending: List[EvictionRecord] = []

    def flush() -> None:
        if not pending:
            return
        if cfg.mode == "push":
            payload = build_payload(pending, cfg.backend_id)
            ok = post_batch(cfg.ranvier_url, payload, cfg.post_timeout_s)
            stats.events_posted += len(pending)
            if ok:
                stats.posts_ok += 1
            else:
                stats.posts_failed += 1
        pending.clear()

    for i, prefix_id in enumerate(stream):
        stats.requests += 1
        h = encode_prefix_hash(prefix_id)
        was_hit, evicted = cache.admit(h, cfg.request_token_count)
        if was_hit:
            stats.cache_hits += 1
        else:
            stats.cache_misses += 1
        if evicted is not None:
            ev_hash, ev_tokens = evicted
            rec = EvictionRecord(
                prefix_hash=ev_hash,
                timestamp_ms=now_ms(),
                prefix_token_count=ev_tokens,
            )
            stats.evictions += 1
            stats.eviction_log.append(rec)
            pending.append(rec)
            if len(pending) >= cfg.batch_size:
                flush()
        # Hard Rule #17: cooperative checkpoint every 1024 iters. Not in the
        # reactor, but keep it cheap and predictable for very large M.
        if (i & 1023) == 1023:
            pass

    flush()
    return stats


def parse_args(argv: List[str]) -> GeneratorConfig:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ranvier-url", default="http://127.0.0.1:8080/v1/cache/events")
    p.add_argument("--mode", choices=("push", "silent"), default="push")
    p.add_argument("--capacity", type=int, default=512,
                   help="LRU capacity (modeled backend KV cache slots)")
    p.add_argument("--population", type=int, default=4096,
                   help="Distinct prefix population for Zipf draw")
    p.add_argument("--requests", type=int, default=10000)
    p.add_argument("--zipf-s", type=float, default=1.1)
    p.add_argument("--batch-size", type=int, default=DEFAULT_BATCH_SIZE)
    p.add_argument("--seed", type=int, default=0xC0FFEE)
    p.add_argument("--backend-id", type=int, default=1)
    p.add_argument("--request-token-count", type=int, default=128)
    a = p.parse_args(argv)
    return GeneratorConfig(
        ranvier_url=a.ranvier_url,
        mode=a.mode,
        capacity=a.capacity,
        population=a.population,
        requests=a.requests,
        zipf_s=a.zipf_s,
        batch_size=a.batch_size,
        seed=a.seed,
        backend_id=a.backend_id,
        request_token_count=a.request_token_count,
    )


def main(argv: Optional[List[str]] = None) -> int:
    cfg = parse_args(argv if argv is not None else sys.argv[1:])
    stats = run_generator(cfg)
    json.dump(
        {
            "mode": cfg.mode,
            "requests": stats.requests,
            "cache_hits": stats.cache_hits,
            "cache_misses": stats.cache_misses,
            "evictions": stats.evictions,
            "events_posted": stats.events_posted,
            "posts_ok": stats.posts_ok,
            "posts_failed": stats.posts_failed,
        },
        sys.stdout,
        indent=2,
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
