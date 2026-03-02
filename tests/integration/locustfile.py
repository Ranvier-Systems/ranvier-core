#!/usr/bin/env python3
"""
Locust Load Testing for Ranvier Core

This load test file performs:
1. Initial administrative registration of backends
2. Simulates concurrent users sending /v1/chat/completions requests
3. Measures TTFT (Time To First Token) - interval between request start and first SSE chunk
4. Validates P99 latency and cluster sync errors

Usage:
    # Headless mode (5 minutes, with reports):
    make benchmark

    # Interactive mode with web UI:
    docker compose -f docker-compose.test.yml --profile benchmark up -d
    # Then open http://localhost:8089
"""

import json
import logging
import os
import random
import re
import time
from typing import Optional

import requests
from locust import HttpUser, task, between, events
from locust.runners import MasterRunner, WorkerRunner

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Environment configuration
RANVIER_NODES = [
    os.environ.get("RANVIER_NODE1", "http://172.28.2.1:8080"),
    os.environ.get("RANVIER_NODE2", "http://172.28.2.2:8080"),
    os.environ.get("RANVIER_NODE3", "http://172.28.2.3:8080"),
]

RANVIER_METRICS = [
    os.environ.get("RANVIER_METRICS1", "http://172.28.2.1:9180"),
    os.environ.get("RANVIER_METRICS2", "http://172.28.2.2:9180"),
    os.environ.get("RANVIER_METRICS3", "http://172.28.2.3:9180"),
]

BACKENDS = [
    {
        "id": 1,
        "ip": os.environ.get("BACKEND1_IP", "172.28.1.10"),
        "port": int(os.environ.get("BACKEND1_PORT", "8000")),
    },
    {
        "id": 2,
        "ip": os.environ.get("BACKEND2_IP", "172.28.1.11"),
        "port": int(os.environ.get("BACKEND2_PORT", "8000")),
    },
]

# Configurable thresholds
P99_LATENCY_THRESHOLD_MS = float(os.environ.get("P99_LATENCY_THRESHOLD_MS", "100"))

# Metrics collection for validation
_initial_sync_errors: dict[str, float] = {}
_backends_registered = False


def get_metric_value(metrics_url: str, metric_name: str) -> Optional[float]:
    """Extract a specific metric value from Prometheus endpoint."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            if metric_name in line:
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    return float(match.group(1))
        return None
    except requests.exceptions.RequestException as e:
        logger.warning(f"Failed to fetch metrics from {metrics_url}: {e}")
        return None


def get_histogram_avg(metrics_url: str, metric_name: str) -> Optional[float]:
    """Get average value from a Prometheus histogram (sum/count)."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        sum_val = None
        count_val = None

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            if f"{metric_name}_sum" in line:
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    sum_val = float(match.group(1))
            elif f"{metric_name}_count" in line:
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    count_val = float(match.group(1))

        if sum_val is not None and count_val is not None and count_val > 0:
            return sum_val / count_val
        return None
    except requests.exceptions.RequestException as e:
        logger.warning(f"Failed to fetch histogram from {metrics_url}: {e}")
        return None


