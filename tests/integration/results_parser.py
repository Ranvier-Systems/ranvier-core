#!/usr/bin/env python3
"""
Unified Benchmark Results Parser for Ranvier

This script consolidates functionality from:
  - parse_locust_output.py (use: parse --type mock)
  - parse_real_benchmark.py (use: parse --type real)
  - compare_results.py (use: compare)

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
    ./results_parser.py compare round_robin_stats.csv prefix_stats.csv

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

    # Standard Locust metrics
    total_requests: int = 0
    failed_requests: int = 0
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
    json_pattern = r"BENCHMARK_STATS_JSON:(\{[^}]+\})"
    match = re.search(json_pattern, content)
    if match:
        try:
            stats = json.loads(match.group(1))
            results["cache_hits"] = stats.get("cache_hits")
            results["cache_misses"] = stats.get("cache_misses")
            results["cache_hit_rate_pct"] = stats.get("cache_hit_rate_pct")
            results["ttft_improvement_pct"] = stats.get("ttft_improvement_pct")
            results["total_prompt_tokens"] = stats.get("total_prompt_tokens")
            results["total_completion_tokens"] = stats.get("total_completion_tokens")
            results["tokens_per_second"] = stats.get("tokens_per_second")
            results["unique_prefixes"] = stats.get("unique_prefixes")

            # Override TTFT stats from JSON if available (more accurate)
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
    return results


def parse_aggregated_stats(content: str) -> Dict[str, Any]:
    """Parse Locust aggregated statistics."""
    results = {
        "total_requests": 0,
        "failed_requests": 0,
        "failure_rate_pct": 0.0,
        "avg_response_time_ms": None,
        "requests_per_sec": 0.0,
    }

    # Pattern: Aggregated    1263  282(22.33%) |     38       0      72     53 |    3.09        0.69
    agg_pattern = r"Aggregated\s+(\d+)\s+(\d+)\(([0-9.]+)%\)\s+\|\s+(\d+)"
    match = re.search(agg_pattern, content)
    if match:
        results["total_requests"] = int(match.group(1))
        results["failed_requests"] = int(match.group(2))
        results["failure_rate_pct"] = float(match.group(3))
        results["avg_response_time_ms"] = float(match.group(4))

    # Parse requests per second from aggregated line
    for line in content.split("\n"):
        if "Aggregated" in line:
            rps_match = re.search(r"\|\s+([0-9.]+)\s+[0-9.]+\s*$", line)
            if rps_match:
                results["requests_per_sec"] = float(rps_match.group(1))
            break

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

        # Override cache TTFT from JSON if available
        if json_stats.get("ttft_cache_hit_p50_ms"):
            results.ttft_cache_hit_p50_ms = json_stats["ttft_cache_hit_p50_ms"]
        if json_stats.get("ttft_cache_hit_p99_ms"):
            results.ttft_cache_hit_p99_ms = json_stats["ttft_cache_hit_p99_ms"]
        if json_stats.get("ttft_cache_miss_p50_ms"):
            results.ttft_cache_miss_p50_ms = json_stats["ttft_cache_miss_p50_ms"]
        if json_stats.get("ttft_cache_miss_p99_ms"):
            results.ttft_cache_miss_p99_ms = json_stats["ttft_cache_miss_p99_ms"]

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

def format_change(baseline: Optional[float], new: Optional[float], lower_is_better: bool = True) -> str:
    """Format the change between two values with percentage and direction."""
    if baseline is None or new is None:
        return "N/A"

    if baseline == 0:
        if new == 0:
            return "0 (--)"
        return f"{new:.2f} (NEW)"

    diff = new - baseline
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
    lines.append("BENCHMARK COMPARISON")
    lines.append("=" * 80)
    lines.append(f"Baseline: {baseline.source_file}")
    lines.append(f"New:      {new.source_file}")
    lines.append("")

    # Metrics to compare: (key, display_name, lower_is_better)
    metrics = [
        ("p50_ttft_ms", "P50 TTFT (ms)", True),
        ("p75_ttft_ms", "P75 TTFT (ms)", True),
        ("p90_ttft_ms", "P90 TTFT (ms)", True),
        ("p95_ttft_ms", "P95 TTFT (ms)", True),
        ("p99_ttft_ms", "P99 TTFT (ms)", True),
        ("ttft_cache_hit_p50_ms", "Cache Hit P50 (ms)", True),
        ("ttft_cache_hit_p99_ms", "Cache Hit P99 (ms)", True),
        ("ttft_cache_miss_p50_ms", "Cache Miss P50 (ms)", True),
        ("ttft_cache_miss_p99_ms", "Cache Miss P99 (ms)", True),
        ("cache_hit_rate_pct", "Cache Hit Rate (%)", False),
        ("avg_response_time_ms", "Avg Response (ms)", True),
        ("total_requests", "Total Requests", False),
        ("failed_requests", "Failed Requests", True),
        ("failure_rate_pct", "Failure Rate (%)", True),
        ("requests_per_sec", "Requests/sec", False),
        ("tokens_per_second", "Tokens/sec", False),
        ("sync_errors", "Sync Errors", True),
    ]

    lines.append(f"{'Metric':<25} {'Baseline':>12} {'New':>12} {'Change':>30}")
    lines.append("-" * 80)

    for key, name, lower_is_better in metrics:
        baseline_val = getattr(baseline, key, None)
        new_val = getattr(new, key, None)

        # Skip if both are None
        if baseline_val is None and new_val is None:
            continue

        baseline_str = f"{baseline_val:.2f}" if baseline_val is not None else "N/A"
        new_str = f"{new_val:.2f}" if new_val is not None else "N/A"
        change_str = format_change(baseline_val, new_val, lower_is_better)

        lines.append(f"{name:<25} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    lines.append("-" * 80)

    # Validation status
    lines.append(f"\nValidation: {'PASSED' if baseline.validation_passed else 'FAILED'} -> {'PASSED' if new.validation_passed else 'FAILED'}")

    # P99 summary
    if baseline.p99_ttft_ms and new.p99_ttft_ms and baseline.p99_ttft_ms > 0:
        p99_improvement = ((baseline.p99_ttft_ms - new.p99_ttft_ms) / baseline.p99_ttft_ms) * 100
        if p99_improvement > 0:
            lines.append(f"\nP99 TTFT improved by {p99_improvement:.1f}%")
        elif p99_improvement < 0:
            lines.append(f"\nP99 TTFT regressed by {abs(p99_improvement):.1f}%")
        else:
            lines.append(f"\nP99 TTFT unchanged")

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
