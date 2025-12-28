#!/usr/bin/env python3
"""
Benchmark Comparison Script for Ranvier Core

This script runs the same workload twice:
1. With prefix-aware routing enabled (Ranvier's default)
2. With round-robin routing (baseline, no prefix optimization)

It then compares the results to demonstrate the value of prefix-aware routing.

Usage:
    # With external vLLM endpoints:
    VLLM_ENDPOINT_1=http://server1:8000 VLLM_ENDPOINT_2=http://server2:8000 \
    python3 tests/integration/run_benchmark_comparison.py

    # With local vLLM (requires GPU):
    python3 tests/integration/run_benchmark_comparison.py --local-vllm

    # Custom duration:
    python3 tests/integration/run_benchmark_comparison.py --duration 10m

Output:
    - benchmark-reports/comparison_<timestamp>/
        - prefix_aware_output.log
        - prefix_aware_stats.csv
        - round_robin_output.log
        - round_robin_stats.csv
        - comparison_summary.txt
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def detect_docker_compose() -> str:
    """Detect available docker compose command."""
    try:
        result = subprocess.run(
            ["docker", "compose", "version"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return "docker compose"
    except FileNotFoundError:
        pass

    try:
        result = subprocess.run(
            ["docker-compose", "version"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return "docker-compose"
    except FileNotFoundError:
        pass

    raise RuntimeError("Neither 'docker compose' nor 'docker-compose' found")


def check_vllm_endpoints() -> Tuple[bool, str]:
    """Check if vLLM endpoints are configured or available."""
    endpoint1 = os.environ.get("VLLM_ENDPOINT_1")
    endpoint2 = os.environ.get("VLLM_ENDPOINT_2")

    if endpoint1 and endpoint2:
        return True, f"Using external endpoints: {endpoint1}, {endpoint2}"

    return False, "No VLLM_ENDPOINT_* environment variables set"


def parse_locust_output(log_file: str) -> Dict:
    """Parse Locust output log and extract metrics."""
    with open(log_file, "r") as f:
        content = f.read()

    results = {
        "p50_ttft_ms": None,
        "p99_ttft_ms": None,
        "cache_hit_rate_pct": None,
        "ttft_cache_hit_p50_ms": None,
        "ttft_cache_miss_p50_ms": None,
        "ttft_improvement_pct": None,
        "tokens_per_second": None,
        "total_requests": 0,
        "failed_requests": 0,
        "requests_per_sec": 0.0,
    }

    # Parse TTFT percentiles
    ttft_pattern = r"GET\s+TTFT \(Time To First Token\)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
    ttft_match = re.search(ttft_pattern, content)
    if ttft_match:
        results["p50_ttft_ms"] = float(ttft_match.group(1))
        results["p99_ttft_ms"] = float(ttft_match.group(8))

    # Parse JSON stats from the log
    json_pattern = r"BENCHMARK_STATS_JSON:(\{.*\})"
    json_match = re.search(json_pattern, content)
    if json_match:
        try:
            stats = json.loads(json_match.group(1))
            results["cache_hit_rate_pct"] = stats.get("cache_hit_rate_pct")
            results["ttft_cache_hit_p50_ms"] = stats.get("ttft_cache_hit_p50_ms")
            results["ttft_cache_miss_p50_ms"] = stats.get("ttft_cache_miss_p50_ms")
            results["ttft_improvement_pct"] = stats.get("ttft_improvement_pct")
            results["tokens_per_second"] = stats.get("tokens_per_second")
        except json.JSONDecodeError:
            pass

    # Parse aggregated stats
    agg_pattern = r"Aggregated\s+(\d+)\s+(\d+)\(([0-9.]+)%\)"
    agg_match = re.search(agg_pattern, content)
    if agg_match:
        results["total_requests"] = int(agg_match.group(1))
        results["failed_requests"] = int(agg_match.group(2))

    # Parse requests per second
    for line in content.split("\n"):
        if "Aggregated" in line:
            rps_match = re.search(r"\|\s+([0-9.]+)\s+[0-9.]+\s*$", line)
            if rps_match:
                results["requests_per_sec"] = float(rps_match.group(1))
            break

    return results


def format_comparison_table(prefix_results: Dict, roundrobin_results: Dict) -> str:
    """Generate a formatted comparison table."""
    lines = []
    lines.append("=" * 80)
    lines.append("BENCHMARK COMPARISON: Prefix-Aware Routing vs Round-Robin")
    lines.append("=" * 80)
    lines.append("")

    # Define metrics to compare
    metrics = [
        ("P50 TTFT (ms)", "p50_ttft_ms", True),
        ("P99 TTFT (ms)", "p99_ttft_ms", True),
        ("Cache Hit Rate (%)", "cache_hit_rate_pct", False),
        ("TTFT Cache Hit P50 (ms)", "ttft_cache_hit_p50_ms", True),
        ("TTFT Cache Miss P50 (ms)", "ttft_cache_miss_p50_ms", True),
        ("Tokens/Second", "tokens_per_second", False),
        ("Total Requests", "total_requests", False),
        ("Failed Requests", "failed_requests", True),
        ("Requests/sec", "requests_per_sec", False),
    ]

    lines.append(f"{'Metric':<30} {'Round-Robin':>15} {'Prefix-Aware':>15} {'Improvement':>20}")
    lines.append("-" * 80)

    for name, key, lower_is_better in metrics:
        rr_val = roundrobin_results.get(key)
        pa_val = prefix_results.get(key)

        rr_str = f"{rr_val:.2f}" if rr_val is not None else "N/A"
        pa_str = f"{pa_val:.2f}" if pa_val is not None else "N/A"

        improvement = ""
        if rr_val is not None and pa_val is not None and rr_val != 0:
            if lower_is_better:
                pct = ((rr_val - pa_val) / rr_val) * 100
                if pct > 0:
                    improvement = f"+{pct:.1f}% faster"
                elif pct < 0:
                    improvement = f"{abs(pct):.1f}% slower"
            else:
                pct = ((pa_val - rr_val) / rr_val) * 100
                if pct > 0:
                    improvement = f"+{pct:.1f}% better"
                elif pct < 0:
                    improvement = f"{abs(pct):.1f}% worse"

        lines.append(f"{name:<30} {rr_str:>15} {pa_str:>15} {improvement:>20}")

    lines.append("-" * 80)
    lines.append("")

    # Summary
    if prefix_results.get("cache_hit_rate_pct") is not None:
        cache_rate = prefix_results["cache_hit_rate_pct"]
        lines.append(f"Cache Hit Rate with Prefix Routing: {cache_rate:.1f}%")

    if prefix_results.get("ttft_improvement_pct") is not None:
        ttft_imp = prefix_results["ttft_improvement_pct"]
        lines.append(f"TTFT Improvement (Cache Hit vs Miss): {ttft_imp:.1f}%")

    # Overall assessment
    lines.append("")
    p99_prefix = prefix_results.get("p99_ttft_ms")
    p99_rr = roundrobin_results.get("p99_ttft_ms")

    if p99_prefix and p99_rr:
        if p99_prefix < p99_rr:
            improvement = ((p99_rr - p99_prefix) / p99_rr) * 100
            lines.append(f"RESULT: Prefix-aware routing improved P99 TTFT by {improvement:.1f}%")
        elif p99_prefix > p99_rr:
            regression = ((p99_prefix - p99_rr) / p99_rr) * 100
            lines.append(f"RESULT: Prefix-aware routing regressed P99 TTFT by {regression:.1f}%")
        else:
            lines.append("RESULT: No significant difference in P99 TTFT")

    lines.append("=" * 80)
    return "\n".join(lines)


def run_benchmark(
    docker_compose: str,
    output_dir: Path,
    label: str,
    routing_mode: str,
    duration: str,
    users: int,
    spawn_rate: int,
    local_vllm: bool,
    extra_env: Dict[str, str],
) -> Tuple[bool, Dict]:
    """Run a single benchmark with the specified configuration."""
    print(f"\n{'='*60}")
    print(f"Running {label} benchmark (routing_mode={routing_mode})")
    print(f"{'='*60}")

    compose_file = "docker-compose.benchmark-real.yml"
    project_name = f"ranvier-benchmark-{label.lower().replace(' ', '-')}"

    # Build environment
    env = os.environ.copy()
    env["RANVIER_ROUTING_MODE"] = routing_mode
    env["BENCHMARK_MODE"] = routing_mode
    env.update(extra_env)

    # Determine profiles
    profiles = ["benchmark"]
    if local_vllm:
        profiles.append("local-vllm")

    profile_args = []
    for p in profiles:
        profile_args.extend(["--profile", p])

    # Start the cluster
    print(f"Starting cluster with {routing_mode} routing...")
    start_cmd = docker_compose.split() + [
        "-f", compose_file,
        "-p", project_name,
    ] + profile_args + ["up", "-d", "--build"]

    result = subprocess.run(start_cmd, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Failed to start cluster: {result.stderr}")
        return False, {}

    # Wait for cluster to be healthy
    print("Waiting for cluster to become healthy...")
    time.sleep(30 if local_vllm else 15)

    # Run Locust
    print(f"Starting Locust load test ({duration} duration, {users} users)...")
    output_log = output_dir / f"{label.lower().replace(' ', '_')}_output.log"

    locust_cmd = docker_compose.split() + [
        "-f", compose_file,
        "-p", project_name,
    ] + profile_args + [
        "run", "--rm",
        "-e", f"BENCHMARK_MODE={routing_mode}",
        "locust",
        "-f", "/mnt/locust/locustfile_real.py",
        "--host=http://172.29.2.1:8080",
        f"--users={users}",
        f"--spawn-rate={spawn_rate}",
        f"--run-time={duration}",
        "--headless",
        "--only-summary",
        "--exit-code-on-error", "1",
    ]

    with open(output_log, "w") as log_file:
        result = subprocess.run(
            locust_cmd,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )

    locust_exit = result.returncode

    # Stop the cluster
    print("Stopping cluster...")
    stop_cmd = docker_compose.split() + [
        "-f", compose_file,
        "-p", project_name,
    ] + profile_args + ["down", "-v", "--remove-orphans"]

    subprocess.run(stop_cmd, env=env, capture_output=True)

    # Parse results
    print(f"Parsing results from {output_log}...")
    results = parse_locust_output(str(output_log))

    # Save stats CSV
    stats_csv = output_dir / f"{label.lower().replace(' ', '_')}_stats.csv"
    with open(stats_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=results.keys())
        writer.writeheader()
        writer.writerow(results)

    print(f"Results saved to {stats_csv}")

    success = locust_exit == 0
    return success, results


def main():
    parser = argparse.ArgumentParser(
        description="Run benchmark comparison: Prefix-Aware vs Round-Robin routing"
    )
    parser.add_argument(
        "--duration", "-d",
        default="5m",
        help="Duration of each benchmark run (default: 5m)",
    )
    parser.add_argument(
        "--users", "-u",
        type=int,
        default=10,
        help="Number of concurrent users (default: 10)",
    )
    parser.add_argument(
        "--spawn-rate", "-s",
        type=int,
        default=2,
        help="User spawn rate per second (default: 2)",
    )
    parser.add_argument(
        "--local-vllm",
        action="store_true",
        help="Use local vLLM containers (requires GPU)",
    )
    parser.add_argument(
        "--output-dir", "-o",
        default=None,
        help="Output directory (default: benchmark-reports/comparison_<timestamp>)",
    )
    parser.add_argument(
        "--skip-roundrobin",
        action="store_true",
        help="Skip round-robin baseline (only run prefix-aware)",
    )
    parser.add_argument(
        "--skip-prefix",
        action="store_true",
        help="Skip prefix-aware (only run round-robin baseline)",
    )

    args = parser.parse_args()

    # Check prerequisites
    try:
        docker_compose = detect_docker_compose()
        print(f"Using: {docker_compose}")
    except RuntimeError as e:
        print(f"Error: {e}")
        sys.exit(1)

    # Check vLLM endpoints
    if not args.local_vllm:
        endpoints_ok, msg = check_vllm_endpoints()
        if not endpoints_ok:
            print(f"Warning: {msg}")
            print("Tip: Set VLLM_ENDPOINT_1 and VLLM_ENDPOINT_2 or use --local-vllm")
            response = input("Continue anyway? (y/N): ")
            if response.lower() != "y":
                sys.exit(1)
        else:
            print(msg)

    # Setup output directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    if args.output_dir:
        output_dir = Path(args.output_dir)
    else:
        output_dir = Path("benchmark-reports") / f"comparison_{timestamp}"

    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {output_dir}")

    extra_env = {}
    roundrobin_results = {}
    prefix_results = {}

    # Run round-robin baseline
    if not args.skip_roundrobin:
        success, roundrobin_results = run_benchmark(
            docker_compose=docker_compose,
            output_dir=output_dir,
            label="Round-Robin",
            routing_mode="round_robin",
            duration=args.duration,
            users=args.users,
            spawn_rate=args.spawn_rate,
            local_vllm=args.local_vllm,
            extra_env=extra_env,
        )
        if not success:
            print("Warning: Round-robin benchmark had errors")

        # Cool-down period between runs
        if not args.skip_prefix:
            print("\nCooling down for 30 seconds between runs...")
            time.sleep(30)

    # Run prefix-aware
    if not args.skip_prefix:
        success, prefix_results = run_benchmark(
            docker_compose=docker_compose,
            output_dir=output_dir,
            label="Prefix-Aware",
            routing_mode="prefix",
            duration=args.duration,
            users=args.users,
            spawn_rate=args.spawn_rate,
            local_vllm=args.local_vllm,
            extra_env=extra_env,
        )
        if not success:
            print("Warning: Prefix-aware benchmark had errors")

    # Generate comparison if both ran
    if roundrobin_results and prefix_results:
        comparison = format_comparison_table(prefix_results, roundrobin_results)
        print("\n" + comparison)

        # Save comparison summary
        summary_file = output_dir / "comparison_summary.txt"
        with open(summary_file, "w") as f:
            f.write(comparison)
        print(f"\nComparison saved to: {summary_file}")
    elif prefix_results:
        print("\nPrefix-aware results:")
        for k, v in prefix_results.items():
            if v is not None:
                print(f"  {k}: {v}")
    elif roundrobin_results:
        print("\nRound-robin results:")
        for k, v in roundrobin_results.items():
            if v is not None:
                print(f"  {k}: {v}")

    print(f"\nAll results saved to: {output_dir}")


if __name__ == "__main__":
    main()
