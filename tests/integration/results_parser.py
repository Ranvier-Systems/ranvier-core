#!/usr/bin/env python3
"""
Unified Benchmark Results Parser for Ranvier

Commands:
  parse      Parse a benchmark log file to CSV/JSON
  summary    Show human-readable summary of results
  compare    Compare two benchmark results (baseline vs new)
  aggregate  Aggregate N repeat runs (median/IQR + refuse-to-conclude verdict)
  export     Export results to different formats

Usage:
    # Parse a log file (auto-detects type)
    ./results_parser.py parse benchmark.log -o stats.csv

    # Show summary of results
    ./results_parser.py summary stats.csv
    ./results_parser.py summary benchmark.log

    # Compare two results
    ./results_parser.py compare baseline.csv optimized.csv

    # Export to different formats
    ./results_parser.py export stats.csv --format markdown
    ./results_parser.py export stats.csv --format json

Examples:
    # Parse real vLLM benchmark output
    ./results_parser.py parse benchmark-reports/20250117_prefix/benchmark.log

    # Quick comparison of A/B test results
    ./results_parser.py compare baseline/benchmark.log optimized/benchmark.log

    # Generate markdown table for documentation
    ./results_parser.py export stats.csv --format markdown > results.md
"""

import argparse
import csv
import json
import re
import statistics
import sys
from dataclasses import dataclass, field, asdict
from datetime import datetime
from pathlib import Path
from typing import Optional, Dict, Any, List


# =============================================================================
# Per-bucket reporting guards
# =============================================================================
# TTFT below this floor is treated as a measurement artifact (empty SSE role
# delta or similar) rather than real model latency. 8B+ LLMs cannot physically
# produce a first real token this fast.
TTFT_FLOOR_MS = 100.0
# Buckets with fewer than this many samples do not produce meaningful P50/P99.
MIN_BUCKET_SAMPLES = 10
# Below this baseline, an improvement-% computation divides by near-zero and
# produces nonsense — report it as "baseline too low" instead.
MIN_IMPROVEMENT_DENOMINATOR_MS = 50.0
# Any improvement beyond this magnitude is always an artifact, never a real
# performance change. Display as ">cap"/"<-cap" and exclude from pp deltas.
IMPROVEMENT_CAP_PCT = 1000.0


# =============================================================================
# Data Classes
# =============================================================================