def get_histogram_percentile(metrics_url: str, metric_name: str, percentile: float) -> Optional[float]:
    """Calculate percentile from Prometheus histogram buckets using linear interpolation.

    Reference: https://prometheus.io/docs/practices/histograms/#quantiles
    """
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        buckets = []
        bucket_pattern = re.compile(
            rf'{metric_name}_bucket\{{le="([^"]+)"\}}\s+(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)'
        )

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            match = bucket_pattern.search(line)
            if match:
                le_str = match.group(1)
                count = float(match.group(2))
                if le_str == "+Inf":
                    upper_bound = float("inf")
                else:
                    upper_bound = float(le_str)
                buckets.append((upper_bound, count))

        if not buckets:
            return None

        buckets.sort(key=lambda x: x[0])

        total_count = buckets[-1][1] if buckets else 0
        if total_count == 0:
            return None

        target_count = percentile * total_count
        prev_bound = 0.0
        prev_count = 0.0

        for upper_bound, cumulative_count in buckets:
            if cumulative_count >= target_count:
                if upper_bound == float("inf"):
                    return prev_bound
                bucket_count = cumulative_count - prev_count
                if bucket_count == 0:
                    return prev_bound
                fraction = (target_count - prev_count) / bucket_count
                return prev_bound + (upper_bound - prev_bound) * fraction
            prev_bound = upper_bound
            prev_count = cumulative_count

        for upper_bound, _ in reversed(buckets):
            if upper_bound != float("inf"):
                return upper_bound
        return None

    except requests.exceptions.RequestException as e:
        logger.warning(f"Failed to fetch histogram percentile from {metrics_url}: {e}")
        return None
    except (ValueError, ZeroDivisionError) as e:
        logger.warning(f"Failed to calculate percentile for {metric_name}: {e}")
        return None


def get_ranvier_latency_breakdown() -> dict:
    """Get Ranvier's internal latency breakdown from Prometheus metrics.

    Returns P50 and P99 latencies in milliseconds for routing, tokenization
    (total, primary, boundary detection), ART lookup, and backend connect.
    """
    breakdown = {
        "routing_latency_p50_ms": None,
        "routing_latency_p99_ms": None,
        "tokenization_latency_p50_ms": None,
        "tokenization_latency_p99_ms": None,
        "primary_tokenization_latency_p50_ms": None,
        "primary_tokenization_latency_p99_ms": None,
        "boundary_detection_latency_p50_ms": None,
        "boundary_detection_latency_p99_ms": None,
        "art_lookup_latency_p50_ms": None,
        "art_lookup_latency_p99_ms": None,
        "connect_latency_p50_ms": None,
        "connect_latency_p99_ms": None,
    }

    # Metrics to scrape: (prometheus_name, p50_list, p99_list)
    metrics_spec = [
        ("seastar_ranvier_router_routing_latency_seconds", [], []),
        ("seastar_ranvier_router_tokenization_latency_seconds", [], []),
        ("seastar_ranvier_router_primary_tokenization_latency_seconds", [], []),
        ("seastar_ranvier_router_boundary_detection_latency_seconds", [], []),
        ("seastar_ranvier_backend_connect_duration_seconds", [], []),
    ]

    for metrics_url in RANVIER_METRICS:
        for metric_name, p50_vals, p99_vals in metrics_spec:
            p50 = get_histogram_percentile(metrics_url, metric_name, 0.50)
            p99 = get_histogram_percentile(metrics_url, metric_name, 0.99)
            if p50 is not None:
                p50_vals.append(p50)
                if p99 is not None:
                    p99_vals.append(p99)
            else:
                avg = get_histogram_avg(metrics_url, metric_name)
                if avg is not None:
                    p50_vals.append(avg)
                    p99_vals.append(avg)

    # Map collected values to breakdown keys (convert seconds to ms)
    key_map = [
        ("routing_latency", metrics_spec[0]),
        ("tokenization_latency", metrics_spec[1]),
        ("primary_tokenization_latency", metrics_spec[2]),
        ("boundary_detection_latency", metrics_spec[3]),
        ("connect_latency", metrics_spec[4]),
    ]
    for key_prefix, (_, p50_vals, p99_vals) in key_map:
        if p50_vals:
            breakdown[f"{key_prefix}_p50_ms"] = (sum(p50_vals) / len(p50_vals)) * 1000
        if p99_vals:
            breakdown[f"{key_prefix}_p99_ms"] = (sum(p99_vals) / len(p99_vals)) * 1000

    # Derived: ART lookup = routing - tokenization
    if breakdown["routing_latency_p50_ms"] is not None and breakdown["tokenization_latency_p50_ms"] is not None:
        breakdown["art_lookup_latency_p50_ms"] = max(0, breakdown["routing_latency_p50_ms"] - breakdown["tokenization_latency_p50_ms"])
    if breakdown["routing_latency_p99_ms"] is not None and breakdown["tokenization_latency_p99_ms"] is not None:
        breakdown["art_lookup_latency_p99_ms"] = max(0, breakdown["routing_latency_p99_ms"] - breakdown["tokenization_latency_p99_ms"])

    return breakdown


