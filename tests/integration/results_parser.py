#!/usr/bin/env python3
"""
Unified Benchmark Results Parser for Ranvier

Commands:
  parse      Parse a benchmark log file to CSV/JSON
  summary    Show human-readable summary of results
  compare    Compare two benchmark results (baseline vs new)
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
import sys
from dataclasses import dataclass, field, asdict
from datetime import datetime
from pathlib import Path
from typing import Optional, Dict, Any, List


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
    ttft_xlarge_hit_p50_ms: Optional[float] = None
    ttft_xlarge_hit_p99_ms: Optional[float] = None
    ttft_xlarge_miss_p50_ms: Optional[float] = None
    ttft_xlarge_miss_p99_ms: Optional[float] = None
    ttft_xlarge_improvement_pct: Optional[float] = None

    # Standard Locust metrics
    total_requests: int = 0
    failed_requests: int = 0      # Actual errors (non-2xx, timeouts, parse errors)
    incomplete_requests: int = 0  # Got HTTP 200 but terminated before TTFT recorded
    incomplete_rate_pct: float = 0.0
    failure_rate_pct: float = 0.0
    avg_response_time_ms: Optional[float] = None
    requests_per_sec: float = 0.0

    # Validation
    sync_errors: int = 0
    validation_passed: bool = False
    p99_threshold_ms: Optional[float] = None

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary, filtering None values optionally."""
        return asdict(self)

    def to_csv_row(self) -> Dict[str, Any]:
        """Convert to CSV-friendly dictionary."""
        return {k: (v if v is not None else "") for k, v in asdict(self).items()}


# =============================================================================
# Parsers
# =============================================================================

def detect_benchmark_type(content: str) -> str:
    """Auto-detect whether this is a mock or real vLLM benchmark."""
    # Real benchmarks have cache hit/miss tracking
    if "Cache HIT" in content or "Cache MISS" in content:
        return "real"
    if "BENCHMARK_STATS_JSON" in content:
        return "real"
    if "cache_hit_rate" in content.lower():
        return "real"
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
                elif bucket_name == "xlarge":
                    results["ttft_xlarge_hit_p50_ms"] = b.get("cache_hit_ttft_p50_ms")
                    results["ttft_xlarge_hit_p99_ms"] = b.get("cache_hit_ttft_p99_ms")
                    results["ttft_xlarge_miss_p50_ms"] = b.get("cache_miss_ttft_p50_ms")
                    results["ttft_xlarge_miss_p99_ms"] = b.get("cache_miss_ttft_p99_ms")
                    results["ttft_xlarge_improvement_pct"] = b.get("ttft_improvement_pct")

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
            elif bucket == "xlarge":
                results["ttft_xlarge_hit_p50_ms"] = hit_p50
                results["ttft_xlarge_miss_p50_ms"] = miss_p50
                results["ttft_xlarge_improvement_pct"] = improvement

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

    # Real benchmark specific parsing
    if benchmark_type == "real":
        # Cache TTFT
        cache_ttft = parse_cache_ttft(content)
        results.ttft_cache_hit_p50_ms = cache_ttft["ttft_cache_hit_p50_ms"]
        results.ttft_cache_hit_p99_ms = cache_ttft["ttft_cache_hit_p99_ms"]
        results.ttft_cache_miss_p50_ms = cache_ttft["ttft_cache_miss_p50_ms"]
        results.ttft_cache_miss_p99_ms = cache_ttft["ttft_cache_miss_p99_ms"]

        # JSON stats
        json_stats = parse_json_stats(content)
        results.cache_hits = json_stats.get("cache_hits")
        results.cache_misses = json_stats.get("cache_misses")
        results.cache_hit_rate_pct = json_stats.get("cache_hit_rate_pct")
        results.ttft_improvement_pct = json_stats.get("ttft_improvement_pct")
        results.total_prompt_tokens = json_stats.get("total_prompt_tokens")
        results.total_completion_tokens = json_stats.get("total_completion_tokens")
        results.tokens_per_second = json_stats.get("tokens_per_second")
        results.unique_prefixes = json_stats.get("unique_prefixes")

        # Ranvier overhead metrics (P50/P99 percentiles)
        results.routing_latency_p50_ms = json_stats.get("routing_latency_p50_ms")
        results.routing_latency_p99_ms = json_stats.get("routing_latency_p99_ms")
        results.tokenization_latency_p50_ms = json_stats.get("tokenization_latency_p50_ms")
        results.tokenization_latency_p99_ms = json_stats.get("tokenization_latency_p99_ms")
        results.art_lookup_latency_p50_ms = json_stats.get("art_lookup_latency_p50_ms")
        results.art_lookup_latency_p99_ms = json_stats.get("art_lookup_latency_p99_ms")
        results.connect_latency_p50_ms = json_stats.get("connect_latency_p50_ms")
        results.connect_latency_p99_ms = json_stats.get("connect_latency_p99_ms")

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

        # Recalculate failure rate if we have valid counts but rate is 0
        if results.total_requests > 0 and results.failure_rate_pct == 0.0 and results.failed_requests > 0:
            results.failure_rate_pct = (results.failed_requests / results.total_requests) * 100

        # Per-bucket stats from JSON (priority source)
        if json_stats.get("ttft_large_hit_p50_ms"):
            results.ttft_large_hit_p50_ms = json_stats["ttft_large_hit_p50_ms"]
            results.ttft_large_hit_p99_ms = json_stats.get("ttft_large_hit_p99_ms")
            results.ttft_large_miss_p50_ms = json_stats.get("ttft_large_miss_p50_ms")
            results.ttft_large_miss_p99_ms = json_stats.get("ttft_large_miss_p99_ms")
            results.ttft_large_improvement_pct = json_stats.get("ttft_large_improvement_pct")
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

        # Benchmark mode
        mode_match = re.search(r"Benchmark Mode: (\w+)", content)
        if mode_match:
            results.benchmark_mode = mode_match.group(1)

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

    # Map CSV fields to dataclass
    for key in asdict(results).keys():
        if key in data and data[key]:
            val = data[key]
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


