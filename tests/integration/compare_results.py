#!/usr/bin/env python3
"""
Compare two benchmark result CSV files and show improvement summary.

Usage:
    python3 compare_results.py <baseline_csv> <new_csv>

Example:
    python3 compare_results.py benchmark-reports/20241225_stats.csv benchmark-reports/20241226_stats.csv

Output:
    A formatted table showing the difference between baseline and new results,
    with percentage improvements (negative = improvement, positive = regression).
"""

import csv
import sys
from typing import Optional


def load_csv(filepath: str) -> dict:
    """Load a benchmark stats CSV file."""
    with open(filepath, "r") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        if not rows:
            raise ValueError(f"No data in {filepath}")
        return rows[0]


def format_change(baseline: Optional[float], new: Optional[float], lower_is_better: bool = True) -> str:
    """Format the change between two values with percentage and direction indicator."""
    if baseline is None or new is None:
        return "N/A"

    if baseline == 0:
        if new == 0:
            return "0 (--)"
        return f"{new:.2f} (NEW)"

    diff = new - baseline
    pct = (diff / baseline) * 100

    # For metrics where lower is better (latency, errors), negative change is good
    # For metrics where higher is better (throughput), positive change is good
    if lower_is_better:
        if pct < -1:
            indicator = "✓ BETTER"
        elif pct > 1:
            indicator = "✗ WORSE"
        else:
            indicator = "~ SAME"
    else:
        if pct > 1:
            indicator = "✓ BETTER"
        elif pct < -1:
            indicator = "✗ WORSE"
        else:
            indicator = "~ SAME"

    sign = "+" if diff > 0 else ""
    return f"{sign}{diff:.2f} ({sign}{pct:.1f}%) {indicator}"


def compare_results(baseline_path: str, new_path: str):
    """Compare two benchmark results and print a summary table."""
    baseline = load_csv(baseline_path)
    new = load_csv(new_path)

    print("=" * 80)
    print("BENCHMARK COMPARISON")
    print("=" * 80)
    print(f"Baseline: {baseline_path}")
    print(f"New:      {new_path}")
    print()

    # Define metrics to compare with their display names and whether lower is better
    metrics = [
        ("p50_ttft_ms", "P50 TTFT (ms)", True),
        ("p75_ttft_ms", "P75 TTFT (ms)", True),
        ("p90_ttft_ms", "P90 TTFT (ms)", True),
        ("p95_ttft_ms", "P95 TTFT (ms)", True),
        ("p99_ttft_ms", "P99 TTFT (ms)", True),
        ("avg_response_time_ms", "Avg Response (ms)", True),
        ("total_requests", "Total Requests", False),
        ("failed_requests", "Failed Requests", True),
        ("failure_rate_pct", "Failure Rate (%)", True),
        ("requests_per_sec", "Requests/sec", False),
        ("sync_errors", "Sync Errors", True),
    ]

    # Print comparison table
    print(f"{'Metric':<25} {'Baseline':>12} {'New':>12} {'Change':>30}")
    print("-" * 80)

    for key, name, lower_is_better in metrics:
        baseline_val = baseline.get(key)
        new_val = new.get(key)

        # Convert to float if possible
        try:
            baseline_float = float(baseline_val) if baseline_val else None
            new_float = float(new_val) if new_val else None
        except (ValueError, TypeError):
            baseline_float = None
            new_float = None

        baseline_str = f"{baseline_float:.2f}" if baseline_float is not None else "N/A"
        new_str = f"{new_float:.2f}" if new_float is not None else "N/A"
        change_str = format_change(baseline_float, new_float, lower_is_better)

        print(f"{name:<25} {baseline_str:>12} {new_str:>12} {change_str:>30}")

    print("-" * 80)

    # Validation status
    baseline_passed = baseline.get("validation_passed", "").lower() == "true"
    new_passed = new.get("validation_passed", "").lower() == "true"

    print(f"\nValidation: {'PASSED' if baseline_passed else 'FAILED'} -> {'PASSED' if new_passed else 'FAILED'}")

    # Calculate overall summary
    try:
        baseline_p99 = float(baseline.get("p99_ttft_ms", 0))
        new_p99 = float(new.get("p99_ttft_ms", 0))
        if baseline_p99 > 0:
            p99_improvement = ((baseline_p99 - new_p99) / baseline_p99) * 100
            if p99_improvement > 0:
                print(f"\n🎉 P99 TTFT improved by {p99_improvement:.1f}%")
            elif p99_improvement < 0:
                print(f"\n⚠️  P99 TTFT regressed by {abs(p99_improvement):.1f}%")
            else:
                print(f"\n→ P99 TTFT unchanged")
    except (ValueError, TypeError, ZeroDivisionError):
        pass

    print("=" * 80)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <baseline_csv> <new_csv>")
        print()
        print("Compare two benchmark result CSV files and show improvement summary.")
        print()
        print("Example:")
        print(f"  {sys.argv[0]} benchmark-reports/20241225_stats.csv benchmark-reports/20241226_stats.csv")
        sys.exit(1)

    baseline_path = sys.argv[1]
    new_path = sys.argv[2]

    try:
        compare_results(baseline_path, new_path)
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Error comparing results: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