def register_backends_on_all_nodes():
    """Register backends on all Ranvier nodes."""
    global _backends_registered
    if _backends_registered:
        return

    logger.info("Registering backends on all Ranvier nodes...")

    for node_url in RANVIER_NODES:
        for backend in BACKENDS:
            url = (
                f"{node_url}/admin/backends"
                f"?id={backend['id']}"
                f"&ip={backend['ip']}"
                f"&port={backend['port']}"
            )
            try:
                resp = requests.post(url, timeout=10)
                if resp.status_code == 200:
                    logger.info(f"Registered backend {backend['id']} on {node_url}")
                else:
                    logger.warning(
                        f"Failed to register backend {backend['id']} on {node_url}: "
                        f"{resp.status_code} - {resp.text}"
                    )
            except requests.exceptions.RequestException as e:
                logger.error(f"Error registering backend {backend['id']} on {node_url}: {e}")

    _backends_registered = True
    logger.info("Backend registration complete")


def capture_initial_sync_errors():
    """Capture initial sync error counts from all nodes.

    Tracks two Prometheus counters from gossip_service.cpp:
    - router_cluster_sync_invalid: malformed gossip packets received
    - router_cluster_sync_untrusted: packets received from unknown peers
    """
    global _initial_sync_errors
    _initial_sync_errors = {}

    for i, metrics_url in enumerate(RANVIER_METRICS):
        node_name = f"node{i + 1}"
        invalid = get_metric_value(metrics_url, "router_cluster_sync_invalid") or 0.0
        untrusted = get_metric_value(metrics_url, "router_cluster_sync_untrusted") or 0.0
        _initial_sync_errors[node_name] = {"invalid": invalid, "untrusted": untrusted}
        logger.info(f"Initial sync errors on {node_name}: invalid={invalid}, untrusted={untrusted}")


def check_sync_errors() -> tuple[bool, str]:
    """Check if any sync errors occurred during the test run.

    Returns:
        Tuple of (passed, message)
    """
    errors_found = []

    for i, metrics_url in enumerate(RANVIER_METRICS):
        node_name = f"node{i + 1}"
        invalid = get_metric_value(metrics_url, "router_cluster_sync_invalid")
        untrusted = get_metric_value(metrics_url, "router_cluster_sync_untrusted")

        initial = _initial_sync_errors.get(node_name, {"invalid": 0.0, "untrusted": 0.0})

        if invalid is not None:
            delta = invalid - initial["invalid"]
            if delta > 0:
                errors_found.append(f"{node_name}: {delta} new invalid sync packets")

        if untrusted is not None:
            delta = untrusted - initial["untrusted"]
            if delta > 0:
                errors_found.append(f"{node_name}: {delta} new untrusted sync packets")

    if errors_found:
        return False, f"Sync errors detected: {', '.join(errors_found)}"

    return True, "No sync errors detected"


@events.test_start.add_listener
def on_test_start(environment, **kwargs):
    """Called when the test starts."""
    # Only run setup on master or in standalone mode
    if isinstance(environment.runner, WorkerRunner):
        return

    logger.info("=" * 60)
    logger.info("Starting Ranvier Load Test")
    logger.info(f"P99 Latency Threshold: {P99_LATENCY_THRESHOLD_MS}ms")

    # Log wait_time configuration for debugging
    wait_min = os.environ.get('LOCUST_WAIT_MIN', '1')
    wait_max = os.environ.get('LOCUST_WAIT_MAX', '3')
    logger.info(f"Wait time: LOCUST_WAIT_MIN={wait_min}s, LOCUST_WAIT_MAX={wait_max}s")
    logger.info("=" * 60)

    # Register backends
    register_backends_on_all_nodes()

    # Capture initial sync error counts
    capture_initial_sync_errors()

    # Wait a moment for backends to be ready
    time.sleep(2)

    logger.info("Load test initialization complete")