def compare_results(baseline: BenchmarkResults, new: BenchmarkResults) -> str:
    """Compare two benchmark results and return formatted comparison."""
    lines = []
    lines.append("=" * 80)
    lines.append("BENCHMARK COMPARISON: Round-Robin vs Prefix-Aware")
    lines.append("=" * 80)
    lines.append(f"Baseline (Round-Robin): {baseline.source_file}")
    lines.append(f"New (Prefix-Aware):     {new.source_file}")
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
    lines.append("")

    # (key, display_name, lower_is_better, is_improvement_pct)
    bucket_metrics = [
        ("ttft_large_hit_p50_ms", "Large Hit P50 (ms)", True, False),
        ("ttft_large_hit_p99_ms", "Large Hit P99 (ms)", True, False),
        ("ttft_large_miss_p50_ms", "Large Miss P50 (ms)", True, False),
        ("ttft_large_miss_p99_ms", "Large Miss P99 (ms)", True, False),
        ("ttft_large_improvement_pct", "Large Improvement (%)", False, True),
        ("ttft_xlarge_hit_p50_ms", "XLarge Hit P50 (ms)", True, False),
        ("ttft_xlarge_hit_p99_ms", "XLarge Hit P99 (ms)", True, False),
        ("ttft_xlarge_miss_p50_ms", "XLarge Miss P50 (ms)", True, False),
        ("ttft_xlarge_miss_p99_ms", "XLarge Miss P99 (ms)", True, False),
        ("ttft_xlarge_improvement_pct", "XLarge Improvement (%)", False, True),
    ]

    lines.append(f"{'Metric':<25} {'Baseline':>12} {'New':>12} {'Change':>30}")
    has_bucket_data = False
    for key, name, lower_is_better, is_improvement_pct in bucket_metrics:
        baseline_val = getattr(baseline, key, None)
        new_val = getattr(new, key, None)
        if baseline_val is None and new_val is None:
            continue
        has_bucket_data = True
        baseline_str = f"{baseline_val:.1f}" if baseline_val is not None else "N/A"
        new_str = f"{new_val:.1f}" if new_val is not None else "N/A"
        change_str = format_change(baseline_val, new_val, lower_is_better, is_improvement_pct)
        lines.append(f"{name:<25} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    if not has_bucket_data:
        lines.append("  (No per-bucket data available)")

    # OVERALL TTFT
    lines.append("")
    lines.append("-" * 80)
    lines.append("OVERALL TTFT (All Request Sizes)")
    lines.append("-" * 80)
    lines.append("  (Aggregate may be misleading - see per-bucket for real impact)")
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

    lines.append("")
    lines.append("-" * 80)

    # Validation status
    lines.append(f"Validation: {'PASSED' if baseline.validation_passed else 'FAILED'} -> {'PASSED' if new.validation_passed else 'FAILED'}")

    # Summary
    lines.append("")
    lines.append("SUMMARY:")
    if new.cache_hit_rate_pct and baseline.cache_hit_rate_pct:
        improvement = new.cache_hit_rate_pct - baseline.cache_hit_rate_pct
        lines.append(f"  Cache Hit Rate: {baseline.cache_hit_rate_pct:.1f}% -> {new.cache_hit_rate_pct:.1f}% (+{improvement:.1f}%)")
    if new.ttft_large_improvement_pct:
        lines.append(f"  Large Prefix TTFT Improvement: {new.ttft_large_improvement_pct:.1f}%")
    if new.ttft_xlarge_improvement_pct:
        lines.append(f"  XLarge Prefix TTFT Improvement: {new.ttft_xlarge_improvement_pct:.1f}%")

    lines.append("=" * 80)

    return "\n".join(lines)


# =============================================================================
# CLI Commands
# =============================================================================

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
    elif args.command == "export":
        return cmd_export(args)

    return 0


if __name__ == "__main__":
    sys.exit(main())