@dataclass
class BenchmarkResults:
    """Unified benchmark results structure."""
    # Metadata
    timestamp: str = ""
    source_file: str = ""
    benchmark_type: str = ""  # "mock" or "real"
    benchmark_mode: Optional[str] = None  # "prefix", "round_robin", etc.

    # TTFT Percentiles
    p50_ttft_ms: Optional[float] = None
    p75_ttft_ms: Optional[float] = None
    p90_ttft_ms: Optional[float] = None
    p95_ttft_ms: Optional[float] = None
    p99_ttft_ms: Optional[float] = None

    # Cache-specific metrics (real benchmarks only)
    cache_hits: Optional[int] = None
    cache_misses: Optional[int] = None
    cache_hit_rate_pct: Optional[float] = None
    ttft_cache_hit_p50_ms: Optional[float] = None
    ttft_cache_hit_p99_ms: Optional[float] = None
    ttft_cache_miss_p50_ms: Optional[float] = None
    ttft_cache_miss_p99_ms: Optional[float] = None
    ttft_improvement_pct: Optional[float] = None

    # Token throughput (real benchmarks only)
    total_prompt_tokens: Optional[int] = None
    total_completion_tokens: Optional[int] = None
    tokens_per_second: Optional[float] = None
    unique_prefixes: Optional[int] = None

    # Ranvier overhead metrics (from Prometheus) - P50/P99 percentiles
    routing_latency_p50_ms: Optional[float] = None
    routing_latency_p99_ms: Optional[float] = None
    tokenization_latency_p50_ms: Optional[float] = None
    tokenization_latency_p99_ms: Optional[float] = None
    primary_tokenization_latency_p50_ms: Optional[float] = None
    primary_tokenization_latency_p99_ms: Optional[float] = None
    boundary_detection_latency_p50_ms: Optional[float] = None
    boundary_detection_latency_p99_ms: Optional[float] = None
    art_lookup_latency_p50_ms: Optional[float] = None
    art_lookup_latency_p99_ms: Optional[float] = None
    connect_latency_p50_ms: Optional[float] = None
    connect_latency_p99_ms: Optional[float] = None

    # Per-bucket TTFT (real benchmarks only)
    ttft_large_hit_p50_ms: Optional[float] = None
    ttft_large_hit_p99_ms: Optional[float] = None
    ttft_large_miss_p50_ms: Optional[float] = None
    ttft_large_miss_p99_ms: Optional[float] = None
    ttft_large_improvement_pct: Optional[float] = None
    ttft_large_total_count: Optional[int] = None
    ttft_large_hit_count: Optional[int] = None
    ttft_large_miss_count: Optional[int] = None
    ttft_xlarge_hit_p50_ms: Optional[float] = None
    ttft_xlarge_hit_p99_ms: Optional[float] = None
    ttft_xlarge_miss_p50_ms: Optional[float] = None
    ttft_xlarge_miss_p99_ms: Optional[float] = None
    ttft_xlarge_improvement_pct: Optional[float] = None
    ttft_xlarge_total_count: Optional[int] = None
    ttft_xlarge_hit_count: Optional[int] = None
    ttft_xlarge_miss_count: Optional[int] = None

    # Standard Locust metrics
    total_requests: int = 0
    failed_requests: int = 0      # Actual errors (non-2xx, timeouts, parse errors)
    incomplete_requests: int = 0  # Got HTTP 200 but terminated before TTFT recorded
    incomplete_rate_pct: float = 0.0
    failure_rate_pct: float = 0.0
    avg_response_time_ms: Optional[float] = None
    requests_per_sec: float = 0.0

    # Prometheus routing counters (scraped at end-of-run from prometheus_metrics.txt
    # in the report dir). load_aware_fallbacks_total is the single counter that
    # distinguishes affinity-thrashing from other routing regressions; see
    # .dev-context/investigation-289-routing-regression.md.
    load_aware_fallbacks_total: Optional[int] = None
    # Cache-residency downgrades (#527): ART hits abandoned because the owning
    # backend's gossiped KV-cache residency fell below threshold. This is a
    # SECOND diversion mechanism distinct from load-aware fallbacks; a high
    # value here means residency routing (not load-aware) is breaking affinity.
    residency_route_downgrades_total: Optional[int] = None
    # Residency-signal health (#527): proves the feature is *live* vs merely
    # not-firing. A residency benchmark that shows 0 downgrades is meaningless
    # unless these confirm the signal is flowing. cache_states_received_total>0
    # and residency_cache_size>0 mean gossip + per-shard cache are populated; a
    # 0-downgrade run with healthy signal just means cache never crossed the
    # residency threshold (i.e. no cache pressure). Names verified at
    # gossip_service.cpp:123-126 and router_service.cpp:1748.
    cache_states_sent_total: Optional[int] = None
    cache_states_received_total: Optional[int] = None
    residency_cache_size: Optional[int] = None
    # Per-backend in-flight request gauge — printed as a sorted list so prefix-
    # concentration hot-spotting shows up directly in the comparison output.
    backend_active_requests: Optional[Dict[str, float]] = None

    # Validation
    sync_errors: int = 0
    validation_passed: bool = False
    p99_threshold_ms: Optional[float] = None

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary, filtering None values optionally."""
        return asdict(self)

    def to_csv_row(self) -> Dict[str, Any]:
        """Convert to CSV-friendly dictionary.

        Nested dict fields (e.g. backend_active_requests) are serialized as
        compact JSON so a single CSV row can survive a round trip without
        flattening the per-backend keys into columns.
        """
        row = {}
        for k, v in asdict(self).items():
            if v is None:
                row[k] = ""
            elif isinstance(v, dict):
                row[k] = json.dumps(v, sort_keys=True)
            else:
                row[k] = v
        return row


# =============================================================================
# Parsers
# =============================================================================

def detect_benchmark_type(content: str) -> str:
    """Auto-detect whether this is a mock or real vLLM benchmark."""
    # Real benchmarks have cache hit/miss tracking from locustfile_real.py
    if "Cache HIT" in content or "Cache MISS" in content:
        return "real"
    if "cache_hit_rate" in content.lower():
        return "real"
    # Note: BENCHMARK_STATS_JSON is emitted by both mock and real locustfiles,
    # so it is not a reliable signal for benchmark type.
    # Default to mock (simpler format)
    return "mock"


def parse_ttft_percentiles(content: str) -> Dict[str, Optional[float]]:
    """Parse TTFT percentile table from Locust output."""
    results = {
        "p50_ttft_ms": None,
        "p75_ttft_ms": None,
        "p90_ttft_ms": None,
        "p95_ttft_ms": None,
        "p99_ttft_ms": None,
    }

    # Pattern: GET      TTFT (Time To First Token)    55     57     58     58     61     64     67     68     72     72     72    422
    # Groups map to: 50%, 66%, 75%, 80%, 90%, 95%, 98%, 99%, 99.9%, 99.99%, 100%
    ttft_pattern = r"GET\s+TTFT \(Time To First Token\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    match = re.search(ttft_pattern, content)
    if match:
        results["p50_ttft_ms"] = float(match.group(1))
        results["p75_ttft_ms"] = float(match.group(3))
        results["p90_ttft_ms"] = float(match.group(5))
        results["p95_ttft_ms"] = float(match.group(6))
        results["p99_ttft_ms"] = float(match.group(8))

    return results


def parse_cache_ttft(content: str) -> Dict[str, Optional[float]]:
    """Parse cache-specific TTFT metrics."""
    results = {
        "ttft_cache_hit_p50_ms": None,
        "ttft_cache_hit_p99_ms": None,
        "ttft_cache_miss_p50_ms": None,
        "ttft_cache_miss_p99_ms": None,
    }

    # Cache hit pattern
    cache_hit_pattern = r"GET\s+TTFT \(Cache HIT\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    hit_match = re.search(cache_hit_pattern, content)
    if hit_match:
        results["ttft_cache_hit_p50_ms"] = float(hit_match.group(1))
        results["ttft_cache_hit_p99_ms"] = float(hit_match.group(8))

    # Cache miss pattern
    cache_miss_pattern = r"GET\s+TTFT \(Cache MISS\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    miss_match = re.search(cache_miss_pattern, content)
    if miss_match:
        results["ttft_cache_miss_p50_ms"] = float(miss_match.group(1))
        results["ttft_cache_miss_p99_ms"] = float(miss_match.group(8))

    return results


def parse_json_stats(content: str) -> Dict[str, Any]:
    """Parse JSON stats block from locustfile_real.py output."""
    results = {}

    # Find the JSON object after BENCHMARK_STATS_JSON:
    # Handle nested objects by finding balanced braces
    json_start = content.find("BENCHMARK_STATS_JSON:")
    if json_start == -1:
        return results

    json_start = content.find("{", json_start)
    if json_start == -1:
        return results

    # Find matching closing brace
    depth = 0
    json_end = json_start
    for i, char in enumerate(content[json_start:]):
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                json_end = json_start + i + 1
                break

    try:
        stats = json.loads(content[json_start:json_end])

        # Core metrics
        results["cache_hits"] = stats.get("cache_hits")
        results["cache_misses"] = stats.get("cache_misses")
        results["cache_hit_rate_pct"] = stats.get("cache_hit_rate_pct")
        results["ttft_improvement_pct"] = stats.get("ttft_improvement_pct")
        results["total_prompt_tokens"] = stats.get("total_prompt_tokens")
        results["total_completion_tokens"] = stats.get("total_completion_tokens")
        results["tokens_per_second"] = stats.get("tokens_per_second")
        results["unique_prefixes"] = stats.get("unique_prefixes")

        # Ranvier overhead metrics (P50/P99 percentiles)
        results["routing_latency_p50_ms"] = stats.get("routing_latency_p50_ms")
        results["routing_latency_p99_ms"] = stats.get("routing_latency_p99_ms")
        results["tokenization_latency_p50_ms"] = stats.get("tokenization_latency_p50_ms")
        results["tokenization_latency_p99_ms"] = stats.get("tokenization_latency_p99_ms")
        results["primary_tokenization_latency_p50_ms"] = stats.get("primary_tokenization_latency_p50_ms")
        results["primary_tokenization_latency_p99_ms"] = stats.get("primary_tokenization_latency_p99_ms")
        results["boundary_detection_latency_p50_ms"] = stats.get("boundary_detection_latency_p50_ms")
        results["boundary_detection_latency_p99_ms"] = stats.get("boundary_detection_latency_p99_ms")
        results["art_lookup_latency_p50_ms"] = stats.get("art_lookup_latency_p50_ms")
        results["art_lookup_latency_p99_ms"] = stats.get("art_lookup_latency_p99_ms")
        results["connect_latency_p50_ms"] = stats.get("connect_latency_p50_ms")
        results["connect_latency_p99_ms"] = stats.get("connect_latency_p99_ms")

        # Request counts
        results["total_requests"] = stats.get("total_requests")
        results["failed_requests"] = stats.get("failed_requests")
        results["incomplete_requests"] = stats.get("incomplete_requests")
        results["incomplete_rate_pct"] = stats.get("incomplete_rate_pct")

        # TTFT stats from JSON (more accurate)
        if stats.get("ttft_cache_hit_p50_ms"):
            results["ttft_cache_hit_p50_ms"] = stats["ttft_cache_hit_p50_ms"]
        if stats.get("ttft_cache_hit_p99_ms"):
            results["ttft_cache_hit_p99_ms"] = stats["ttft_cache_hit_p99_ms"]
        if stats.get("ttft_cache_miss_p50_ms"):
            results["ttft_cache_miss_p50_ms"] = stats["ttft_cache_miss_p50_ms"]
        if stats.get("ttft_cache_miss_p99_ms"):
            results["ttft_cache_miss_p99_ms"] = stats["ttft_cache_miss_p99_ms"]

        # Per-bucket stats
        bucket_stats = stats.get("bucket_stats", {})
        for bucket_name in ["large", "xlarge"]:
            if bucket_name in bucket_stats:
                b = bucket_stats[bucket_name]
                if bucket_name == "large":
                    results["ttft_large_hit_p50_ms"] = b.get("cache_hit_ttft_p50_ms")
                    results["ttft_large_hit_p99_ms"] = b.get("cache_hit_ttft_p99_ms")
                    results["ttft_large_miss_p50_ms"] = b.get("cache_miss_ttft_p50_ms")
                    results["ttft_large_miss_p99_ms"] = b.get("cache_miss_ttft_p99_ms")
                    results["ttft_large_improvement_pct"] = b.get("ttft_improvement_pct")
                    results["ttft_large_total_count"] = b.get("requests")
                    results["ttft_large_hit_count"] = b.get("cache_hit_count")
                    results["ttft_large_miss_count"] = b.get("cache_miss_count")
                elif bucket_name == "xlarge":
                    results["ttft_xlarge_hit_p50_ms"] = b.get("cache_hit_ttft_p50_ms")
                    results["ttft_xlarge_hit_p99_ms"] = b.get("cache_hit_ttft_p99_ms")
                    results["ttft_xlarge_miss_p50_ms"] = b.get("cache_miss_ttft_p50_ms")
                    results["ttft_xlarge_miss_p99_ms"] = b.get("cache_miss_ttft_p99_ms")
                    results["ttft_xlarge_improvement_pct"] = b.get("ttft_improvement_pct")
                    results["ttft_xlarge_total_count"] = b.get("requests")
                    results["ttft_xlarge_hit_count"] = b.get("cache_hit_count")
                    results["ttft_xlarge_miss_count"] = b.get("cache_miss_count")

    except json.JSONDecodeError:
        pass

    return results


def parse_cache_stats_text(content: str) -> Dict[str, Any]:
    """Parse cache statistics from human-readable text output."""
    results = {}

    # Cache Hits: 5393
    hits_match = re.search(r"Cache Hits:\s*(\d+)", content)
    if hits_match:
        results["cache_hits"] = int(hits_match.group(1))

    # Cache Misses: 117
    misses_match = re.search(r"Cache Misses:\s*(\d+)", content)
    if misses_match:
        results["cache_misses"] = int(misses_match.group(1))

    # Cache Hit Rate: 97.9%
    rate_match = re.search(r"Cache Hit Rate:\s*([0-9.]+)%", content)
    if rate_match:
        results["cache_hit_rate_pct"] = float(rate_match.group(1))

    # Unique Prefixes: 117
    prefixes_match = re.search(r"Unique Prefixes:\s*(\d+)", content)
    if prefixes_match:
        results["unique_prefixes"] = int(prefixes_match.group(1))

    # TTFT Improvement: -13.2% (from TTFT Comparison section)
    improvement_match = re.search(r"TTFT Improvement:\s*(-?[0-9.]+)%", content)
    if improvement_match:
        results["ttft_improvement_pct"] = float(improvement_match.group(1))

    # Cache Hit P50: 459.9ms
    hit_p50_match = re.search(r"Cache Hit P50:\s*([0-9.]+)ms", content)
    if hit_p50_match:
        results["ttft_cache_hit_p50_ms"] = float(hit_p50_match.group(1))

    # Cache Hit P99: 607.4ms
    hit_p99_match = re.search(r"Cache Hit P99:\s*([0-9.]+)ms", content)
    if hit_p99_match:
        results["ttft_cache_hit_p99_ms"] = float(hit_p99_match.group(1))

    # Cache Miss P50: 406.3ms
    miss_p50_match = re.search(r"Cache Miss P50:\s*([0-9.]+)ms", content)
    if miss_p50_match:
        results["ttft_cache_miss_p50_ms"] = float(miss_p50_match.group(1))

    # Cache Miss P99: 1040.7ms
    miss_p99_match = re.search(r"Cache Miss P99:\s*([0-9.]+)ms", content)
    if miss_p99_match:
        results["ttft_cache_miss_p99_ms"] = float(miss_p99_match.group(1))

    return results


def parse_bucket_ttft(content: str) -> Dict[str, Any]:
    """Parse per-bucket TTFT from the benchmark summary table.

    Example table (raw output):
      Bucket         Reqs        P50        P99    Hit P50   Miss P50    Improv%
      --------------------------------------------------------------------
      large          1818    460.1ms    509.0ms    460.1ms    562.1ms      18.2%
      xlarge         2210    514.8ms    623.7ms    514.7ms    691.2ms      25.5%

    Example with logger prefix:
      [2026-01-18 02:16:06,560] 5b62eb0e5595/INFO/locustfile_real:   large          1818    460.1ms    509.0ms    460.1ms    562.1ms      18.2%
    """
    results = {}

    # Pattern for bucket row: bucket_name  reqs  p50  p99  hit_p50  miss_p50  improvement
    # Don't anchor to start of line - log lines may have logger prefix
    # Match: (large|xlarge) followed by digits (reqs), then ms values, then percentage
    # Values may be actual numbers or "N/A"
    bucket_pattern = r"\b(large|xlarge)\s+(\d+)\s+([0-9.]+ms|N/A)\s+([0-9.]+ms|N/A)\s+([0-9.]+ms|N/A)\s+([0-9.]+ms|N/A)\s+(-?[0-9.]+%|N/A)"

    for line in content.split("\n"):
        match = re.search(bucket_pattern, line)
        if match:
            bucket = match.group(1)
            # Groups: 1=bucket, 2=reqs, 3=p50, 4=p99, 5=hit_p50, 6=miss_p50, 7=improvement

            total_count = int(match.group(2))

            # Parse hit_p50 (group 5)
            hit_p50_str = match.group(5)
            hit_p50 = float(hit_p50_str.replace("ms", "")) if hit_p50_str != "N/A" else None

            # Parse miss_p50 (group 6)
            miss_p50_str = match.group(6)
            miss_p50 = float(miss_p50_str.replace("ms", "")) if miss_p50_str != "N/A" else None

            # Parse improvement (group 7)
            improv_str = match.group(7)
            improvement = float(improv_str.replace("%", "")) if improv_str != "N/A" else None

            if bucket == "large":
                results["ttft_large_hit_p50_ms"] = hit_p50
                results["ttft_large_miss_p50_ms"] = miss_p50
                results["ttft_large_improvement_pct"] = improvement
                results["ttft_large_total_count"] = total_count
            elif bucket == "xlarge":
                results["ttft_xlarge_hit_p50_ms"] = hit_p50
                results["ttft_xlarge_miss_p50_ms"] = miss_p50
                results["ttft_xlarge_improvement_pct"] = improvement
                results["ttft_xlarge_total_count"] = total_count

    # Also parse the highlighted summary lines for large/xlarge improvements
    # Example: "Large Prefix (2000-4000 tokens) TTFT Improvement: 18.2%"
    large_improv_match = re.search(r"Large Prefix.*TTFT Improvement:\s*(-?[0-9.]+)%", content)
    if large_improv_match and not results.get("ttft_large_improvement_pct"):
        results["ttft_large_improvement_pct"] = float(large_improv_match.group(1))

    xlarge_improv_match = re.search(r"XLarge Prefix.*TTFT Improvement:\s*(-?[0-9.]+)%", content)
    if xlarge_improv_match and not results.get("ttft_xlarge_improvement_pct"):
        results["ttft_xlarge_improvement_pct"] = float(xlarge_improv_match.group(1))

    return results


def parse_aggregated_stats(content: str) -> Dict[str, Any]:
    """Parse Locust aggregated statistics.

    Example formats:
      Standard:   Aggregated    1263  282(22.33%) |     38       0      72     53 |    3.09        0.69
      With 0%:    Aggregated    5510    0(0.00%)  |     58      31     124    148 |    9.18        3.71
      Logger:     [timestamp] .../INFO/...: Aggregated    5510    0(0.00%) ...
    """
    results = {
        "total_requests": 0,
        "failed_requests": 0,
        "failure_rate_pct": 0.0,
        "avg_response_time_ms": None,
        "requests_per_sec": 0.0,
    }

    # Pattern: Aggregated    1263  282(22.33%) |     38       0      72     53 |    3.09        0.69
    # Don't anchor - may have logger prefix
    agg_pattern = r"Aggregated\s+(\d+)\s+(\d+)\(([0-9.]+)%\)\s+\|\s+(\d+)"
    match = re.search(agg_pattern, content)
    if match:
        results["total_requests"] = int(match.group(1))
        results["failed_requests"] = int(match.group(2))
        results["failure_rate_pct"] = float(match.group(3))
        results["avg_response_time_ms"] = float(match.group(4))

    # Parse requests per second from aggregated line
    # Use last occurrence (final stats, not intermediate)
    for line in content.split("\n"):
        line = line.rstrip()  # Remove trailing \r and whitespace
        if "Aggregated" in line:
            # Try to find RPS at end of line after the last pipe
            rps_match = re.search(r"\|\s+([0-9.]+)\s+[0-9.]+$", line)
            if rps_match:
                results["requests_per_sec"] = float(rps_match.group(1))
            # Don't break - use last Aggregated line (final stats)

    # If we didn't find aggregated stats, try alternative formats
    if results["total_requests"] == 0:
        # Try parsing from custom summary output
        # Total Requests: 5510
        total_match = re.search(r"Total Requests:\s*(\d+)", content)
        if total_match:
            results["total_requests"] = int(total_match.group(1))

        # Failed (errors): 0
        failed_match = re.search(r"Failed \(errors\):\s*(\d+)", content)
        if failed_match:
            results["failed_requests"] = int(failed_match.group(1))

        # Incomplete (timeout): 0
        incomplete_match = re.search(r"Incomplete \(timeout\):\s*(\d+)", content)
        if incomplete_match:
            results["incomplete_requests"] = int(incomplete_match.group(1))

        # Calculate failure rate and incomplete rate
        if results["total_requests"] > 0:
            results["failure_rate_pct"] = (results["failed_requests"] / results["total_requests"]) * 100
            results["incomplete_rate_pct"] = (results.get("incomplete_requests", 0) / results["total_requests"]) * 100

        # Requests/Second: 9.18
        rps_match = re.search(r"Requests/Second:\s*([0-9.]+)", content)
        if rps_match:
            results["requests_per_sec"] = float(rps_match.group(1))

    # Also try to get total from benchmark stats
    if results["total_requests"] == 0:
        # From BENCHMARK_STATS_JSON or similar
        json_pattern = r'"total_requests":\s*(\d+)'
        json_match = re.search(json_pattern, content)
        if json_match:
            results["total_requests"] = int(json_match.group(1))

    return results


# Prometheus counter / gauge names verified against src/metrics_service.hpp:
#   - "routing_load_aware_fallbacks_total" : counter (metrics_service.hpp:123)
#   - "backend_active_requests"            : per-backend gauge labelled by backend_id
#                                            (metrics_service.hpp:999, label backend_id=...)
# Both are exposed in the "ranvier" metric group; Prometheus serializes the group
# name as a prefix.
#
# IMPORTANT — Seastar is shard-per-core, and its Prometheus protocol emits one
# series PER SHARD with a `shard="N"` label, e.g.:
#   ranvier_routing_load_aware_fallbacks_total{shard="0"} 1234
#   ranvier_routing_load_aware_fallbacks_total{shard="1"} 1190
#   ranvier_backend_active_requests{shard="0",backend_id="3"} 5
# so we must (a) tolerate an optional label block after the metric name and
# (b) SUM across shards (counters) / aggregate per backend_id (gauges). The
# label order is not guaranteed, hence the permissive `{...}` matching.
#
# A per-backend `requests_routed_total` counter does NOT currently exist; the
# closest signal is `ranvier_backend_active_requests` (instantaneous in-flight
# count) and the per-backend latency histogram count
# (`ranvier_backend_latency_seconds_count{backend_id=...}`), which we scrape as
# a cheap approximation of "requests dispatched per backend." Mark these as
# verified-via-source if the user asks.
#
# Seastar's Prometheus exposition prepends "seastar_" to the group name, so the
# wire names are actually "seastar_ranvier_<metric>". We match an OPTIONAL
# "seastar_" prefix so the parser works against both the raw /metrics dump and
# any pre-stripped variant.
_PROM_FALLBACK_RE = re.compile(
    r"^(?:seastar_)?ranvier_routing_load_aware_fallbacks_total(?:\{[^}]*\})?\s+([0-9.eE+-]+)"
)
# Cache-residency downgrade counter (#527), verified at router_service.cpp:1742
# (make_counter "router_residency_route_downgrades_total", "ranvier" group).
_PROM_RESIDENCY_RE = re.compile(
    r"^(?:seastar_)?ranvier_router_residency_route_downgrades_total(?:\{[^}]*\})?\s+([0-9.eE+-]+)"
)
# Residency-signal health (#527): proves the feature is live even when downgrades=0.
# gossip counters verified at gossip_service.cpp:123-126; gauge at router_service.cpp:1748.
_PROM_CACHE_STATES_SENT_RE = re.compile(
    r"^(?:seastar_)?ranvier_gossip_cache_states_sent_total(?:\{[^}]*\})?\s+([0-9.eE+-]+)"
)
_PROM_CACHE_STATES_RECV_RE = re.compile(
    r"^(?:seastar_)?ranvier_gossip_cache_states_received_total(?:\{[^}]*\})?\s+([0-9.eE+-]+)"
)
_PROM_RESIDENCY_CACHE_SIZE_RE = re.compile(
    r"^(?:seastar_)?ranvier_router_residency_cache_size(?:\{[^}]*\})?\s+([0-9.eE+-]+)"
)
# NOTE: backend_active_requests is an instantaneous gauge — it reads ~0 when
# scraped after traffic has drained at end-of-run, so it is a poor distribution
# signal. The cumulative histogram count below is preferred (see caller).
_PROM_BACKEND_ACTIVE_RE = re.compile(
    r'^(?:seastar_)?ranvier_backend_active_requests\{[^}]*backend_id="([^"]+)"[^}]*\}\s+([0-9.eE+-]+)'
)
_PROM_BACKEND_HIST_COUNT_RE = re.compile(
    r'^(?:seastar_)?ranvier_backend_latency_seconds_count\{[^}]*backend_id="([^"]+)"[^}]*\}\s+([0-9.eE+-]+)'
)


def parse_prometheus_dump(prom_path: str) -> Dict[str, Any]:
    """Scrape routing counters from a Prometheus text exposition dump.

    Expected file: the body of an HTTP GET against the Ranvier /metrics endpoint
    captured at end-of-run. See bench.sh for the curl invocation. Missing file
    is non-fatal — returns an empty dict and the caller leaves the fields as
    None so the comparison output makes the absence visible.

    Seastar emits per-shard series; counters are summed across shards and
    per-backend gauges are summed per backend_id.
    """
    out: Dict[str, Any] = {}
    try:
        with open(prom_path, "r") as f:
            content = f.read()
    except (FileNotFoundError, IsADirectoryError, PermissionError):
        return out

    fallbacks: Optional[float] = None  # summed across shards
    residency: Optional[float] = None  # summed across shards
    cs_sent: Optional[float] = None    # summed across shards
    cs_recv: Optional[float] = None    # summed across shards
    res_cache_size: Optional[float] = None  # max across shards (per-shard gauge)
    active: Dict[str, float] = {}      # summed per backend_id across shards
    routed_total: Dict[str, float] = {}
    for line in content.splitlines():
        if not line or line.startswith("#"):
            continue
        m = _PROM_FALLBACK_RE.match(line)
        if m:
            try:
                fallbacks = (fallbacks or 0.0) + float(m.group(1))
            except ValueError:
                pass
            continue
        m = _PROM_RESIDENCY_RE.match(line)
        if m:
            try:
                residency = (residency or 0.0) + float(m.group(1))
            except ValueError:
                pass
            continue
        m = _PROM_CACHE_STATES_SENT_RE.match(line)
        if m:
            try:
                cs_sent = (cs_sent or 0.0) + float(m.group(1))
            except ValueError:
                pass
            continue
        m = _PROM_CACHE_STATES_RECV_RE.match(line)
        if m:
            try:
                cs_recv = (cs_recv or 0.0) + float(m.group(1))
            except ValueError:
                pass
            continue
        m = _PROM_RESIDENCY_CACHE_SIZE_RE.match(line)
        if m:
            try:
                # Per-shard gauge ("backends tracked"); take the max as the
                # representative coverage rather than summing across shards.
                v = float(m.group(1))
                res_cache_size = v if res_cache_size is None else max(res_cache_size, v)
            except ValueError:
                pass
            continue
        m = _PROM_BACKEND_ACTIVE_RE.match(line)
        if m:
            try:
                active[m.group(1)] = active.get(m.group(1), 0.0) + float(m.group(2))
            except ValueError:
                pass
            continue
        m = _PROM_BACKEND_HIST_COUNT_RE.match(line)
        if m:
            try:
                routed_total[m.group(1)] = routed_total.get(m.group(1), 0.0) + float(m.group(2))
            except ValueError:
                pass
            continue

    if fallbacks is not None:
        out["load_aware_fallbacks_total"] = int(fallbacks)
    if residency is not None:
        out["residency_route_downgrades_total"] = int(residency)
    if cs_sent is not None:
        out["cache_states_sent_total"] = int(cs_sent)
    if cs_recv is not None:
        out["cache_states_received_total"] = int(cs_recv)
    if res_cache_size is not None:
        out["residency_cache_size"] = int(res_cache_size)
    if active:
        out["backend_active_requests"] = active
    if routed_total:
        # Returned alongside to enrich the per-backend distribution block.
        out["backend_routed_total"] = routed_total
    return out


def parse_benchmark_log(filepath: str, benchmark_type: Optional[str] = None) -> BenchmarkResults:
    """Parse a benchmark log file and return structured results."""
    with open(filepath, "r") as f:
        content = f.read()

    # Auto-detect type if not specified
    if benchmark_type is None:
        benchmark_type = detect_benchmark_type(content)

    results = BenchmarkResults(
        timestamp=datetime.now().isoformat(),
        source_file=filepath,
        benchmark_type=benchmark_type,
    )

    # Parse TTFT percentiles (common to both types)
    ttft = parse_ttft_percentiles(content)
    results.p50_ttft_ms = ttft["p50_ttft_ms"]
    results.p75_ttft_ms = ttft["p75_ttft_ms"]
    results.p90_ttft_ms = ttft["p90_ttft_ms"]
    results.p95_ttft_ms = ttft["p95_ttft_ms"]
    results.p99_ttft_ms = ttft["p99_ttft_ms"]

    # Parse aggregated stats (common to both types)
    agg = parse_aggregated_stats(content)
    results.total_requests = agg["total_requests"]
    results.failed_requests = agg["failed_requests"]
    results.failure_rate_pct = agg["failure_rate_pct"]
    results.avg_response_time_ms = agg["avg_response_time_ms"]
    results.requests_per_sec = agg["requests_per_sec"]

    # Parse JSON stats block (emitted by both mock and real locustfiles)
    json_stats = parse_json_stats(content)

    # Ranvier overhead metrics from Prometheus histograms (available for both types)
    results.routing_latency_p50_ms = json_stats.get("routing_latency_p50_ms")
    results.routing_latency_p99_ms = json_stats.get("routing_latency_p99_ms")
    results.tokenization_latency_p50_ms = json_stats.get("tokenization_latency_p50_ms")
    results.tokenization_latency_p99_ms = json_stats.get("tokenization_latency_p99_ms")
    results.primary_tokenization_latency_p50_ms = json_stats.get("primary_tokenization_latency_p50_ms")
    results.primary_tokenization_latency_p99_ms = json_stats.get("primary_tokenization_latency_p99_ms")
    results.boundary_detection_latency_p50_ms = json_stats.get("boundary_detection_latency_p50_ms")
    results.boundary_detection_latency_p99_ms = json_stats.get("boundary_detection_latency_p99_ms")
    results.art_lookup_latency_p50_ms = json_stats.get("art_lookup_latency_p50_ms")
    results.art_lookup_latency_p99_ms = json_stats.get("art_lookup_latency_p99_ms")
    results.connect_latency_p50_ms = json_stats.get("connect_latency_p50_ms")
    results.connect_latency_p99_ms = json_stats.get("connect_latency_p99_ms")

    # Real benchmark specific parsing (cache stats, token counts, etc.)
    if benchmark_type == "real":
        # Cache TTFT
        cache_ttft = parse_cache_ttft(content)
        results.ttft_cache_hit_p50_ms = cache_ttft["ttft_cache_hit_p50_ms"]
        results.ttft_cache_hit_p99_ms = cache_ttft["ttft_cache_hit_p99_ms"]
        results.ttft_cache_miss_p50_ms = cache_ttft["ttft_cache_miss_p50_ms"]
        results.ttft_cache_miss_p99_ms = cache_ttft["ttft_cache_miss_p99_ms"]

        # Cache and token stats from JSON
        results.cache_hits = json_stats.get("cache_hits")
        results.cache_misses = json_stats.get("cache_misses")
        results.cache_hit_rate_pct = json_stats.get("cache_hit_rate_pct")
        results.ttft_improvement_pct = json_stats.get("ttft_improvement_pct")
        results.total_prompt_tokens = json_stats.get("total_prompt_tokens")
        results.total_completion_tokens = json_stats.get("total_completion_tokens")
        results.tokens_per_second = json_stats.get("tokens_per_second")
        results.unique_prefixes = json_stats.get("unique_prefixes")

        # Override cache TTFT from JSON if available
        if json_stats.get("ttft_cache_hit_p50_ms"):
            results.ttft_cache_hit_p50_ms = json_stats["ttft_cache_hit_p50_ms"]
        if json_stats.get("ttft_cache_hit_p99_ms"):
            results.ttft_cache_hit_p99_ms = json_stats["ttft_cache_hit_p99_ms"]
        if json_stats.get("ttft_cache_miss_p50_ms"):
            results.ttft_cache_miss_p50_ms = json_stats["ttft_cache_miss_p50_ms"]
        if json_stats.get("ttft_cache_miss_p99_ms"):
            results.ttft_cache_miss_p99_ms = json_stats["ttft_cache_miss_p99_ms"]

        # Override total_requests from JSON if aggregated stats returned 0
        if json_stats.get("total_requests") and results.total_requests == 0:
            results.total_requests = json_stats["total_requests"]
        if json_stats.get("failed_requests") is not None and results.failed_requests == 0:
            results.failed_requests = json_stats["failed_requests"]

        # Incomplete requests (always from JSON, not in Locust aggregated stats)
        if json_stats.get("incomplete_requests") is not None:
            results.incomplete_requests = json_stats["incomplete_requests"]
        if json_stats.get("incomplete_rate_pct") is not None:
            results.incomplete_rate_pct = json_stats["incomplete_rate_pct"]

        # Recalculate failure rate if we have valid counts but rate is 0
        if results.total_requests > 0 and results.failure_rate_pct == 0.0 and results.failed_requests > 0:
            results.failure_rate_pct = (results.failed_requests / results.total_requests) * 100

        # Per-bucket stats from JSON (priority source).
        # Counts are authoritative — take them whenever present, even when
        # some P50 values are missing (e.g. bucket with zero cache hits).
        if json_stats.get("ttft_large_total_count") is not None:
            results.ttft_large_total_count = json_stats.get("ttft_large_total_count")
            results.ttft_large_hit_count = json_stats.get("ttft_large_hit_count")
            results.ttft_large_miss_count = json_stats.get("ttft_large_miss_count")
        if json_stats.get("ttft_large_hit_p50_ms"):
            results.ttft_large_hit_p50_ms = json_stats["ttft_large_hit_p50_ms"]
            results.ttft_large_hit_p99_ms = json_stats.get("ttft_large_hit_p99_ms")
            results.ttft_large_miss_p50_ms = json_stats.get("ttft_large_miss_p50_ms")
            results.ttft_large_miss_p99_ms = json_stats.get("ttft_large_miss_p99_ms")
            results.ttft_large_improvement_pct = json_stats.get("ttft_large_improvement_pct")

        if json_stats.get("ttft_xlarge_total_count") is not None:
            results.ttft_xlarge_total_count = json_stats.get("ttft_xlarge_total_count")
            results.ttft_xlarge_hit_count = json_stats.get("ttft_xlarge_hit_count")
            results.ttft_xlarge_miss_count = json_stats.get("ttft_xlarge_miss_count")
        if json_stats.get("ttft_xlarge_hit_p50_ms"):
            results.ttft_xlarge_hit_p50_ms = json_stats["ttft_xlarge_hit_p50_ms"]
            results.ttft_xlarge_hit_p99_ms = json_stats.get("ttft_xlarge_hit_p99_ms")
            results.ttft_xlarge_miss_p50_ms = json_stats.get("ttft_xlarge_miss_p50_ms")
            results.ttft_xlarge_miss_p99_ms = json_stats.get("ttft_xlarge_miss_p99_ms")
            results.ttft_xlarge_improvement_pct = json_stats.get("ttft_xlarge_improvement_pct")

        # Parse text-based cache stats (fallback/override for JSON)
        text_stats = parse_cache_stats_text(content)
        if text_stats.get("cache_hits") and not results.cache_hits:
            results.cache_hits = text_stats["cache_hits"]
        if text_stats.get("cache_misses") and not results.cache_misses:
            results.cache_misses = text_stats["cache_misses"]
        if text_stats.get("cache_hit_rate_pct") and not results.cache_hit_rate_pct:
            results.cache_hit_rate_pct = text_stats["cache_hit_rate_pct"]
        if text_stats.get("unique_prefixes") and not results.unique_prefixes:
            results.unique_prefixes = text_stats["unique_prefixes"]
        if text_stats.get("ttft_improvement_pct") and not results.ttft_improvement_pct:
            results.ttft_improvement_pct = text_stats["ttft_improvement_pct"]
        # Override cache TTFT from text if not set
        if text_stats.get("ttft_cache_hit_p50_ms") and not results.ttft_cache_hit_p50_ms:
            results.ttft_cache_hit_p50_ms = text_stats["ttft_cache_hit_p50_ms"]
        if text_stats.get("ttft_cache_hit_p99_ms") and not results.ttft_cache_hit_p99_ms:
            results.ttft_cache_hit_p99_ms = text_stats["ttft_cache_hit_p99_ms"]
        if text_stats.get("ttft_cache_miss_p50_ms") and not results.ttft_cache_miss_p50_ms:
            results.ttft_cache_miss_p50_ms = text_stats["ttft_cache_miss_p50_ms"]
        if text_stats.get("ttft_cache_miss_p99_ms") and not results.ttft_cache_miss_p99_ms:
            results.ttft_cache_miss_p99_ms = text_stats["ttft_cache_miss_p99_ms"]

        # Parse per-bucket TTFT from text (fallback if JSON didn't have it)
        bucket_stats = parse_bucket_ttft(content)
        if bucket_stats.get("ttft_large_hit_p50_ms") and not results.ttft_large_hit_p50_ms:
            results.ttft_large_hit_p50_ms = bucket_stats["ttft_large_hit_p50_ms"]
        if bucket_stats.get("ttft_large_hit_p99_ms") and not results.ttft_large_hit_p99_ms:
            results.ttft_large_hit_p99_ms = bucket_stats["ttft_large_hit_p99_ms"]
        if bucket_stats.get("ttft_large_miss_p50_ms") and not results.ttft_large_miss_p50_ms:
            results.ttft_large_miss_p50_ms = bucket_stats["ttft_large_miss_p50_ms"]
        if bucket_stats.get("ttft_large_miss_p99_ms") and not results.ttft_large_miss_p99_ms:
            results.ttft_large_miss_p99_ms = bucket_stats["ttft_large_miss_p99_ms"]
        if bucket_stats.get("ttft_large_improvement_pct") and not results.ttft_large_improvement_pct:
            results.ttft_large_improvement_pct = bucket_stats["ttft_large_improvement_pct"]
        if bucket_stats.get("ttft_xlarge_hit_p50_ms") and not results.ttft_xlarge_hit_p50_ms:
            results.ttft_xlarge_hit_p50_ms = bucket_stats["ttft_xlarge_hit_p50_ms"]
        if bucket_stats.get("ttft_xlarge_hit_p99_ms") and not results.ttft_xlarge_hit_p99_ms:
            results.ttft_xlarge_hit_p99_ms = bucket_stats["ttft_xlarge_hit_p99_ms"]
        if bucket_stats.get("ttft_xlarge_miss_p50_ms") and not results.ttft_xlarge_miss_p50_ms:
            results.ttft_xlarge_miss_p50_ms = bucket_stats["ttft_xlarge_miss_p50_ms"]
        if bucket_stats.get("ttft_xlarge_miss_p99_ms") and not results.ttft_xlarge_miss_p99_ms:
            results.ttft_xlarge_miss_p99_ms = bucket_stats["ttft_xlarge_miss_p99_ms"]
        if bucket_stats.get("ttft_xlarge_improvement_pct") and not results.ttft_xlarge_improvement_pct:
            results.ttft_xlarge_improvement_pct = bucket_stats["ttft_xlarge_improvement_pct"]
        if bucket_stats.get("ttft_large_total_count") and not results.ttft_large_total_count:
            results.ttft_large_total_count = bucket_stats["ttft_large_total_count"]
        if bucket_stats.get("ttft_xlarge_total_count") and not results.ttft_xlarge_total_count:
            results.ttft_xlarge_total_count = bucket_stats["ttft_xlarge_total_count"]

        # Benchmark mode
        mode_match = re.search(r"Benchmark Mode: (\w+)", content)
        if mode_match:
            results.benchmark_mode = mode_match.group(1)

    # Prometheus dump (optional; sibling file in report dir). Missing dump is
    # fine — the comparison output will show "N/A" and the load_aware_fallbacks
    # row will be absent, which is itself a signal to the operator that the
    # counter wasn't captured.
    prom_path = Path(filepath).parent / "prometheus_metrics.txt"
    prom = parse_prometheus_dump(str(prom_path))
    if "load_aware_fallbacks_total" in prom:
        results.load_aware_fallbacks_total = prom["load_aware_fallbacks_total"]
    if "residency_route_downgrades_total" in prom:
        results.residency_route_downgrades_total = prom["residency_route_downgrades_total"]
    if "cache_states_sent_total" in prom:
        results.cache_states_sent_total = prom["cache_states_sent_total"]
    if "cache_states_received_total" in prom:
        results.cache_states_received_total = prom["cache_states_received_total"]
    if "residency_cache_size" in prom:
        results.residency_cache_size = prom["residency_cache_size"]
    # Prefer per-backend histogram counts (cumulative dispatched requests) if
    # available; fall back to the active-requests gauge (instantaneous in-flight)
    # otherwise. Either is usable for spotting hot-spotting.
    if "backend_routed_total" in prom:
        results.backend_active_requests = prom["backend_routed_total"]
    elif "backend_active_requests" in prom:
        results.backend_active_requests = prom["backend_active_requests"]

    # Sync errors
    sync_match = re.search(r"(\d+) new sync errors", content)
    if sync_match:
        results.sync_errors = int(sync_match.group(1))

    # Validation
    if "BENCHMARK PASSED" in content:
        results.validation_passed = True
    elif "BENCHMARK FAILED" in content:
        results.validation_passed = False

    # P99 threshold
    threshold_match = re.search(r"P99 Latency Threshold: ([0-9.]+)ms", content)
    if threshold_match:
        results.p99_threshold_ms = float(threshold_match.group(1))

    return results


def load_csv_results(filepath: str) -> BenchmarkResults:
    """Load benchmark results from a CSV file."""
    with open(filepath, "r") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        if not rows:
            raise ValueError(f"No data in {filepath}")
        data = rows[0]

    results = BenchmarkResults(source_file=filepath)

    # Dict fields are serialized as JSON in to_csv_row(); recover them here.
    DICT_FIELDS = {"backend_active_requests"}

    # Map CSV fields to dataclass
    for key in asdict(results).keys():
        if key in data and data[key]:
            val = data[key]
            if key in DICT_FIELDS:
                try:
                    setattr(results, key, json.loads(val))
                except (json.JSONDecodeError, TypeError):
                    pass
                continue
            current = getattr(results, key)
            if isinstance(current, bool):
                setattr(results, key, val.lower() == "true")
            elif isinstance(current, int):
                try:
                    setattr(results, key, int(float(val)))
                except ValueError:
                    pass
            elif isinstance(current, float):
                try:
                    setattr(results, key, float(val))
                except ValueError:
                    pass
            elif current is None:
                # Try to infer type
                try:
                    setattr(results, key, float(val))
                except ValueError:
                    setattr(results, key, val)
            else:
                setattr(results, key, val)

    return results


# =============================================================================
# Output Functions
# =============================================================================

def write_csv(results: BenchmarkResults, output_path: str):
    """Write results to CSV file."""
    with open(output_path, "w", newline="") as f:
        row = results.to_csv_row()
        writer = csv.DictWriter(f, fieldnames=row.keys())
        writer.writeheader()
        writer.writerow(row)


def write_json(results: BenchmarkResults, output_path: str):
    """Write results to JSON file."""
    with open(output_path, "w") as f:
        json.dump(results.to_dict(), f, indent=2)


def format_markdown_table(results: BenchmarkResults) -> str:
    """Format results as a markdown table."""
    lines = ["| Metric | Value |", "|--------|-------|"]

    metrics = [
        ("Benchmark Type", results.benchmark_type),
        ("Benchmark Mode", results.benchmark_mode),
        ("P50 TTFT (ms)", results.p50_ttft_ms),
        ("P75 TTFT (ms)", results.p75_ttft_ms),
        ("P90 TTFT (ms)", results.p90_ttft_ms),
        ("P95 TTFT (ms)", results.p95_ttft_ms),
        ("P99 TTFT (ms)", results.p99_ttft_ms),
        ("Cache Hit Rate (%)", results.cache_hit_rate_pct),
        ("Cache Hits", results.cache_hits),
        ("Cache Misses", results.cache_misses),
        ("TTFT Cache Hit P50 (ms)", results.ttft_cache_hit_p50_ms),
        ("TTFT Cache Hit P99 (ms)", results.ttft_cache_hit_p99_ms),
        ("TTFT Cache Miss P50 (ms)", results.ttft_cache_miss_p50_ms),
        ("TTFT Cache Miss P99 (ms)", results.ttft_cache_miss_p99_ms),
        ("TTFT Improvement (%)", results.ttft_improvement_pct),
        ("Tokens/Second", results.tokens_per_second),
        ("Total Requests", results.total_requests),
        ("Failed Requests", results.failed_requests),
        ("Failure Rate (%)", results.failure_rate_pct),
        ("Requests/Second", results.requests_per_sec),
        ("Sync Errors", results.sync_errors),
        ("Validation Passed", results.validation_passed),
    ]

    for name, value in metrics:
        if value is not None:
            if isinstance(value, float):
                lines.append(f"| {name} | {value:.2f} |")
            else:
                lines.append(f"| {name} | {value} |")

    return "\n".join(lines)


def print_summary(results: BenchmarkResults):
    """Print a human-readable summary of results."""
    print("\n" + "=" * 60)
    print("BENCHMARK RESULTS SUMMARY")
    print("=" * 60)

    print(f"\nSource: {results.source_file}")
    print(f"Type: {results.benchmark_type}")
    if results.benchmark_mode:
        print(f"Mode: {results.benchmark_mode}")

    if results.benchmark_type == "real" and results.cache_hit_rate_pct is not None:
        print("\nCache Performance:")
        print(f"  Cache Hit Rate: {results.cache_hit_rate_pct:.1f}%")
        if results.cache_hits is not None:
            print(f"  Cache Hits: {results.cache_hits}")
        if results.cache_misses is not None:
            print(f"  Cache Misses: {results.cache_misses}")

    print("\nTTFT Latency:")
    if results.p50_ttft_ms is not None:
        print(f"  P50: {results.p50_ttft_ms:.1f}ms")
    if results.p99_ttft_ms is not None:
        print(f"  P99: {results.p99_ttft_ms:.1f}ms")

    if results.ttft_cache_hit_p50_ms is not None:
        print(f"  Cache Hit P50: {results.ttft_cache_hit_p50_ms:.1f}ms")
    if results.ttft_cache_miss_p50_ms is not None:
        print(f"  Cache Miss P50: {results.ttft_cache_miss_p50_ms:.1f}ms")
    if results.ttft_improvement_pct is not None:
        print(f"  Improvement (Hit vs Miss): {results.ttft_improvement_pct:.1f}%")

    print("\nThroughput:")
    if results.tokens_per_second is not None:
        print(f"  Tokens/Second: {results.tokens_per_second:.1f}")
    print(f"  Requests/Second: {results.requests_per_sec:.2f}")
    print(f"  Total Requests: {results.total_requests}")
    print(f"  Failed Requests: {results.failed_requests}")

    print("\nValidation:")
    print(f"  Sync Errors: {results.sync_errors}")
    print(f"  Passed: {'Yes' if results.validation_passed else 'No'}")

    print("=" * 60)


# =============================================================================
# Comparison Functions
# =============================================================================

def _sanitize_bucket_ttft(value: Optional[float], count: Optional[int]) -> Optional[float]:
    """Drop per-bucket TTFT values that are measurement artifacts or have too few samples.

    Returns None when:
      - value is missing
      - the sample count (hit_count for hit metrics, miss_count for miss metrics)
        is known and below MIN_BUCKET_SAMPLES
      - the value is below TTFT_FLOOR_MS (physically impossible for an 8B+ LLM)
    """
    if value is None:
        return None
    if count is not None and count < MIN_BUCKET_SAMPLES:
        return None
    if value < TTFT_FLOOR_MS:
        return None
    return value


def _sample_tag(baseline_count: Optional[int], new_count: Optional[int]) -> str:
    """Format a ``[n=baseline/new]`` tag, or empty string when both counts are unknown.

    Accepts floats (CSV round-trip may widen ints) and always renders as integers.
    """
    def _fmt(v):
        if v is None:
            return "?"
        try:
            return str(int(v))
        except (TypeError, ValueError):
            return str(v)
    if baseline_count is None and new_count is None:
        return ""
    return f" [n={_fmt(baseline_count)}/{_fmt(new_count)}]"


def _format_improvement_pct(pct: Optional[float]) -> str:
    """Format a per-run improvement percentage, collapsing extreme artifacts to a cap marker."""
    if pct is None:
        return "N/A"
    if pct > IMPROVEMENT_CAP_PCT:
        return f">+{int(IMPROVEMENT_CAP_PCT)}%"
    if pct < -IMPROVEMENT_CAP_PCT:
        return f"<-{int(IMPROVEMENT_CAP_PCT)}%"
    return f"{pct:.1f}"


def format_change(
    baseline: Optional[float],
    new: Optional[float],
    lower_is_better: bool = True,
    is_improvement_pct: bool = False
) -> str:
    """Format the change between two values with percentage and direction.

    Args:
        baseline: The baseline value
        new: The new value to compare
        lower_is_better: If True, decreases are BETTER. If False, increases are BETTER.
        is_improvement_pct: If True, this metric is itself an improvement percentage,
            so going from negative to positive is always BETTER.
    """
    if baseline is None or new is None:
        return "N/A"

    diff = new - baseline

    # Special handling for improvement percentages (e.g., TTFT improvement)
    # Show percentage point change, not percent-of-percent
    if is_improvement_pct:
        sign = "+" if diff > 0 else ""
        # Determine if change is better/worse (higher improvement % is better)
        if diff > 1:
            indicator = "BETTER"
        elif diff < -1:
            indicator = "WORSE"
        else:
            indicator = "SAME"
        return f"{sign}{diff:.2f}pp ({indicator})"

    if baseline == 0:
        if new == 0:
            return "0 (--)"
        return f"{new:.2f} (NEW)"

    pct = (diff / baseline) * 100

    # Cap percentage changes that are always measurement artifacts.
    if abs(pct) > IMPROVEMENT_CAP_PCT:
        cap_str = f">+{int(IMPROVEMENT_CAP_PCT)}%" if pct > 0 else f"<-{int(IMPROVEMENT_CAP_PCT)}%"
        sign = "+" if diff > 0 else ""
        return f"{sign}{diff:.2f} ({cap_str}) ARTIFACT"

    if lower_is_better:
        if pct < -1:
            indicator = "BETTER"
        elif pct > 1:
            indicator = "WORSE"
        else:
            indicator = "SAME"
    else:
        if pct > 1:
            indicator = "BETTER"
        elif pct < -1:
            indicator = "WORSE"
        else:
            indicator = "SAME"

    sign = "+" if diff > 0 else ""
    return f"{sign}{diff:.2f} ({sign}{pct:.1f}%) {indicator}"


def _max_incomplete_rate(*runs: BenchmarkResults) -> float:
    """Return the larger of the two incomplete-rate values for honest P99 captioning."""
    return max((r.incomplete_rate_pct or 0.0) for r in runs)


def compare_results(baseline: BenchmarkResults, new: BenchmarkResults) -> str:
    """Compare two benchmark results and return formatted comparison."""
    lines = []
    lines.append("=" * 80)
    lines.append("BENCHMARK COMPARISON: Round-Robin vs Prefix-Aware")
    lines.append("=" * 80)
    lines.append(f"Baseline (Round-Robin): {baseline.source_file}")
    lines.append(f"New (Prefix-Aware):     {new.source_file}")
    lines.append("")

    # VALIDATION + INCOMPLETE-RATE BANNER (top of report).
    # Surfacing this above the metrics blocks is intentional: a green P99
    # alongside a non-zero timeout rate is the exact metric-blindness pattern
    # that hid the May 22 2026 13B/20u regression. See
    # .dev-context/investigation-may22-affinity-thrashing-reproduction.md.
    lines.append("-" * 80)
    lines.append("RUN STATUS (read this first)")
    lines.append("-" * 80)
    val_b = "PASSED" if baseline.validation_passed else "FAILED"
    val_n = "PASSED" if new.validation_passed else "FAILED"
    lines.append(f"  Validation:   {val_b} -> {val_n}")
    inc_b = baseline.incomplete_rate_pct or 0.0
    inc_n = new.incomplete_rate_pct or 0.0
    inc_warn = ""
    if max(inc_b, inc_n) > 0.0:
        inc_warn = "   *** TIMEOUTS PRESENT — P99 figures below EXCLUDE incompletes ***"
    lines.append(
        f"  Incompletes:  baseline {baseline.incomplete_requests} ({inc_b:.1f}%)  "
        f"new {new.incomplete_requests} ({inc_n:.1f}%){inc_warn}"
    )
    lines.append("")

    # KEY METRICS - Cache hit rate is the most important comparison
    lines.append("-" * 80)
    lines.append("KEY METRICS (Cache Efficiency)")
    lines.append("-" * 80)

    cache_metrics = [
        ("cache_hit_rate_pct", "Cache Hit Rate (%)", False),
        ("cache_hits", "Cache Hits", False),
        ("cache_misses", "Cache Misses", True),
        ("unique_prefixes", "Unique Prefixes", None),
    ]

    lines.append(f"{'Metric':<25} {'Baseline':>12} {'New':>12} {'Change':>30}")
    for key, name, lower_is_better in cache_metrics:
        baseline_val = getattr(baseline, key, None)
        new_val = getattr(new, key, None)
        if baseline_val is None and new_val is None:
            continue
        baseline_str = f"{baseline_val:.1f}" if baseline_val is not None else "N/A"
        new_str = f"{new_val:.1f}" if new_val is not None else "N/A"
        if lower_is_better is not None:
            change_str = format_change(baseline_val, new_val, lower_is_better)
        else:
            change_str = ""
        lines.append(f"{name:<25} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    # PER-BUCKET TTFT - This is where the real improvement shows
    lines.append("")
    lines.append("-" * 80)
    lines.append("PER-BUCKET TTFT IMPROVEMENT (Large Prefixes)")
    lines.append("-" * 80)
    lines.append("  (Prefix-aware routing benefits large prefixes most)")
    lines.append(
        f"  Guards: floor={TTFT_FLOOR_MS:.0f}ms, min-samples={MIN_BUCKET_SAMPLES}, "
        f"min-denominator={MIN_IMPROVEMENT_DENOMINATOR_MS:.0f}ms, cap=±{int(IMPROVEMENT_CAP_PCT)}%"
    )
    lines.append("")
    lines.append(f"{'Metric':<40} {'Baseline':>12} {'New':>12} {'Change':>30}")

    has_bucket_data = False
    for bucket in ("large", "xlarge"):
        title = bucket.capitalize()

        b_total = getattr(baseline, f"ttft_{bucket}_total_count", None)
        n_total = getattr(new, f"ttft_{bucket}_total_count", None)
        b_hit_c = getattr(baseline, f"ttft_{bucket}_hit_count", None)
        n_hit_c = getattr(new, f"ttft_{bucket}_hit_count", None)
        b_miss_c = getattr(baseline, f"ttft_{bucket}_miss_count", None)
        n_miss_c = getattr(new, f"ttft_{bucket}_miss_count", None)

        b_hit_p50 = getattr(baseline, f"ttft_{bucket}_hit_p50_ms", None)
        n_hit_p50 = getattr(new, f"ttft_{bucket}_hit_p50_ms", None)
        b_hit_p99 = getattr(baseline, f"ttft_{bucket}_hit_p99_ms", None)
        n_hit_p99 = getattr(new, f"ttft_{bucket}_hit_p99_ms", None)
        b_miss_p50 = getattr(baseline, f"ttft_{bucket}_miss_p50_ms", None)
        n_miss_p50 = getattr(new, f"ttft_{bucket}_miss_p50_ms", None)
        b_miss_p99 = getattr(baseline, f"ttft_{bucket}_miss_p99_ms", None)
        n_miss_p99 = getattr(new, f"ttft_{bucket}_miss_p99_ms", None)
        b_improv = getattr(baseline, f"ttft_{bucket}_improvement_pct", None)
        n_improv = getattr(new, f"ttft_{bucket}_improvement_pct", None)

        if all(v is None for v in (b_hit_p50, n_hit_p50, b_miss_p50, n_miss_p50,
                                   b_hit_p99, n_hit_p99, b_miss_p99, n_miss_p99,
                                   b_total, n_total)):
            continue
        has_bucket_data = True

        def _int_str(v):
            if v is None:
                return "?"
            try:
                return str(int(v))
            except (TypeError, ValueError):
                return str(v)

        b_total_str = _int_str(b_total)
        n_total_str = _int_str(n_total)
        lines.append(f"{title} bucket  (baseline n={b_total_str}, new n={n_total_str})")

        # Whole-bucket insufficient data shortcut.
        b_below = (b_total is not None and b_total < MIN_BUCKET_SAMPLES)
        n_below = (n_total is not None and n_total < MIN_BUCKET_SAMPLES)
        if b_below or n_below:
            side = []
            if b_below:
                side.append(f"baseline n={_int_str(b_total)}")
            if n_below:
                side.append(f"new n={_int_str(n_total)}")
            lines.append(
                f"  insufficient data ({', '.join(side)}; need >= {MIN_BUCKET_SAMPLES})"
            )
            lines.append("")
            continue

        # Sanitize each percentile against TTFT_FLOOR_MS and per-side sample counts.
        sb_hit_p50 = _sanitize_bucket_ttft(b_hit_p50, b_hit_c)
        sn_hit_p50 = _sanitize_bucket_ttft(n_hit_p50, n_hit_c)
        sb_hit_p99 = _sanitize_bucket_ttft(b_hit_p99, b_hit_c)
        sn_hit_p99 = _sanitize_bucket_ttft(n_hit_p99, n_hit_c)
        sb_miss_p50 = _sanitize_bucket_ttft(b_miss_p50, b_miss_c)
        sn_miss_p50 = _sanitize_bucket_ttft(n_miss_p50, n_miss_c)
        sb_miss_p99 = _sanitize_bucket_ttft(b_miss_p99, b_miss_c)
        sn_miss_p99 = _sanitize_bucket_ttft(n_miss_p99, n_miss_c)

        hit_tag = _sample_tag(b_hit_c, n_hit_c)
        miss_tag = _sample_tag(b_miss_c, n_miss_c)
        rows = [
            (f"{title} Hit P50 (ms){hit_tag}",  sb_hit_p50,  sn_hit_p50),
            (f"{title} Hit P99 (ms){hit_tag}",  sb_hit_p99,  sn_hit_p99),
            (f"{title} Miss P50 (ms){miss_tag}", sb_miss_p50, sn_miss_p50),
            (f"{title} Miss P99 (ms){miss_tag}", sb_miss_p99, sn_miss_p99),
        ]
        for name, bv, nv in rows:
            if bv is None and nv is None:
                bv_str, nv_str, change_str = "N/A", "N/A", "insufficient data"
            else:
                bv_str = f"{bv:.1f}" if bv is not None else "N/A"
                nv_str = f"{nv:.1f}" if nv is not None else "N/A"
                change_str = format_change(bv, nv, True, False)
            lines.append(f"  {name:<38} {bv_str:>12} {nv_str:>12} {change_str:>30}")

        # Per-run improvement % row. The raw value is computed as
        #   (miss_p50 - hit_p50) / miss_p50 * 100
        # in locustfile_real.py, so it is unreliable whenever either
        # hit_p50 or miss_p50 fails sanitation (sub-floor or too few samples),
        # when the denominator miss_p50 is below MIN_IMPROVEMENT_DENOMINATOR_MS,
        # or when the magnitude exceeds IMPROVEMENT_CAP_PCT.
        def _guard_improv(
            raw: Optional[float],
            hit_sanitized: Optional[float],
            miss_sanitized: Optional[float],
            miss_raw: Optional[float],
        ) -> (Optional[float], Optional[str]):
            if raw is None:
                return None, None
            if abs(raw) > IMPROVEMENT_CAP_PCT:
                return None, "artifact"
            if hit_sanitized is None or miss_sanitized is None:
                return None, "artifact"
            if miss_raw is None or miss_raw < MIN_IMPROVEMENT_DENOMINATOR_MS:
                return None, "baseline too low"
            return raw, None

        b_improv_guarded, b_reason = _guard_improv(b_improv, sb_hit_p50, sb_miss_p50, b_miss_p50)
        n_improv_guarded, n_reason = _guard_improv(n_improv, sn_hit_p50, sn_miss_p50, n_miss_p50)

        if b_improv_guarded is None or n_improv_guarded is None:
            # Prefer "artifact" over "baseline too low" since it's the stronger signal.
            reasons = [r for r in (b_reason, n_reason) if r]
            change_str = "artifact" if "artifact" in reasons else (
                "baseline too low" if "baseline too low" in reasons else "N/A"
            )
            b_display = (_format_improvement_pct(b_improv_guarded)
                         if b_improv_guarded is not None
                         else ("N/A" if b_improv is None else _format_improvement_pct(b_improv)))
            n_display = (_format_improvement_pct(n_improv_guarded)
                         if n_improv_guarded is not None
                         else ("N/A" if n_improv is None else _format_improvement_pct(n_improv)))
            # When the raw value was an artifact, hide the misleading digits too.
            if b_improv_guarded is None and b_reason == "artifact" and b_improv is not None \
                    and abs(b_improv) <= IMPROVEMENT_CAP_PCT:
                b_display = "artifact"
            if n_improv_guarded is None and n_reason == "artifact" and n_improv is not None \
                    and abs(n_improv) <= IMPROVEMENT_CAP_PCT:
                n_display = "artifact"
        else:
            change_str = format_change(b_improv_guarded, n_improv_guarded, False, True)
            b_display = _format_improvement_pct(b_improv_guarded)
            n_display = _format_improvement_pct(n_improv_guarded)

        improv_name = f"{title} Improvement (%)"
        lines.append(f"  {improv_name:<38} {b_display:>12} {n_display:>12} {change_str:>30}")
        lines.append("")

    if not has_bucket_data:
        lines.append("  (No per-bucket data available)")

    # OVERALL TTFT
    lines.append("")
    lines.append("-" * 80)
    lines.append("OVERALL TTFT (All Request Sizes)")
    lines.append("-" * 80)
    lines.append("  (Aggregate may be misleading - see per-bucket for real impact)")
    max_inc = _max_incomplete_rate(baseline, new)
    if max_inc > 0.0:
        # Per-side captioning so the reader can't miss it. Locust excludes
        # incomplete (timed-out) requests from its TTFT percentile inputs, so
        # any P99 here is conditional on the run completing — comparing a
        # "fast" run with high timeouts to a "slow" run with none is comparing
        # different request populations.
        lines.append(
            f"  *** P99 TTFT (excl. timeouts: baseline {(baseline.incomplete_rate_pct or 0.0):.2f}%, "
            f"new {(new.incomplete_rate_pct or 0.0):.2f}%) ***"
        )
    lines.append("")

    ttft_metrics = [
        ("p50_ttft_ms", "P50 TTFT (ms)", True),
        ("p99_ttft_ms", "P99 TTFT (ms)", True),
        ("ttft_cache_hit_p50_ms", "Cache Hit P50 (ms)", True),
        ("ttft_cache_hit_p99_ms", "Cache Hit P99 (ms)", True),
        ("ttft_cache_miss_p50_ms", "Cache Miss P50 (ms)", True),
        ("ttft_cache_miss_p99_ms", "Cache Miss P99 (ms)", True),
    ]

    lines.append(f"{'Metric':<25} {'Baseline':>12} {'New':>12} {'Change':>30}")
    for key, name, lower_is_better in ttft_metrics:
        baseline_val = getattr(baseline, key, None)
        new_val = getattr(new, key, None)
        if baseline_val is None and new_val is None:
            continue
        baseline_str = f"{baseline_val:.1f}" if baseline_val is not None else "N/A"
        new_str = f"{new_val:.1f}" if new_val is not None else "N/A"
        change_str = format_change(baseline_val, new_val, lower_is_better)
        lines.append(f"{name:<25} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    # REQUEST STATS
    lines.append("")
    lines.append("-" * 80)
    lines.append("REQUEST STATISTICS")
    lines.append("-" * 80)

    request_metrics = [
        ("total_requests", "Total Requests", False),
        ("failed_requests", "Failed (errors)", True),
        ("incomplete_requests", "Incomplete (timeout)", True),
        ("incomplete_rate_pct", "Incomplete Rate (%)", True),
        ("failure_rate_pct", "Failure Rate (%)", True),
        ("requests_per_sec", "Requests/sec", False),
        ("sync_errors", "Sync Errors", True),
    ]

    lines.append(f"{'Metric':<25} {'Baseline':>12} {'New':>12} {'Change':>30}")
    for key, name, lower_is_better in request_metrics:
        baseline_val = getattr(baseline, key, None)
        new_val = getattr(new, key, None)
        if baseline_val is None and new_val is None:
            continue
        baseline_str = f"{baseline_val:.1f}" if baseline_val is not None else "N/A"
        new_str = f"{new_val:.1f}" if new_val is not None else "N/A"
        change_str = format_change(baseline_val, new_val, lower_is_better)
        lines.append(f"{name:<25} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    # RANVIER OVERHEAD (optional - only shown if data available)
    has_overhead = (baseline.routing_latency_p50_ms is not None or new.routing_latency_p50_ms is not None)
    if has_overhead:
        lines.append("")
        lines.append("-" * 80)
        lines.append("RANVIER OVERHEAD (from Prometheus metrics)")
        lines.append("-" * 80)

        overhead_metrics = [
            ("routing_latency_p50_ms", "Routing Decision P50 (ms)", True),
            ("routing_latency_p99_ms", "Routing Decision P99 (ms)", True),
            ("tokenization_latency_p50_ms", "  - Tokenization P50 (ms)", True),
            ("tokenization_latency_p99_ms", "  - Tokenization P99 (ms)", True),
            ("primary_tokenization_latency_p50_ms", "    - Primary P50 (ms)", True),
            ("primary_tokenization_latency_p99_ms", "    - Primary P99 (ms)", True),
            ("boundary_detection_latency_p50_ms", "    - Boundary Detect P50 (ms)", True),
            ("boundary_detection_latency_p99_ms", "    - Boundary Detect P99 (ms)", True),
            ("art_lookup_latency_p50_ms", "  - ART Lookup P50 (ms)", True),
            ("art_lookup_latency_p99_ms", "  - ART Lookup P99 (ms)", True),
            ("connect_latency_p50_ms", "Backend Connect P50 (ms)", True),
            ("connect_latency_p99_ms", "Backend Connect P99 (ms)", True),
        ]

        lines.append(f"{'Metric':<30} {'Baseline':>12} {'New':>12} {'Change':>30}")
        for key, name, lower_is_better in overhead_metrics:
            baseline_val = getattr(baseline, key, None)
            new_val = getattr(new, key, None)
            if baseline_val is None and new_val is None:
                continue
            baseline_str = f"{baseline_val:.2f}" if baseline_val is not None else "N/A"
            new_str = f"{new_val:.2f}" if new_val is not None else "N/A"
            change_str = format_change(baseline_val, new_val, lower_is_better)
            lines.append(f"{name:<30} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    # ROUTING COUNTERS (Prometheus) — affinity-thrashing fingerprint.
    # See .dev-context/investigation-289-routing-regression.md: the
    # load_aware_fallbacks_total counter is the single signal that
    # distinguishes affinity-thrashing from other regressions.
    has_routing_counters = (
        baseline.load_aware_fallbacks_total is not None
        or new.load_aware_fallbacks_total is not None
        or baseline.residency_route_downgrades_total is not None
        or new.residency_route_downgrades_total is not None
        or baseline.backend_active_requests
        or new.backend_active_requests
    )
    if has_routing_counters:
        lines.append("")
        lines.append("-" * 80)
        lines.append("ROUTING COUNTERS (from Prometheus /metrics dump)")
        lines.append("-" * 80)

        def _fmt_count(v):
            return "N/A" if v is None else str(int(v))

        # Load-aware fallback counter (cumulative since process start). High
        # values relative to total_requests indicate the load-aware logic is
        # firing constantly — investigation #289's "30u" or "flapping" failure
        # mode depending on which side the spike is on.
        b_fb = baseline.load_aware_fallbacks_total
        n_fb = new.load_aware_fallbacks_total
        b_fb_pct = (b_fb / baseline.total_requests * 100.0) if (b_fb and baseline.total_requests) else None
        n_fb_pct = (n_fb / new.total_requests * 100.0) if (n_fb and new.total_requests) else None
        b_fb_str = f"{_fmt_count(b_fb)}" + (f" ({b_fb_pct:.1f}%)" if b_fb_pct is not None else "")
        n_fb_str = f"{_fmt_count(n_fb)}" + (f" ({n_fb_pct:.1f}%)" if n_fb_pct is not None else "")
        lines.append(f"  load_aware_fallbacks_total:        baseline={b_fb_str}  new={n_fb_str}")
        lines.append("  (percent = fallbacks / total_requests; high % suggests load-aware affinity thrashing)")

        # Cache-residency downgrade counter (#527) — the OTHER diversion source.
        # If this is high while load_aware_fallbacks is low, residency routing
        # (not load-aware) is breaking prefix affinity. Both must be considered
        # when reading a 30u miss-tail regression.
        b_rd = baseline.residency_route_downgrades_total
        n_rd = new.residency_route_downgrades_total
        if b_rd is not None or n_rd is not None:
            b_rd_pct = (b_rd / baseline.total_requests * 100.0) if (b_rd and baseline.total_requests) else None
            n_rd_pct = (n_rd / new.total_requests * 100.0) if (n_rd and new.total_requests) else None
            b_rd_str = f"{_fmt_count(b_rd)}" + (f" ({b_rd_pct:.1f}%)" if b_rd_pct is not None else "")
            n_rd_str = f"{_fmt_count(n_rd)}" + (f" ({n_rd_pct:.1f}%)" if n_rd_pct is not None else "")
            lines.append(f"  residency_route_downgrades_total: baseline={b_rd_str}  new={n_rd_str}")
            lines.append("  (percent = downgrades / total_requests; high % means residency routing is diverting ART hits)")

        # Residency-signal health — distinguishes "feature inert because no cache
        # pressure" from "feature off / signal broken". If downgrades=0 but
        # received>0 and cache_size>0, the feature is LIVE and just never crossed
        # the residency threshold (need more cache pressure to exercise it).
        n_recv = new.cache_states_received_total
        n_sent = new.cache_states_sent_total
        n_size = new.residency_cache_size
        if n_recv is not None or n_sent is not None or n_size is not None:
            lines.append(
                f"  residency signal (new): cache_states sent={_fmt_count(n_sent)} "
                f"received={_fmt_count(n_recv)}, residency_cache_size={_fmt_count(n_size)}"
            )
            n_rd_v = new.residency_route_downgrades_total
            if (n_rd_v == 0) and ((n_recv or 0) > 0) and ((n_size or 0) > 0):
                lines.append(
                    "  -> residency LIVE but never fired: cache never crossed the threshold "
                    "(no cache pressure). Lower --gpu-mem-util or raise load to exercise it."
                )
            elif (n_recv == 0 or n_size == 0):
                lines.append(
                    "  -> residency signal NOT flowing (received/cache_size 0): check gossip / vLLM scrape "
                    "before trusting a residency A/B."
                )

        # Per-backend request distribution. Print as sorted list with min/max
        # and a Gini coefficient — hot-spotting from prefix concentration
        # shows up as a high Gini (>0.3 is suspicious on a uniform workload).
        def _gini(vals):
            n = len(vals)
            if n == 0:
                return None
            s = sum(vals)
            if s == 0:
                return 0.0
            srt = sorted(vals)
            cum = sum((i + 1) * v for i, v in enumerate(srt))
            return (2.0 * cum) / (n * s) - (n + 1.0) / n

        def _render_dist(label, dist):
            if not dist:
                lines.append(f"  {label}: (no per-backend data)")
                return
            try:
                pairs = sorted(dist.items(), key=lambda kv: int(kv[0]))
            except ValueError:
                pairs = sorted(dist.items())
            vals = [v for _, v in pairs]
            mn, mx = min(vals), max(vals)
            g = _gini(vals)
            list_str = ", ".join(f"b{k}={int(v) if v == int(v) else v:.1f}" for k, v in pairs)
            g_str = f"{g:.3f}" if g is not None else "N/A"
            lines.append(f"  {label}: min={mn:.0f} max={mx:.0f} gini={g_str}")
            lines.append(f"    [{list_str}]")

        _render_dist("baseline backend dist", baseline.backend_active_requests or {})
        _render_dist("new      backend dist", new.backend_active_requests or {})

    lines.append("")
    lines.append("-" * 80)

    # Summary
    lines.append("")
    lines.append("SUMMARY:")
    if new.cache_hit_rate_pct and baseline.cache_hit_rate_pct:
        improvement = new.cache_hit_rate_pct - baseline.cache_hit_rate_pct
        lines.append(f"  Cache Hit Rate: {baseline.cache_hit_rate_pct:.1f}% -> {new.cache_hit_rate_pct:.1f}% (+{improvement:.1f}%)")
    for bucket_key, bucket_label in (("large", "Large"), ("xlarge", "XLarge")):
        improv = getattr(new, f"ttft_{bucket_key}_improvement_pct", None)
        miss_p50 = getattr(new, f"ttft_{bucket_key}_miss_p50_ms", None)
        total = getattr(new, f"ttft_{bucket_key}_total_count", None)
        if improv is None:
            continue
        if total is not None and total < MIN_BUCKET_SAMPLES:
            lines.append(
                f"  {bucket_label} Prefix TTFT Improvement: insufficient data (n={total})"
            )
            continue
        if abs(improv) > IMPROVEMENT_CAP_PCT:
            lines.append(
                f"  {bucket_label} Prefix TTFT Improvement: {_format_improvement_pct(improv)} (artifact)"
            )
            continue
        if miss_p50 is not None and miss_p50 < MIN_IMPROVEMENT_DENOMINATOR_MS:
            lines.append(
                f"  {bucket_label} Prefix TTFT Improvement: N/A (baseline miss P50 too low: {miss_p50:.1f}ms)"
            )
            continue
        lines.append(f"  {bucket_label} Prefix TTFT Improvement: {improv:.1f}%")

    lines.append("=" * 80)

    return "\n".join(lines)


# =============================================================================
# CLI Commands
# =============================================================================

# =============================================================================
# Repeat-run aggregation
# =============================================================================
# The #1 documented confound in the benchmark history is run-to-run variance
# (identical back-to-back configs giving P99 -37% vs +31%; transient hot-spotting
# in 2-of-4 runs). A single run is not a result. These helpers aggregate N repeat
# runs of the SAME config into median/IQR and, crucially, REFUSE TO CONCLUDE when
# the middle 50% of the effect straddles zero — so the tooling reports "no reliable
# effect" instead of letting a human cherry-pick the best run into a markdown table.
# See .dev-context/benchmark-tooling-review-2026-07-05.md (F3) and BACKLOG §25.

# Metrics where a smaller value is better (orients the improvement/regression verdict).
_LOWER_IS_BETTER = {
    "p50_ttft_ms", "p75_ttft_ms", "p90_ttft_ms", "p95_ttft_ms", "p99_ttft_ms",
    "ttft_cache_hit_p50_ms", "ttft_cache_hit_p99_ms",
    "ttft_cache_miss_p50_ms", "ttft_cache_miss_p99_ms",
    "avg_response_time_ms", "incomplete_rate_pct", "failure_rate_pct",
}

# Numeric metrics summarized per repeat set. Explicit (not "every numeric field")
# so the table stays readable and stable across schema growth.
_AGG_METRICS = [
    "p50_ttft_ms", "p90_ttft_ms", "p95_ttft_ms", "p99_ttft_ms",
    "cache_hit_rate_pct", "ttft_cache_miss_p99_ms", "tokens_per_second",
    "requests_per_sec", "incomplete_rate_pct", "failure_rate_pct",
    "total_requests",
]

# Affinity-thrash hot-spot signature (paired A/B): the prefix arm's P99 is much
# worse than round-robin's DESPITE a high cache-hit rate — routing concentrated
# load onto a hot backend. Thresholds are deliberately loose flags, not gates.
_HOTSPOT_P99_FACTOR = 1.25      # prefix P99 > 1.25x the RR P99
_HOTSPOT_MIN_HIT_RATE = 80.0    # ...while cache hit rate >= 80%
# Single-arm outlier: a repeat whose value sits above Q3 by more than this many IQRs.
_OUTLIER_IQR_MULT = 1.5


def _quartiles(xs: List[float]):
    """(Q1, Q3) via the inclusive method. Degenerate but well-defined for n<=1."""
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return (None, None)
    if n == 1:
        return (s[0], s[0])
    q1, _q2, q3 = statistics.quantiles(s, n=4, method="inclusive")
    return (q1, q3)


def _summarize_metric(values: List[Optional[float]]) -> Optional[Dict[str, Any]]:
    """median/IQR/min/max/n over the non-None values, or None if nothing present."""
    xs = [float(v) for v in values if v is not None]
    if not xs:
        return None
    q1, q3 = _quartiles(xs)
    return {
        "n": len(xs),
        "median": statistics.median(xs),
        "q1": q1,
        "q3": q3,
        "iqr": (q3 - q1) if (q1 is not None and q3 is not None) else None,
        "min": min(xs),
        "max": max(xs),
    }


def _pct_change(baseline: Optional[float], new: Optional[float]) -> Optional[float]:
    """Signed percent change new-vs-baseline; None if not computable."""
    if baseline is None or new is None or baseline == 0:
        return None
    return (new - baseline) / abs(baseline) * 100.0


def aggregate_runs(runs: List[BenchmarkResults],
                   metrics: Optional[List[str]] = None) -> Dict[str, Any]:
    """Single-arm aggregation: per-metric median/IQR across N repeats of one config.

    Also flags per-repeat P99 outliers (value > Q3 + 1.5*IQR) — the transient
    hot-spot signature when repeats of the *same* arm disagree wildly.
    """
    metrics = metrics or _AGG_METRICS
    per_metric: Dict[str, Any] = {}
    for m in metrics:
        summ = _summarize_metric([getattr(r, m, None) for r in runs])
        if summ is not None:
            per_metric[m] = summ

    outliers = []
    p99s = [getattr(r, "p99_ttft_ms", None) for r in runs]
    p99_summ = per_metric.get("p99_ttft_ms")
    if p99_summ and p99_summ["n"] >= 3 and p99_summ["iqr"]:
        threshold = p99_summ["q3"] + _OUTLIER_IQR_MULT * p99_summ["iqr"]
        for i, v in enumerate(p99s):
            if v is not None and v > threshold:
                outliers.append({"repeat": i + 1, "p99_ttft_ms": v, "threshold": threshold})

    return {
        "mode": "single-arm",
        "n_repeats": len(runs),
        "metrics": per_metric,
        "p99_outliers": outliers,
    }


def aggregate_compare(baseline_runs: List[BenchmarkResults],
                      treatment_runs: List[BenchmarkResults],
                      discriminating_metric: str = "p99_ttft_ms") -> Dict[str, Any]:
    """Paired A/B aggregation: baseline[i] vs treatment[i] over N repeats.

    Computes the per-pair percent change of the discriminating metric, then the
    median and IQR of that change across repeats, and a verdict:
      - < 2 pairs                -> INSUFFICIENT DATA
      - IQR straddles zero       -> NO RELIABLE EFFECT (report, do not cherry-pick)
      - otherwise                -> improvement / regression by the median change
    Also flags per-pair affinity-thrash hot-spots (treatment P99 >> baseline P99
    at high hit rate).
    """
    n = min(len(baseline_runs), len(treatment_runs))
    pairs = list(zip(baseline_runs[:n], treatment_runs[:n]))

    deltas: List[float] = []
    per_pair = []
    hotspots = []
    for i, (base, treat) in enumerate(pairs):
        b = getattr(base, discriminating_metric, None)
        t = getattr(treat, discriminating_metric, None)
        d = _pct_change(b, t)
        per_pair.append({"repeat": i + 1, "baseline": b, "treatment": t, "pct_change": d})
        if d is not None:
            deltas.append(d)

        # Hot-spot: prefix (treatment) P99 much worse than RR (baseline) at high hit rate.
        bp99 = getattr(base, "p99_ttft_ms", None)
        tp99 = getattr(treat, "p99_ttft_ms", None)
        thit = getattr(treat, "cache_hit_rate_pct", None)
        if (bp99 and tp99 and thit is not None
                and tp99 > bp99 * _HOTSPOT_P99_FACTOR and thit >= _HOTSPOT_MIN_HIT_RATE):
            hotspots.append({"repeat": i + 1, "baseline_p99_ms": bp99,
                             "treatment_p99_ms": tp99, "cache_hit_rate_pct": thit})

    lower_better = discriminating_metric in _LOWER_IS_BETTER
    delta_summ = _summarize_metric(deltas)

    if delta_summ is None or delta_summ["n"] < 2:
        verdict = "INSUFFICIENT DATA (need >= 2 valid repeats)"
        reliable = False
    else:
        q1, q3 = delta_summ["q1"], delta_summ["q3"]
        if q1 <= 0 <= q3:
            verdict = "NO RELIABLE EFFECT (IQR spans zero)"
            reliable = False
        else:
            med = delta_summ["median"]
            improved = (med < 0) if lower_better else (med > 0)
            verdict = (f"{'IMPROVEMENT' if improved else 'REGRESSION'}: "
                       f"median {med:+.1f}% on {discriminating_metric}")
            reliable = True

    return {
        "mode": "paired-ab",
        "discriminating_metric": discriminating_metric,
        "lower_is_better": lower_better,
        "n_pairs": n,
        "delta_pct": delta_summ,
        "per_pair": per_pair,
        "hotspots": hotspots,
        "verdict": verdict,
        "reliable": reliable,
        "baseline": aggregate_runs(baseline_runs)["metrics"],
        "treatment": aggregate_runs(treatment_runs)["metrics"],
    }


def _fmt_stat(s: Optional[Dict[str, Any]]) -> str:
    if not s:
        return "n/a"
    iqr = "" if s["iqr"] is None else f", IQR {s['iqr']:.1f}"
    return f"median {s['median']:.1f} [{s['min']:.1f}..{s['max']:.1f}]{iqr} (n={s['n']})"


def format_aggregate(agg: Dict[str, Any]) -> str:
    """Human-readable rendering of an aggregate_runs / aggregate_compare result."""
    lines = []
    if agg["mode"] == "paired-ab":
        lines.append("=" * 72)
        lines.append(f"A/B AGGREGATE over {agg['n_pairs']} repeat(s) — "
                     f"discriminating metric: {agg['discriminating_metric']}")
        lines.append("=" * 72)
        lines.append(f"VERDICT: {agg['verdict']}")
        ds = agg["delta_pct"]
        if ds:
            lines.append(f"  {agg['discriminating_metric']} %change: {_fmt_stat(ds)}")
        lines.append("")
        lines.append("Per-repeat %change (treatment vs baseline):")
        for p in agg["per_pair"]:
            d = "n/a" if p["pct_change"] is None else f"{p['pct_change']:+.1f}%"
            lines.append(f"  repeat {p['repeat']}: {d}  "
                         f"(baseline={p['baseline']}, treatment={p['treatment']})")
        if agg["hotspots"]:
            lines.append("")
            lines.append(f"⚠ HOT-SPOT (affinity-thrash) flagged in {len(agg['hotspots'])} repeat(s):")
            for h in agg["hotspots"]:
                lines.append(f"  repeat {h['repeat']}: treatment P99 {h['treatment_p99_ms']:.0f}ms "
                             f"≫ baseline {h['baseline_p99_ms']:.0f}ms at "
                             f"{h['cache_hit_rate_pct']:.0f}% hit rate")
    else:
        lines.append("=" * 72)
        lines.append(f"AGGREGATE over {agg['n_repeats']} repeat(s)")
        lines.append("=" * 72)
        for m, s in agg["metrics"].items():
            lines.append(f"  {m:24s} {_fmt_stat(s)}")
        if agg["p99_outliers"]:
            lines.append("")
            lines.append(f"⚠ P99 OUTLIER in {len(agg['p99_outliers'])} repeat(s) "
                         f"(> Q3 + {_OUTLIER_IQR_MULT}·IQR):")
            for o in agg["p99_outliers"]:
                lines.append(f"  repeat {o['repeat']}: P99 {o['p99_ttft_ms']:.0f}ms "
                             f"(> {o['threshold']:.0f}ms)")
    return "\n".join(lines)


def _resolve_run_input(path: str) -> BenchmarkResults:
    """Accept a report DIR (uses <dir>/benchmark.log), a .csv, or a log file."""
    p = Path(path)
    if p.is_dir():
        log = p / "benchmark.log"
        if not log.exists():
            raise FileNotFoundError(f"no benchmark.log in report dir: {path}")
        return parse_benchmark_log(str(log))
    if path.endswith(".csv"):
        return load_csv_results(path)
    return parse_benchmark_log(path)


def cmd_parse(args):
    """Parse command: Parse a benchmark log file."""
    try:
        results = parse_benchmark_log(args.input, args.type)

        # Determine output path
        if args.output:
            output_path = args.output
        else:
            input_path = Path(args.input)
            output_path = str(input_path.with_suffix(".csv"))

        # Write output
        if args.format == "json":
            if not args.output:
                output_path = str(Path(args.input).with_suffix(".json"))
            write_json(results, output_path)
        else:
            write_csv(results, output_path)

        print(f"Parsed: {args.input}")
        print(f"Output: {output_path}")
        print(f"Type:   {results.benchmark_type}")

        if args.summary:
            print_summary(results)

    except FileNotFoundError:
        print(f"Error: File not found: {args.input}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error parsing: {e}", file=sys.stderr)
        return 1

    return 0


def cmd_summary(args):
    """Summary command: Show human-readable summary."""
    try:
        filepath = args.input

        # Detect if input is CSV or log
        if filepath.endswith(".csv"):
            results = load_csv_results(filepath)
        else:
            results = parse_benchmark_log(filepath, args.type)

        print_summary(results)

    except FileNotFoundError:
        print(f"Error: File not found: {args.input}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    return 0


def cmd_compare(args):
    """Compare command: Compare two benchmark results."""
    try:
        # Load baseline
        if args.baseline.endswith(".csv"):
            baseline = load_csv_results(args.baseline)
        else:
            baseline = parse_benchmark_log(args.baseline)

        # Load new
        if args.new.endswith(".csv"):
            new = load_csv_results(args.new)
        else:
            new = parse_benchmark_log(args.new)

        # Compare and print
        comparison = compare_results(baseline, new)
        print(comparison)

    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error comparing: {e}", file=sys.stderr)
        return 1

    return 0


def cmd_aggregate(args):
    """Aggregate command: median/IQR across N repeat runs, with a refuse-to-conclude verdict."""
    try:
        treatment = [_resolve_run_input(p) for p in args.inputs]
        if args.baseline:
            baseline = [_resolve_run_input(p) for p in args.baseline]
            if len(baseline) != len(treatment):
                print(f"Error: --baseline count ({len(baseline)}) must match "
                      f"inputs count ({len(treatment)}) for paired A/B aggregation",
                      file=sys.stderr)
                return 1
            agg = aggregate_compare(baseline, treatment, args.metric)
        else:
            agg = aggregate_runs(treatment)

        print(format_aggregate(agg))

        if args.json:
            with open(args.json, "w") as f:
                json.dump(agg, f, indent=2)
            print(f"\nWrote structured aggregate: {args.json}")

    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error aggregating: {e}", file=sys.stderr)
        return 1

    return 0


def cmd_export(args):
    """Export command: Export results to different formats."""
    try:
        # Load results
        if args.input.endswith(".csv"):
            results = load_csv_results(args.input)
        else:
            results = parse_benchmark_log(args.input, args.type)

        # Export based on format
        if args.format == "markdown":
            output = format_markdown_table(results)
            if args.output:
                with open(args.output, "w") as f:
                    f.write(output)
            else:
                print(output)

        elif args.format == "json":
            output = json.dumps(results.to_dict(), indent=2)
            if args.output:
                with open(args.output, "w") as f:
                    f.write(output)
            else:
                print(output)

        elif args.format == "csv":
            if args.output:
                write_csv(results, args.output)
            else:
                # Print CSV to stdout
                row = results.to_csv_row()
                writer = csv.DictWriter(sys.stdout, fieldnames=row.keys())
                writer.writeheader()
                writer.writerow(row)

    except FileNotFoundError:
        print(f"Error: File not found: {args.input}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error exporting: {e}", file=sys.stderr)
        return 1

    return 0


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Unified Benchmark Results Parser for Ranvier",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Parse a log file (auto-detects type)
    %(prog)s parse benchmark.log -o stats.csv

    # Show summary
    %(prog)s summary stats.csv

    # Compare two results
    %(prog)s compare baseline.csv optimized.csv

    # Aggregate 3 A/B repeats (RR baseline vs prefix), median-of-3 verdict
    %(prog)s aggregate prefix_rep{1,2,3}/ --baseline rr_rep{1,2,3}/ --metric p99_ttft_ms

    # Export to markdown
    %(prog)s export stats.csv --format markdown
""",
    )

    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # Parse command
    parse_parser = subparsers.add_parser("parse", help="Parse a benchmark log file")
    parse_parser.add_argument("input", help="Input log file")
    parse_parser.add_argument("-o", "--output", help="Output file path")
    parse_parser.add_argument("-t", "--type", choices=["mock", "real"], help="Benchmark type (auto-detected if not specified)")
    parse_parser.add_argument("-f", "--format", choices=["csv", "json"], default="csv", help="Output format (default: csv)")
    parse_parser.add_argument("-s", "--summary", action="store_true", help="Print summary after parsing")

    # Summary command
    summary_parser = subparsers.add_parser("summary", help="Show human-readable summary")
    summary_parser.add_argument("input", help="Input file (CSV or log)")
    summary_parser.add_argument("-t", "--type", choices=["mock", "real"], help="Benchmark type (for log files)")

    # Compare command
    compare_parser = subparsers.add_parser("compare", help="Compare two benchmark results")
    compare_parser.add_argument("baseline", help="Baseline result file (CSV or log)")
    compare_parser.add_argument("new", help="New result file (CSV or log)")

    # Aggregate command
    aggregate_parser = subparsers.add_parser(
        "aggregate",
        help="Aggregate N repeat runs (median/IQR, hot-spot flag, refuse-to-conclude verdict)",
    )
    aggregate_parser.add_argument(
        "inputs", nargs="+",
        help="Repeat runs of one config: report dirs, .csv, or benchmark.log files "
             "(the treatment arm when --baseline is given)",
    )
    aggregate_parser.add_argument(
        "--baseline", nargs="+",
        help="Paired control-arm repeats (same count as inputs) for A/B aggregation",
    )
    aggregate_parser.add_argument(
        "--metric", default="p99_ttft_ms",
        help="Discriminating metric for the A/B verdict (default: p99_ttft_ms)",
    )
    aggregate_parser.add_argument("--json", help="Write structured aggregate JSON to this path")

    # Export command
    export_parser = subparsers.add_parser("export", help="Export results to different formats")
    export_parser.add_argument("input", help="Input file (CSV or log)")
    export_parser.add_argument("-f", "--format", choices=["csv", "json", "markdown"], default="markdown", help="Output format (default: markdown)")
    export_parser.add_argument("-o", "--output", help="Output file (prints to stdout if not specified)")
    export_parser.add_argument("-t", "--type", choices=["mock", "real"], help="Benchmark type (for log files)")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    if args.command == "parse":
        return cmd_parse(args)
    elif args.command == "summary":
        return cmd_summary(args)
    elif args.command == "compare":
        return cmd_compare(args)
    elif args.command == "aggregate":
        return cmd_aggregate(args)
    elif args.command == "export":
        return cmd_export(args)

    return 0


if __name__ == "__main__":
    sys.exit(main())