@events.test_stop.add_listener
def on_test_stop(environment, **kwargs):
    """Called when the test stops."""
    # Only run validation on master or in standalone mode
    if isinstance(environment.runner, WorkerRunner):
        return

    logger.info("=" * 60)
    logger.info("Validating Test Results")
    logger.info("=" * 60)

    validation_passed = True
    validation_messages = []

    # Check sync errors
    sync_passed, sync_msg = check_sync_errors()
    validation_messages.append(sync_msg)
    if not sync_passed:
        validation_passed = False
        logger.error(f"FAIL: {sync_msg}")
    else:
        logger.info(f"PASS: {sync_msg}")

    # Check P99 latency (using TTFT custom metric)
    stats = environment.stats
    ttft_stats = stats.get("TTFT (Time To First Token)", "GET")

    if ttft_stats and ttft_stats.num_requests > 0:
        p99_latency = ttft_stats.get_response_time_percentile(0.99)
        if p99_latency is not None:
            p99_ms = p99_latency  # Already in ms
            if p99_ms > P99_LATENCY_THRESHOLD_MS:
                validation_passed = False
                msg = f"P99 TTFT latency {p99_ms:.2f}ms exceeds {P99_LATENCY_THRESHOLD_MS}ms threshold"
                validation_messages.append(msg)
                logger.error(f"FAIL: {msg}")
            else:
                msg = f"P99 TTFT latency {p99_ms:.2f}ms is within {P99_LATENCY_THRESHOLD_MS}ms threshold"
                validation_messages.append(msg)
                logger.info(f"PASS: {msg}")
    else:
        # Fall back to checking main request stats
        for name, entry in stats.entries.items():
            if "chat/completions" in str(name):
                p99_latency = entry.get_response_time_percentile(0.99)
                if p99_latency is not None and p99_latency > P99_LATENCY_THRESHOLD_MS:
                    validation_passed = False
                    msg = f"P99 latency for {name} is {p99_latency:.2f}ms (exceeds {P99_LATENCY_THRESHOLD_MS}ms)"
                    validation_messages.append(msg)
                    logger.error(f"FAIL: {msg}")

    # Scrape Ranvier internal latency breakdown from Prometheus
    latency_breakdown = get_ranvier_latency_breakdown()

    # Log latency breakdown table
    logger.info("Ranvier Internal Latency Breakdown:")
    logger.info(f"  {'Metric':<40} {'P50 (ms)':>10} {'P99 (ms)':>10}")
    logger.info(f"  {'-'*40} {'-'*10} {'-'*10}")
    for key_prefix, label in [
        ("routing_latency", "Routing (total)"),
        ("tokenization_latency", "Tokenization (total)"),
        ("primary_tokenization_latency", "  Primary tokenization"),
        ("boundary_detection_latency", "  Boundary detection"),
        ("art_lookup_latency", "ART lookup (derived)"),
        ("connect_latency", "Backend connect"),
    ]:
        p50 = latency_breakdown.get(f"{key_prefix}_p50_ms")
        p99 = latency_breakdown.get(f"{key_prefix}_p99_ms")
        p50_str = f"{p50:.3f}" if p50 is not None else "N/A"
        p99_str = f"{p99:.3f}" if p99 is not None else "N/A"
        logger.info(f"  {label:<40} {p50_str:>10} {p99_str:>10}")

    # Build summary and emit BENCHMARK_STATS_JSON for results_parser.py
    summary = {
        "total_requests": stats.total.num_requests if hasattr(stats, 'total') else 0,
        "failed_requests": stats.total.num_failures if hasattr(stats, 'total') else 0,
        "validation_passed": validation_passed,
    }
    summary.update(latency_breakdown)
    logger.info("BENCHMARK_STATS_JSON:" + json.dumps(summary))

    # Log summary
    logger.info("=" * 60)
    if validation_passed:
        logger.info("BENCHMARK PASSED - All validations successful")
    else:
        logger.error("BENCHMARK FAILED - Validation errors detected")
        for msg in validation_messages:
            logger.info(f"  - {msg}")
    logger.info("=" * 60)

    # Set exit code for headless mode
    if not validation_passed:
        environment.process_exit_code = 1


