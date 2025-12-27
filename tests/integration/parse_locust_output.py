#!/usr/bin/env python3
"""
Parse Locust output log and extract statistics to CSV.

Usage:
    python3 parse_locust_output.py <input_log> <output_csv>

Example:
    python3 parse_locust_output.py benchmark-reports/20241226_123456_output.log benchmark-reports/20241226_123456_stats.csv
"""

import csv
import re
import sys
from datetime import datetime


def parse_locust_output(log_file: str) -> dict:
    """Parse Locust output and extract key metrics."""
    with open(log_file, "r") as f:
        content = f.read()

    results = {
        "timestamp": datetime.now().isoformat(),
        "log_file": log_file,
        "p50_ttft_ms": None,
        "p75_ttft_ms": None,
        "p90_ttft_ms": None,
        "p95_ttft_ms": None,
        "p99_ttft_ms": None,
        "total_requests": 0,
        "failed_requests": 0,
        "failure_rate_pct": 0.0,
        "avg_response_time_ms": None,
        "requests_per_sec": 0.0,
        "sync_errors": 0,
        "validation_passed": False,
    }

    # Parse the percentile table for TTFT
    # Example line: GET      TTFT (Time To First Token)    55     57     58     58     61     64     67     68     72     72     72    422
    ttft_pattern = r"GET\s+TTFT \(Time To First Token\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    ttft_match = re.search(ttft_pattern, content)
    if ttft_match:
        results["p50_ttft_ms"] = float(ttft_match.group(1))
        # Groups: 50%, 66%, 75%, 80%, 90%, 95%, 98%, 99%
        results["p75_ttft_ms"] = float(ttft_match.group(3))
        results["p90_ttft_ms"] = float(ttft_match.group(5))
        results["p95_ttft_ms"] = float(ttft_match.group(6))
        results["p99_ttft_ms"] = float(ttft_match.group(8))

    # Parse the summary table for aggregated stats
    # Example: Aggregated    1263  282(22.33%) |     38       0      72     53 |    3.09        0.69
    agg_pattern = r"Aggregated\s+(\d+)\s+(\d+)\(([0-9.]+)%\)\s+\|\s+(\d+)"
    agg_match = re.search(agg_pattern, content)
    if agg_match:
        results["total_requests"] = int(agg_match.group(1))
        results["failed_requests"] = int(agg_match.group(2))
        results["failure_rate_pct"] = float(agg_match.group(3))
        results["avg_response_time_ms"] = float(agg_match.group(4))

    # Parse requests per second
    rps_pattern = r"\|\s+([0-9.]+)\s+[0-9.]+\s*$"
    # Look for it in the aggregated line
    for line in content.split("\n"):
        if "Aggregated" in line:
            rps_match = re.search(rps_pattern, line)
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

    # Extract P99 threshold used
    threshold_pattern = r"P99 Latency Threshold: ([0-9.]+)ms"
    threshold_match = re.search(threshold_pattern, content)
    if threshold_match:
        results["p99_threshold_ms"] = float(threshold_match.group(1))

    return results


def write_csv(results: dict, output_file: str):
    """Write results to CSV file."""
    with open(output_file, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=results.keys())
        writer.writeheader()
        writer.writerow(results)
    print(f"  Stats saved to: {output_file}")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_log> <output_csv>")
        sys.exit(1)

    input_log = sys.argv[1]
    output_csv = sys.argv[2]

    try:
        results = parse_locust_output(input_log)
        write_csv(results, output_csv)

        # Print summary
        print(f"  P99 TTFT: {results['p99_ttft_ms']}ms")
        print(f"  Total requests: {results['total_requests']}")
        print(f"  Failure rate: {results['failure_rate_pct']}%")

    except FileNotFoundError:
        print(f"Error: Could not find {input_log}")
        sys.exit(1)
    except Exception as e:
        print(f"Error parsing output: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
