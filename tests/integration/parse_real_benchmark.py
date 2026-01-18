#!/usr/bin/env python3
"""
DEPRECATED: Use results_parser.py instead.

This script's functionality has been consolidated into results_parser.py.

Instead of:
    python3 parse_real_benchmark.py <input_log> <output_csv>

Use:
    python3 results_parser.py parse <input_log> -o <output_csv> --type real

Or with summary:
    python3 results_parser.py parse <input_log> -o <output_csv> --summary

The new parser auto-detects benchmark type (mock vs real).

This script will be removed in a future release.
================================================================================

Parse Real vLLM Benchmark Output (LEGACY)

Extracts metrics from locustfile_real.py output including:
- Cache hit rate
- TTFT by cache status (hit vs miss)
- Token throughput
- Standard Locust metrics

Usage:
    python3 parse_real_benchmark.py <input_log> <output_csv>

Example:
    python3 parse_real_benchmark.py benchmark-reports/prefix_20251227_output.log benchmark-reports/prefix_20251227_stats.csv
"""

import csv
import json
import re
import sys
import warnings
from datetime import datetime

# Deprecation warning
warnings.warn(
    "\n[DEPRECATED] parse_real_benchmark.py is deprecated.\n"
    "Use: python3 results_parser.py parse <input_log> -o <output_csv>\n",
    DeprecationWarning,
    stacklevel=2,
)
print("\033[1;33m[DEPRECATED]\033[0m Use: results_parser.py parse <log> -o <csv>", file=sys.stderr)


def parse_real_benchmark(log_file: str) -> dict:
    """Parse real benchmark output and extract all metrics."""
    with open(log_file, "r") as f:
        content = f.read()

    results = {
        "timestamp": datetime.now().isoformat(),
        "log_file": log_file,
        # Standard TTFT metrics
        "p50_ttft_ms": None,
        "p75_ttft_ms": None,
        "p90_ttft_ms": None,
        "p95_ttft_ms": None,
        "p99_ttft_ms": None,
        # Cache-specific metrics
        "cache_hits": None,
        "cache_misses": None,
        "cache_hit_rate_pct": None,
        "ttft_cache_hit_p50_ms": None,
        "ttft_cache_hit_p99_ms": None,
        "ttft_cache_miss_p50_ms": None,
        "ttft_cache_miss_p99_ms": None,
        "ttft_improvement_pct": None,
        # Token throughput
        "total_prompt_tokens": None,
        "total_completion_tokens": None,
        "tokens_per_second": None,
        # Standard stats
        "total_requests": 0,
        "failed_requests": 0,
        "failure_rate_pct": 0.0,
        "avg_response_time_ms": None,
        "requests_per_sec": 0.0,
        "unique_prefixes": None,
        # Validation
        "sync_errors": 0,
        "validation_passed": False,
        "benchmark_mode": None,
    }

    # Parse TTFT percentile table
    ttft_pattern = r"GET\s+TTFT \(Time To First Token\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    ttft_match = re.search(ttft_pattern, content)
    if ttft_match:
        results["p50_ttft_ms"] = float(ttft_match.group(1))
        results["p75_ttft_ms"] = float(ttft_match.group(3))
        results["p90_ttft_ms"] = float(ttft_match.group(5))
        results["p95_ttft_ms"] = float(ttft_match.group(6))
        results["p99_ttft_ms"] = float(ttft_match.group(8))

    # Parse cache hit TTFT
    cache_hit_pattern = r"GET\s+TTFT \(Cache HIT\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    cache_hit_match = re.search(cache_hit_pattern, content)
    if cache_hit_match:
        results["ttft_cache_hit_p50_ms"] = float(cache_hit_match.group(1))
        results["ttft_cache_hit_p99_ms"] = float(cache_hit_match.group(8))

    # Parse cache miss TTFT
    cache_miss_pattern = r"GET\s+TTFT \(Cache MISS\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    cache_miss_match = re.search(cache_miss_pattern, content)
    if cache_miss_match:
        results["ttft_cache_miss_p50_ms"] = float(cache_miss_match.group(1))
        results["ttft_cache_miss_p99_ms"] = float(cache_miss_match.group(8))

    # Parse JSON stats (output by locustfile_real.py)
    json_pattern = r"BENCHMARK_STATS_JSON:(\{[^}]+\})"
    json_match = re.search(json_pattern, content)
    if json_match:
        try:
            stats = json.loads(json_match.group(1))
            results["cache_hits"] = stats.get("cache_hits")
            results["cache_misses"] = stats.get("cache_misses")
            results["cache_hit_rate_pct"] = stats.get("cache_hit_rate_pct")
            results["ttft_improvement_pct"] = stats.get("ttft_improvement_pct")
            results["total_prompt_tokens"] = stats.get("total_prompt_tokens")
            results["total_completion_tokens"] = stats.get("total_completion_tokens")
            results["tokens_per_second"] = stats.get("tokens_per_second")
            results["unique_prefixes"] = stats.get("unique_prefixes")

            # Override TTFT stats from JSON if available
            if stats.get("ttft_cache_hit_p50_ms"):
                results["ttft_cache_hit_p50_ms"] = stats["ttft_cache_hit_p50_ms"]
            if stats.get("ttft_cache_hit_p99_ms"):
                results["ttft_cache_hit_p99_ms"] = stats["ttft_cache_hit_p99_ms"]
            if stats.get("ttft_cache_miss_p50_ms"):
                results["ttft_cache_miss_p50_ms"] = stats["ttft_cache_miss_p50_ms"]
            if stats.get("ttft_cache_miss_p99_ms"):
                results["ttft_cache_miss_p99_ms"] = stats["ttft_cache_miss_p99_ms"]
        except json.JSONDecodeError:
            pass

    # Parse benchmark mode
    mode_pattern = r"Benchmark Mode: (\w+)"
    mode_match = re.search(mode_pattern, content)
    if mode_match:
        results["benchmark_mode"] = mode_match.group(1)

    # Parse aggregated stats
    agg_pattern = r"Aggregated\s+(\d+)\s+(\d+)\(([0-9.]+)%\)\s+\|\s+(\d+)"
    agg_match = re.search(agg_pattern, content)
    if agg_match:
        results["total_requests"] = int(agg_match.group(1))
        results["failed_requests"] = int(agg_match.group(2))
        results["failure_rate_pct"] = float(agg_match.group(3))
        results["avg_response_time_ms"] = float(agg_match.group(4))

    # Parse requests per second
    for line in content.split("\n"):
        if "Aggregated" in line:
            rps_match = re.search(r"\|\s+([0-9.]+)\s+[0-9.]+\s*$", line)
            if rps_match:
                results["requests_per_sec"] = float(rps_match.group(1))
            break

    # Check for sync errors
    sync_error_pattern = r"(\d+) new sync errors"
    sync_match = re.search(sync_error_pattern, content)
    if sync_match:
        results["sync_errors"] = int(sync_match.group(1))

    # Check validation result
    if "BENCHMARK PASSED" in content:
        results["validation_passed"] = True
    elif "BENCHMARK FAILED" in content:
        results["validation_passed"] = False

    return results


