#!/usr/bin/env python3
"""
Benchmark Comparison Script for Ranvier Core

This script runs the same workload twice:
1. With prefix-aware routing enabled (Ranvier's default)
2. With random routing (baseline, no prefix optimization)

It then compares the results to demonstrate the value of prefix-aware routing.

## Multi-GPU Testing

More backends = greater relative benefit from prefix-affinity routing:
- 2 backends: Random routing gets ~50% cache hit rate
- 4 backends: Random routing gets ~25% cache hit rate
- 8 backends: Random routing gets ~12.5% cache hit rate

Use --num-backends to configure:

    # Test with 4 GPUs
    python3 tests/integration/run_benchmark_comparison.py --num-backends 4 \
        --stress

    # Configure backend IPs via environment variables
    BACKEND1_IP=10.0.0.1 BACKEND2_IP=10.0.0.2 \
    BACKEND3_IP=10.0.0.3 BACKEND4_IP=10.0.0.4 \
    python3 tests/integration/run_benchmark_comparison.py --num-backends 4

## Stress Testing Mode

For large-prefix stress testing that demonstrates measurable KV cache improvements:

    # Run stress test with large prefixes (2000-8000 tokens)
    python3 tests/integration/run_benchmark_comparison.py --stress

    # Customize large prefix settings
    python3 tests/integration/run_benchmark_comparison.py --stress \
        --large-prefix-min 2000 --large-prefix-max 8000 --num-prefixes 5

Expected Results with Large Prefixes:
    - Cache hits: 50-100ms TTFT (cached prefill)
    - Cache misses: 300-800ms TTFT (full prefill)
    - 3-10x TTFT improvement visible

Usage:
    # With external vLLM endpoints:
    VLLM_ENDPOINT_1=http://server1:8000 VLLM_ENDPOINT_2=http://server2:8000 \
    python3 tests/integration/run_benchmark_comparison.py

    # With local vLLM (requires GPU):
    python3 tests/integration/run_benchmark_comparison.py --local-vllm

    # Stress test mode (large prefixes):
    python3 tests/integration/run_benchmark_comparison.py --stress

    # Multi-GPU stress test:
    python3 tests/integration/run_benchmark_comparison.py --stress --num-backends 4

    # Custom duration:
    python3 tests/integration/run_benchmark_comparison.py --duration 10m

Output:
    - benchmark-reports/comparison_<timestamp>/
        - prefix_aware_output.log
        - prefix_aware_stats.csv
        - random_output.log
        - random_stats.csv
        - comparison_summary.txt
        - stress_report.txt (if --stress mode)
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
    # Check for explicit endpoint variables
    endpoint1 = os.environ.get("VLLM_ENDPOINT_1")
    endpoint2 = os.environ.get("VLLM_ENDPOINT_2")

    if endpoint1 and endpoint2:
        return True, f"Using external endpoints: {endpoint1}, {endpoint2}"

    # Check for sequential config pattern (BACKEND_BASE_IP + BACKEND_PORT_START)
    base_ip = os.environ.get("BACKEND_BASE_IP")
    port_start = os.environ.get("BACKEND_PORT_START")
    num_backends = os.environ.get("NUM_BACKENDS")

    if base_ip and port_start:
        n = int(num_backends) if num_backends else 2
        endpoints = [f"{base_ip}:{int(port_start) + i}" for i in range(n)]
        return True, f"Using sequential endpoints: {', '.join(endpoints)}"

    # Check for per-backend config (BACKEND1_IP, BACKEND2_IP, etc.)
    backend1_ip = os.environ.get("BACKEND1_IP")
    backend1_port = os.environ.get("BACKEND1_PORT", "8000")
    if backend1_ip:
        endpoints = [f"{backend1_ip}:{backend1_port}"]
        for i in range(2, 9):
            ip = os.environ.get(f"BACKEND{i}_IP")
            if ip:
                port = os.environ.get(f"BACKEND{i}_PORT", "8000")
                endpoints.append(f"{ip}:{port}")
        return True, f"Using per-backend endpoints: {', '.join(endpoints)}"

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
        "bucket_stats": {},
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
            # Parse bucket stats for stress testing
            if "bucket_stats" in stats:
                results["bucket_stats"] = stats["bucket_stats"]
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


def format_comparison_table(prefix_results: Dict, random_results: Dict) -> str:
    """Generate a formatted comparison table."""
    lines = []
    lines.append("=" * 80)
    lines.append("BENCHMARK COMPARISON: Prefix-Aware Routing vs Random")
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

    lines.append(f"{'Metric':<30} {'Random':>15} {'Prefix-Aware':>15} {'Improvement':>20}")
    lines.append("-" * 80)

    for name, key, lower_is_better in metrics:
        rr_val = random_results.get(key)
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
    p99_rr = random_results.get("p99_ttft_ms")

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


def format_stress_test_report(
    prefix_results: Dict,
    random_results: Dict,
    config: Dict
) -> str:
    """Generate a detailed stress test report with per-bucket analysis.

    Args:
        prefix_results: Results from prefix-affinity routing run
        random_results: Results from random routing run
        config: Configuration used for the stress test
    """
    lines = []
    lines.append("=" * 100)
    lines.append("LARGE-PREFIX STRESS TEST REPORT: KV Cache Validation")
    lines.append("=" * 100)
    lines.append("")

    # Configuration
    num_backends = config.get('num_backends', 2)
    expected_random_hit = 100.0 / num_backends
    lines.append("Configuration:")
    lines.append(f"  Number of Backends: {num_backends}")
    lines.append(f"  Number of Ranvier Nodes: {config.get('num_ranvier_nodes', 3)}")
    lines.append(f"  Prompt Distribution: {config.get('prompt_distribution', 'large-prefix')}")
    lines.append(f"  Large Prefix Min Tokens: {config.get('large_prefix_min', 2000)}")
    lines.append(f"  Large Prefix Max Tokens: {config.get('large_prefix_max', 8000)}")
    lines.append(f"  Number of Prefixes: {config.get('num_prefixes', 5)}")
    lines.append(f"  Expected Random Cache Hit Rate: {expected_random_hit:.1f}% (1/{num_backends})")
    lines.append("")

    # Overall comparison
    lines.append("-" * 100)
    lines.append("OVERALL RESULTS")
    lines.append("-" * 100)

    pa_hit_rate = prefix_results.get("cache_hit_rate_pct", 0)
    rr_hit_rate = random_results.get("cache_hit_rate_pct", 0)
    lines.append(f"  Cache Hit Rate: {rr_hit_rate:.1f}% (random) -> {pa_hit_rate:.1f}% (prefix-affinity)")

    pa_improvement = prefix_results.get("ttft_improvement_pct")
    rr_improvement = random_results.get("ttft_improvement_pct")
    if pa_improvement:
        lines.append(f"  TTFT Improvement (Cache Hit vs Miss): {pa_improvement:.1f}%")
    lines.append("")

    # Per-bucket analysis
    lines.append("-" * 100)
    lines.append("PER-BUCKET TTFT ANALYSIS")
    lines.append("-" * 100)
    lines.append("")

    pa_buckets = prefix_results.get("bucket_stats", {})
    rr_buckets = random_results.get("bucket_stats", {})

    bucket_info = [
        ("tiny", "0-100 tokens", "Negligible prefill time - no visible KV cache benefit"),
        ("small", "100-500 tokens", "Small prefill time - minimal KV cache benefit"),
        ("medium", "500-2000 tokens", "Moderate prefill time - some KV cache benefit"),
        ("large", "2000-4000 tokens", "Significant prefill time - visible KV cache benefit"),
        ("xlarge", "4000-8000 tokens", "Large prefill time - maximum KV cache benefit"),
    ]

    for bucket_name, token_range, description in bucket_info:
        pa_bucket = pa_buckets.get(bucket_name, {})
        rr_bucket = rr_buckets.get(bucket_name, {})

        if not pa_bucket and not rr_bucket:
            continue

        lines.append(f"  {bucket_name.upper()} ({token_range})")
        lines.append(f"    {description}")
        lines.append("")

        pa_requests = pa_bucket.get("requests", 0)
        rr_requests = rr_bucket.get("requests", 0)
        lines.append(f"    Requests: {rr_requests} (random), {pa_requests} (prefix-affinity)")

        # TTFT comparison
        pa_hit_p50 = pa_bucket.get("cache_hit_ttft_p50_ms")
        pa_miss_p50 = pa_bucket.get("cache_miss_ttft_p50_ms")
        rr_hit_p50 = rr_bucket.get("cache_hit_ttft_p50_ms")
        rr_miss_p50 = rr_bucket.get("cache_miss_ttft_p50_ms")

        if pa_hit_p50:
            lines.append(f"    Prefix-Affinity Cache Hit P50: {pa_hit_p50:.1f}ms")
        if pa_miss_p50:
            lines.append(f"    Prefix-Affinity Cache Miss P50: {pa_miss_p50:.1f}ms")
        if rr_hit_p50:
            lines.append(f"    Random Cache Hit P50: {rr_hit_p50:.1f}ms")
        if rr_miss_p50:
            lines.append(f"    Random Cache Miss P50: {rr_miss_p50:.1f}ms")

        # Calculate improvements
        pa_improv = pa_bucket.get("ttft_improvement_pct")
        if pa_improv:
            lines.append(f"    TTFT Improvement (Prefix-Affinity): {pa_improv:.1f}%")

            # Determine if improvement is significant
            if pa_improv >= 50:
                lines.append(f"    *** SIGNIFICANT IMPROVEMENT: {pa_improv:.1f}% faster with cache hit ***")
            elif pa_improv >= 20:
                lines.append(f"    * Notable improvement: {pa_improv:.1f}% faster with cache hit")

        lines.append("")

    # Summary and recommendations
    lines.append("-" * 100)
    lines.append("SUMMARY AND ANALYSIS")
    lines.append("-" * 100)
    lines.append("")

    # Calculate overall improvement for large prefixes
    large_improvement = None
    xlarge_improvement = None

    if pa_buckets.get("large"):
        large_improvement = pa_buckets["large"].get("ttft_improvement_pct")
    if pa_buckets.get("xlarge"):
        xlarge_improvement = pa_buckets["xlarge"].get("ttft_improvement_pct")

    if large_improvement or xlarge_improvement:
        avg_large_improvement = 0
        count = 0
        if large_improvement:
            avg_large_improvement += large_improvement
            count += 1
        if xlarge_improvement:
            avg_large_improvement += xlarge_improvement
            count += 1
        if count > 0:
            avg_large_improvement /= count

        lines.append(f"  Large Prefix TTFT Improvement: {avg_large_improvement:.1f}%")

        if avg_large_improvement >= 50:
            lines.append("")
            lines.append("  CONCLUSION: KV cache prefix-affinity routing provides SIGNIFICANT benefit")
            lines.append("  for large-prefix workloads (2000+ tokens). Cache hits are 2-10x faster")
            lines.append("  than cache misses for GPU prefill operations.")
        elif avg_large_improvement >= 20:
            lines.append("")
            lines.append("  CONCLUSION: KV cache prefix-affinity routing provides MODERATE benefit")
            lines.append("  for large-prefix workloads.")
        else:
            lines.append("")
            lines.append("  CONCLUSION: KV cache improvement is minimal for this workload.")
            lines.append("  Consider checking vLLM --enable-prefix-caching is enabled.")
    else:
        lines.append("  No large prefix data available for analysis.")
        lines.append("  Ensure PROMPT_DISTRIBUTION=large-prefix or stress mode is enabled.")

    # Expected vs actual comparison
    lines.append("")
    lines.append("Expected Results with Large Prefixes (2000-8000 tokens):")
    lines.append("  - Cache hits: 50-100ms TTFT (cached prefill)")
    lines.append("  - Cache misses: 300-800ms TTFT (full prefill)")
    lines.append("  - Expected improvement: 50-90% (3-10x faster)")
    lines.append("")

    if large_improvement and large_improvement >= 50:
        lines.append("  RESULT: PASS - Achieved target improvement for large prefixes")
    elif large_improvement and large_improvement >= 20:
        lines.append("  RESULT: PARTIAL - Some improvement observed but below target")
    elif large_improvement:
        lines.append("  RESULT: NEEDS INVESTIGATION - Below expected improvement")
        lines.append("  Recommendations:")
        lines.append("    1. Verify vLLM --enable-prefix-caching is enabled")
        lines.append("    2. Check GPU memory utilization (OOM may evict cache)")
        lines.append("    3. Ensure prefix tokens match block alignment (16 tokens)")
    else:
        lines.append("  RESULT: NO DATA - Run with --stress flag for large prefix testing")

    lines.append("")
    lines.append("=" * 100)
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
        description="Run benchmark comparison: Prefix-Aware vs Random routing"
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
        "--skip-random",
        action="store_true",
        help="Skip random baseline (only run prefix-aware)",
    )
    parser.add_argument(
        "--skip-prefix",
        action="store_true",
        help="Skip prefix-aware (only run random baseline)",
    )
    parser.add_argument(
        "--stress",
        action="store_true",
        help="Enable stress testing mode with large prefixes (2000-8000 tokens)",
    )
    parser.add_argument(
        "--prompt-distribution",
        default=None,
        choices=["mixed", "short", "medium", "long", "large-prefix", "stress"],
        help="Prompt distribution mode (default: mixed, or large-prefix if --stress)",
    )
    parser.add_argument(
        "--large-prefix-min",
        type=int,
        default=2000,
        help="Minimum tokens for large prefixes (default: 2000)",
    )
    parser.add_argument(
        "--large-prefix-max",
        type=int,
        default=8000,
        help="Maximum tokens for large prefixes (default: 8000)",
    )
    parser.add_argument(
        "--num-prefixes",
        type=int,
        default=5,
        help="Number of large prefixes to generate (default: 5)",
    )
    parser.add_argument(
        "--num-backends",
        type=int,
        default=2,
        help="Number of vLLM backends (default: 2). More backends = greater cache benefit.",
    )
    parser.add_argument(
        "--num-ranvier-nodes",
        type=int,
        default=3,
        help="Number of Ranvier router nodes (default: 3)",
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
            print("Tip: Configure endpoints using one of these methods:")
            print("  - VLLM_ENDPOINT_1=http://host:8000 VLLM_ENDPOINT_2=http://host:8001")
            print("  - BACKEND_BASE_IP=172.17.0.1 BACKEND_PORT_START=8000 NUM_BACKENDS=8")
            print("  - BACKEND1_IP=host1 BACKEND2_IP=host2 ...")
            print("  - Use --local-vllm to start vLLM containers automatically")
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
        dir_suffix = "stress" if args.stress else "comparison"
        output_dir = Path("benchmark-reports") / f"{dir_suffix}_{timestamp}"

    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {output_dir}")

    # Configure prompt distribution
    prompt_distribution = args.prompt_distribution
    if prompt_distribution is None:
        prompt_distribution = "large-prefix" if args.stress else "mixed"

    # Configure stress testing environment variables
    extra_env = {
        "PROMPT_DISTRIBUTION": prompt_distribution,
        "LARGE_PREFIX_MIN_TOKENS": str(args.large_prefix_min),
        "LARGE_PREFIX_MAX_TOKENS": str(args.large_prefix_max),
        "NUM_LARGE_PREFIXES": str(args.num_prefixes),
        "NUM_BACKENDS": str(args.num_backends),
        "NUM_RANVIER_NODES": str(args.num_ranvier_nodes),
    }

    # Store config for reporting
    stress_config = {
        "stress_mode": args.stress,
        "prompt_distribution": prompt_distribution,
        "large_prefix_min": args.large_prefix_min,
        "large_prefix_max": args.large_prefix_max,
        "num_prefixes": args.num_prefixes,
        "num_backends": args.num_backends,
        "num_ranvier_nodes": args.num_ranvier_nodes,
    }

    if args.stress:
        print(f"\nStress Testing Mode:")
        print(f"  Prompt Distribution: {prompt_distribution}")
        print(f"  Large Prefix Tokens: {args.large_prefix_min}-{args.large_prefix_max}")
        print(f"  Number of Prefixes: {args.num_prefixes}")
        print(f"  Number of Backends: {args.num_backends}")
        print(f"  Number of Ranvier Nodes: {args.num_ranvier_nodes}")
        # Print expected random cache hit rate for comparison
        expected_random_hit = 100.0 / args.num_backends
        print(f"  Expected Random Cache Hit Rate: {expected_random_hit:.1f}%")
        print("")

    random_results = {}
    prefix_results = {}

    # Run random baseline
    if not args.skip_random:
        success, random_results = run_benchmark(
            docker_compose=docker_compose,
            output_dir=output_dir,
            label="Random",
            routing_mode="random",
            duration=args.duration,
            users=args.users,
            spawn_rate=args.spawn_rate,
            local_vllm=args.local_vllm,
            extra_env=extra_env,
        )
        if not success:
            print("Warning: Random benchmark had errors")

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
    if random_results and prefix_results:
        comparison = format_comparison_table(prefix_results, random_results)
        print("\n" + comparison)

        # Save comparison summary
        summary_file = output_dir / "comparison_summary.txt"
        with open(summary_file, "w") as f:
            f.write(comparison)
        print(f"\nComparison saved to: {summary_file}")

        # Generate stress test report if in stress mode
        if args.stress or prompt_distribution in ["large-prefix", "stress"]:
            stress_report = format_stress_test_report(
                prefix_results, random_results, stress_config
            )
            print("\n" + stress_report)

            stress_report_file = output_dir / "stress_report.txt"
            with open(stress_report_file, "w") as f:
                f.write(stress_report)
            print(f"\nStress report saved to: {stress_report_file}")

    elif prefix_results:
        print("\nPrefix-aware results:")
        for k, v in prefix_results.items():
            if v is not None and k != "bucket_stats":
                print(f"  {k}: {v}")

        # Still generate stress report for prefix-only runs
        if args.stress or prompt_distribution in ["large-prefix", "stress"]:
            stress_report = format_stress_test_report(
                prefix_results, {}, stress_config
            )
            print("\n" + stress_report)

            stress_report_file = output_dir / "stress_report.txt"
            with open(stress_report_file, "w") as f:
                f.write(stress_report)
            print(f"\nStress report saved to: {stress_report_file}")

    elif random_results:
        print("\nRandom results:")
        for k, v in random_results.items():
            if v is not None and k != "bucket_stats":
                print(f"  {k}: {v}")

    # Save config for reproducibility
    config_file = output_dir / "config.json"
    with open(config_file, "w") as f:
        json.dump({
            "duration": args.duration,
            "users": args.users,
            "spawn_rate": args.spawn_rate,
            "prompt_distribution": prompt_distribution,
            "stress_mode": args.stress,
            "large_prefix_min": args.large_prefix_min,
            "large_prefix_max": args.large_prefix_max,
            "num_prefixes": args.num_prefixes,
        }, f, indent=2)

    print(f"\nAll results saved to: {output_dir}")


if __name__ == "__main__":
    main()