class ChatCompletionUser(HttpUser):
    """Simulates users sending chat completion requests to the cluster."""

    # Wait between requests (configurable via env for CI testing)
    # Default: 1-3s for realistic user simulation
    # CI mode: Set LOCUST_WAIT_MIN=0.1 LOCUST_WAIT_MAX=0.5 for higher throughput
    wait_time = between(
        float(os.environ.get('LOCUST_WAIT_MIN', '1')),
        float(os.environ.get('LOCUST_WAIT_MAX', '3'))
    )

    # Sample prompts for variety
    PROMPTS = [
        "Hello, how are you today?",
        "What is the capital of France?",
        "Explain quantum computing in simple terms.",
        "Write a haiku about programming.",
        "What's the weather like?",
        "Tell me a joke.",
        "How do I make pasta?",
        "What is machine learning?",
        "Describe the ocean.",
        "Why is the sky blue?",
    ]

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._request_count = 0

    @task
    def chat_completion(self):
        """Send requests to different nodes in round-robin fashion."""
        # Round-robin across nodes
        node_index = self._request_count % len(RANVIER_NODES)
        self._request_count += 1

        prompt = random.choice(self.PROMPTS)

        request_body = {
            "model": "test-model",
            "messages": [{"role": "user", "content": prompt}],
            "stream": True,
        }

        # Get the target node URL
        target_url = RANVIER_NODES[node_index]

        start_time = time.perf_counter()
        ttft = None
        error_occurred = False

        try:
            # Use requests library directly for proper SSE streaming support
            # Timeout: (connect, read) - shorter for mock backends in CI
            resp = requests.post(
                f"{target_url}/v1/chat/completions",
                json=request_body,
                headers={"Content-Type": "application/json"},
                stream=True,
                timeout=(5, 10),
            )

            if resp.status_code != 200:
                error_occurred = True
                events.request.fire(
                    request_type="POST",
                    name=f"/v1/chat/completions (node{node_index + 1})",
                    response_time=(time.perf_counter() - start_time) * 1000,
                    response_length=0,
                    exception=Exception(f"Status: {resp.status_code}"),
                    context={},
                )
            else:
                for line in resp.iter_lines():
                    if line:
                        decoded = line.decode("utf-8")
                        if decoded.startswith("data: "):
                            if ttft is None:
                                ttft = (time.perf_counter() - start_time) * 1000

                            if decoded == "data: [DONE]":
                                break

                total_time = (time.perf_counter() - start_time) * 1000

                events.request.fire(
                    request_type="POST",
                    name=f"/v1/chat/completions (node{node_index + 1})",
                    response_time=total_time,
                    response_length=0,
                    exception=None,
                    context={},
                )

                if ttft is not None:
                    events.request.fire(
                        request_type="GET",
                        name="TTFT (Time To First Token)",
                        response_time=ttft,
                        response_length=0,
                        exception=None,
                        context={},
                    )

        except Exception as e:
            total_time = (time.perf_counter() - start_time) * 1000
            events.request.fire(
                request_type="POST",
                name=f"/v1/chat/completions (node{node_index + 1})",
                response_time=total_time,
                response_length=0,
                exception=e,
                context={},
            )
            logger.warning(f"Request to node{node_index + 1} failed: {e}")