def write_csv(results: dict, output_file: str):
    """Write results to CSV file."""
    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=results.keys())
        writer.writeheader()
        writer.writerow(results)
    print(f"  Stats saved to: {output_file}")


def print_summary(results: dict):
    """Print a human-readable summary."""
    print("\n" + "=" * 60)
    print("REAL BENCHMARK RESULTS SUMMARY")
    print("=" * 60)

    if results.get("benchmark_mode"):
        print(f"Benchmark Mode: {results['benchmark_mode']}")

    print("\nCache Performance:")
    if results.get("cache_hit_rate_pct") is not None:
        print(f"  Cache Hit Rate: {results['cache_hit_rate_pct']:.1f}%")
        print(f"  Cache Hits: {results['cache_hits']}")
        print(f"  Cache Misses: {results['cache_misses']}")

    print("\nTTFT Latency:")
    if results.get("p50_ttft_ms"):
        print(f"  Overall P50: {results['p50_ttft_ms']:.1f}ms")
        print(f"  Overall P99: {results['p99_ttft_ms']:.1f}ms")
    if results.get("ttft_cache_hit_p50_ms"):
        print(f"  Cache Hit P50: {results['ttft_cache_hit_p50_ms']:.1f}ms")
    if results.get("ttft_cache_miss_p50_ms"):
        print(f"  Cache Miss P50: {results['ttft_cache_miss_p50_ms']:.1f}ms")
    if results.get("ttft_improvement_pct"):
        print(f"  Improvement (Hit vs Miss): {results['ttft_improvement_pct']:.1f}%")

    print("\nThroughput:")
    if results.get("tokens_per_second"):
        print(f"  Tokens/Second: {results['tokens_per_second']:.1f}")
    print(f"  Requests/Second: {results['requests_per_sec']:.2f}")
    print(f"  Total Requests: {results['total_requests']}")
    print(f"  Failed Requests: {results['failed_requests']}")

    print("\nValidation:")
    print(f"  Sync Errors: {results['sync_errors']}")
    print(f"  Passed: {'Yes' if results['validation_passed'] else 'No'}")

    print("=" * 60)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_log> <output_csv>")
        sys.exit(1)

    input_log = sys.argv[1]
    output_csv = sys.argv[2]

    try:
        results = parse_real_benchmark(input_log)
        write_csv(results, output_csv)
        print_summary(results)

    except FileNotFoundError:
        print(f"Error: Could not find {input_log}")
        sys.exit(1)
    except Exception as e:
        print(f"Error parsing output: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
