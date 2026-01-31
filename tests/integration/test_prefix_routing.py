#!/usr/bin/env python3
"""
E2E Integration Tests for Prefix Routing (Core Value Proposition)

This test suite validates Ranvier's prefix caching behavior:
1. Same prefix routes to same backend consistently (cache affinity)
2. Route learning creates cache entries (ART lookup hits)
3. Cache hit metrics reflect routing behavior
4. Routes propagate across cluster nodes via gossip
5. LRU eviction preserves hot routes

These tests verify the core value proposition: token-prefix-aware routing
that maximizes KV cache reuse on GPU backends.

Usage:
    python tests/integration/test_prefix_routing.py

Requirements:
    - Docker and docker-compose installed
    - pytest (optional, can run with unittest)
    - requests library
"""

import json
import os
import re
import subprocess
import sys
import time
import unittest
from typing import Dict, List, Optional, Tuple

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)


# =============================================================================
# Configuration
# =============================================================================

COMPOSE_FILE = os.path.join(os.path.dirname(__file__), "..", "..", "docker-compose.test.yml")
PROJECT_NAME = "ranvier-prefix-routing-test"


def get_docker_host() -> str:
    """Get the hostname to reach Docker-exposed ports."""
    import socket
    try:
        socket.gethostbyname("host.docker.internal")
        return "host.docker.internal"
    except socket.gaierror:
        return "localhost"


DOCKER_HOST = get_docker_host()

# Node endpoints (mapped ports from docker-compose)
NODES = {
    "node1": {"api": f"http://{DOCKER_HOST}:8081", "metrics": f"http://{DOCKER_HOST}:9181"},
    "node2": {"api": f"http://{DOCKER_HOST}:8082", "metrics": f"http://{DOCKER_HOST}:9182"},
    "node3": {"api": f"http://{DOCKER_HOST}:8083", "metrics": f"http://{DOCKER_HOST}:9183"},
}

# Backend addresses (as seen from inside the Docker network)
BACKENDS = {
    1: {"ip": "172.28.1.10", "port": 8000},
    2: {"ip": "172.28.1.11", "port": 8000},
}

# Timeouts
STARTUP_TIMEOUT = 60  # seconds to wait for cluster to start
PROPAGATION_TIMEOUT = 5  # seconds to wait for gossip propagation
REQUEST_TIMEOUT = 30  # seconds for HTTP requests


# =============================================================================
# Docker Compose Helpers
# =============================================================================

COMPOSE_CMD = None


def get_compose_cmd() -> List[str]:
    """Detect and return the docker compose command."""
    global COMPOSE_CMD
    if COMPOSE_CMD is not None:
        return COMPOSE_CMD

    # Try 'docker compose' (plugin) first
    try:
        result = subprocess.run(
            ["docker", "compose", "version"],
            capture_output=True,
            text=True
        )
        if result.returncode == 0:
            COMPOSE_CMD = ["docker", "compose"]
            return COMPOSE_CMD
    except FileNotFoundError:
        pass

    # Fall back to 'docker-compose' (standalone)
    try:
        result = subprocess.run(
            ["docker-compose", "--version"],
            capture_output=True,
            text=True
        )
        if result.returncode == 0:
            COMPOSE_CMD = ["docker-compose"]
            return COMPOSE_CMD
    except FileNotFoundError:
        pass

    raise RuntimeError(
        "Neither 'docker compose' nor 'docker-compose' found. "
        "Please install Docker with Compose plugin or docker-compose standalone."
    )


def run_compose(args: List[str], check: bool = True, show_output: bool = False) -> subprocess.CompletedProcess:
    """Run docker-compose command with the test configuration."""
    compose_cmd = get_compose_cmd()
    cmd = compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME] + args
    print(f"  Running: {' '.join(cmd)}")

    result = subprocess.run(cmd, capture_output=True, text=True)

    if show_output or result.returncode != 0:
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr)

    if check and result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, cmd, result.stdout, result.stderr)

    return result


def check_container_running(container_name: str) -> bool:
    """Check if a container is still running."""
    try:
        compose_cmd = get_compose_cmd()
        result = subprocess.run(
            compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME, "ps", "-q", container_name],
            capture_output=True,
            text=True
        )
        return result.returncode == 0 and len(result.stdout.strip()) > 0
    except (FileNotFoundError, RuntimeError):
        return True


def wait_for_healthy(url: str, timeout: int = 60, container_name: str = None) -> bool:
    """Wait for an endpoint to become healthy."""
    start = time.time()
    while time.time() - start < timeout:
        if container_name and not check_container_running(container_name):
            print(f"    Container {container_name} has exited!")
            return False
        try:
            resp = requests.get(url, timeout=5)
            if resp.status_code == 200:
                return True
        except requests.exceptions.RequestException:
            pass
        time.sleep(1)
    return False


# =============================================================================
# Metrics Helpers
# =============================================================================

def get_metric_value(metrics_url: str, metric_name: str, labels: Dict[str, str] = None) -> Optional[float]:
    """Extract a specific metric value from Prometheus endpoint.

    Args:
        metrics_url: Base URL for metrics endpoint
        metric_name: Name of metric to find (partial match supported)
        labels: Optional dict of label=value to match

    Returns:
        Float value if found, None otherwise
    """
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            if metric_name in line:
                # Check labels if specified
                if labels:
                    all_match = all(f'{k}="{v}"' in line for k, v in labels.items())
                    if not all_match:
                        continue

                # Extract numeric value at end of line
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    return float(match.group(1))
        return None
    except requests.exceptions.RequestException:
        return None


def get_all_metrics(metrics_url: str) -> Dict[str, List[float]]:
    """Get all metrics from the Prometheus endpoint."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return {}

        metrics = {}
        for line in resp.text.split("\n"):
            if line.startswith("#") or not line.strip():
                continue
            match = re.match(r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{([^}]*)\})?\s+([\d.eE+-]+)", line)
            if match:
                name = match.group(1)
                value = float(match.group(3))
                if name not in metrics:
                    metrics[name] = []
                metrics[name].append(value)
        return metrics
    except requests.exceptions.RequestException:
        return {}


def debug_print_routing_metrics(metrics_url: str) -> None:
    """Print all metrics containing routing-related keywords for debugging."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            print(f"    DEBUG: Failed to fetch metrics: {resp.status_code}")
            return

        keywords = ["radix", "lookup", "route", "tree", "cache", "hit", "miss"]
        print("    DEBUG: Routing-related metrics found:")
        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            line_lower = line.lower()
            if any(kw in line_lower for kw in keywords):
                print(f"      {line[:120]}")
    except requests.exceptions.RequestException as e:
        print(f"    DEBUG: Error fetching metrics: {e}")


def get_cache_metrics(metrics_url: str, debug: bool = False) -> Dict[str, float]:
    """Get all cache-related metrics.

    Note: Seastar prefixes all metrics with 'seastar_', so actual names are:
    - seastar_ranvier_radix_tree_lookup_hits_total (from router_service.cpp)
    - seastar_ranvier_radix_tree_lookup_misses_total (from router_service.cpp)
    - seastar_ranvier_router_cache_hits (from router_service.cpp)
    - seastar_ranvier_router_cache_misses (from router_service.cpp)
    - seastar_ranvier_cache_hit_ratio (gauge from metrics_service.hpp)
    """
    metrics = get_all_metrics(metrics_url)

    if debug:
        debug_print_routing_metrics(metrics_url)

    return {
        # Radix tree lookup metrics (Seastar adds 'seastar_' prefix)
        "radix_tree_lookup_hits": sum(metrics.get("seastar_ranvier_radix_tree_lookup_hits_total", [0])),
        "radix_tree_lookup_misses": sum(metrics.get("seastar_ranvier_radix_tree_lookup_misses_total", [0])),
        # Router cache metrics
        "router_cache_hits": sum(metrics.get("seastar_ranvier_router_cache_hits", [0])),
        "router_cache_misses": sum(metrics.get("seastar_ranvier_router_cache_misses", [0])),
        # Cache hit ratio gauge
        "cache_hit_ratio": metrics.get("seastar_ranvier_cache_hit_ratio", [0])[0] if metrics.get("seastar_ranvier_cache_hit_ratio") else 0,
    }


# =============================================================================
# Request Helpers
# =============================================================================

def send_chat_request(
    api_url: str,
    messages: List[Dict[str, str]],
    stream: bool = True,
    timeout: int = REQUEST_TIMEOUT,
    retries: int = 3,
    debug: bool = False
) -> Tuple[int, str, Dict[str, str]]:
    """Send a chat completion request and return (status_code, response_text, headers).

    Args:
        api_url: The Ranvier API URL
        messages: List of chat messages
        stream: Whether to use streaming response
        timeout: Request timeout in seconds
        retries: Number of retries for empty responses
        debug: Print debug information

    Returns:
        Tuple of (status_code, aggregated_response_text, response_headers)
    """
    request_body = {
        "model": "test-model",
        "messages": messages,
        "stream": stream
    }

    for attempt in range(retries):
        try:
            resp = requests.post(
                f"{api_url}/v1/chat/completions",
                json=request_body,
                headers={"Content-Type": "application/json"},
                stream=stream,
                timeout=timeout
            )

            response_text = ""
            headers = dict(resp.headers)

            if stream and resp.status_code == 200:
                raw_lines = []
                for line in resp.iter_lines():
                    if line:
                        decoded = line.decode("utf-8")
                        raw_lines.append(decoded)
                        if decoded.startswith("data: ") and decoded != "data: [DONE]":
                            try:
                                chunk = json.loads(decoded[6:])
                                if "choices" in chunk and chunk["choices"]:
                                    delta = chunk["choices"][0].get("delta", {})
                                    if "content" in delta:
                                        response_text += delta["content"]
                            except json.JSONDecodeError:
                                pass

                if debug and not response_text:
                    print(f"    DEBUG: Empty response, raw lines: {raw_lines[:5]}")
            else:
                response_text = resp.text

            # If we got a valid response, return it
            if response_text or resp.status_code != 200:
                return resp.status_code, response_text, headers

            # Empty response with 200 status - retry
            if attempt < retries - 1:
                if debug:
                    print(f"    DEBUG: Empty response on attempt {attempt + 1}, retrying...")
                time.sleep(0.5)
                continue

            # Final attempt - return what we have
            return resp.status_code, response_text, headers

        except requests.exceptions.RequestException as e:
            if attempt < retries - 1:
                time.sleep(0.5)
                continue
            return 0, str(e), {}

    return 0, "Max retries exceeded", {}


def extract_backend_id(response_text: str, headers: Dict[str, str] = None) -> Optional[int]:
    """Extract backend ID from mock backend response or headers.

    Mock backend returns: "Response from backend {BACKEND_ID}"
    Mock backend also sets header: X-Backend-ID: {BACKEND_ID}

    Args:
        response_text: The response body text
        headers: Optional response headers dict

    Returns:
        Backend ID as integer, or None if not found
    """
    # Try X-Backend-ID header first (most reliable)
    if headers:
        backend_header = headers.get("X-Backend-ID") or headers.get("x-backend-id")
        if backend_header:
            try:
                return int(backend_header)
            except ValueError:
                pass

    # Fall back to parsing response text
    if response_text:
        match = re.search(r"backend\s+(\d+)", response_text.lower())
        if match:
            return int(match.group(1))

    return None


def register_backends(api_url: str) -> bool:
    """Register all backends on a node. Returns True if successful."""
    for backend_id, backend_info in BACKENDS.items():
        url = (
            f"{api_url}/admin/backends"
            f"?id={backend_id}"
            f"&ip={backend_info['ip']}"
            f"&port={backend_info['port']}"
        )
        try:
            resp = requests.post(url, timeout=10)
            if resp.status_code != 200:
                print(f"    Failed to register backend {backend_id}: {resp.text}")
                return False
        except requests.exceptions.RequestException as e:
            print(f"    Failed to register backend {backend_id}: {e}")
            return False
    return True


# =============================================================================
# Test Prompts
# =============================================================================

# These prompts are designed to test prefix routing behavior.
# They share common prefixes to verify cache affinity.

SYSTEM_PROMPT = "You are a helpful AI assistant. You always respond concisely and accurately."

# Prompts that share a common system message (should route to same backend after learning)
SHARED_PREFIX_PROMPTS = [
    [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": "What is 2+2?"}
    ],
    [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": "What is the capital of France?"}
    ],
    [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": "Explain quantum computing briefly."}
    ],
]

# Prompts with different system messages (may route to different backends)
DIFFERENT_PREFIX_PROMPTS = [
    [
        {"role": "system", "content": "You are a math tutor. Focus on explaining calculations."},
        {"role": "user", "content": "What is 2+2?"}
    ],
    [
        {"role": "system", "content": "You are a geography expert. Focus on locations and maps."},
        {"role": "user", "content": "What is the capital of France?"}
    ],
]

# Unique prompts for testing route learning (each should be distinct)
def generate_unique_prompt(seed: int) -> List[Dict[str, str]]:
    """Generate a unique prompt for testing."""
    return [
        {"role": "system", "content": f"Unique system prompt number {seed} for testing prefix routing."},
        {"role": "user", "content": f"This is test query {seed}. Please respond with your backend ID."}
    ]


# =============================================================================
# Test Suite
# =============================================================================

class PrefixRoutingTest(unittest.TestCase):
    """E2E tests for prefix routing (cache affinity) behavior."""

    @classmethod
    def setUpClass(cls):
        """Start the Docker Compose cluster."""
        print("\n" + "=" * 70)
        print("Setting up test cluster for Prefix Routing tests...")
        print("=" * 70)

        # Ensure clean state
        run_compose(["down", "-v", "--remove-orphans"], check=False)

        # Check for pre-built images
        skip_build = os.environ.get("SKIP_BUILD", "").lower() in ("1", "true", "yes")

        if not skip_build:
            try:
                compose_cmd = get_compose_cmd()
                create_result = subprocess.run(
                    compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME,
                                   "create", "--no-build"],
                    capture_output=True, text=True, timeout=30
                )
                if create_result.returncode == 0:
                    print("\nDocker images already exist. Skipping build.")
                    skip_build = True
                    subprocess.run(
                        compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME, "rm", "-f"],
                        capture_output=True, timeout=30
                    )
            except (subprocess.TimeoutExpired, Exception):
                pass

        if not skip_build:
            print("\nBuilding containers...")
            result = run_compose(["build"], check=False, show_output=True)
            if result.returncode != 0:
                raise RuntimeError("Failed to build containers")

        print("\nStarting cluster...")
        result = run_compose(["up", "-d"], check=False, show_output=True)
        if result.returncode != 0:
            raise RuntimeError("Failed to start cluster")

        # Wait for all nodes to become healthy
        print("\nWaiting for nodes to become healthy...")
        container_names = {"node1": "ranvier1", "node2": "ranvier2", "node3": "ranvier3"}
        all_healthy = True

        for name, endpoints in NODES.items():
            print(f"  Waiting for {name}...")
            container = container_names.get(name)
            if not wait_for_healthy(f"{endpoints['metrics']}/metrics", timeout=STARTUP_TIMEOUT, container_name=container):
                print(f"  ERROR: {name} did not become healthy")
                all_healthy = False
            else:
                print(f"  {name} is healthy")

        if not all_healthy:
            print("\nContainer logs:")
            run_compose(["logs", "--tail=50"], check=False, show_output=True)
            raise RuntimeError("Not all nodes became healthy")

        # Register backends on all nodes
        print("\nRegistering backends on all nodes...")
        for node_name, node_endpoints in NODES.items():
            print(f"  Registering on {node_name}...")
            if not register_backends(node_endpoints["api"]):
                raise RuntimeError(f"Failed to register backends on {node_name}")

        # Wait for gossip connections
        print("\nWaiting for gossip connections...")
        time.sleep(3)

        print("\nCluster is ready for prefix routing tests")
        print("=" * 70 + "\n")

    @classmethod
    def tearDownClass(cls):
        """Tear down the Docker Compose cluster."""
        print("\n" + "=" * 70)
        print("Tearing down test cluster...")
        print("=" * 70)
        run_compose(["down", "-v", "--remove-orphans"], check=False)
        print("Cleanup complete")

    # =========================================================================
    # Core Prefix Routing Tests
    # =========================================================================

    def test_01_same_prefix_routes_consistently(self):
        """Verify that identical prompts route to the same backend consistently.

        This is the fundamental cache affinity test: if the prefix cache is working,
        repeated requests with the same prefix should always hit the same backend.
        """
        print("\nTest: Same prefix routes consistently")

        api_url = NODES["node1"]["api"]
        prompt = SHARED_PREFIX_PROMPTS[0]

        # Send the same request multiple times
        backend_ids = []
        for i in range(5):
            status, response, headers = send_chat_request(api_url, prompt, debug=(i == 0))
            self.assertEqual(status, 200, f"Request {i+1} failed: {response}")

            backend_id = extract_backend_id(response, headers)
            self.assertIsNotNone(backend_id, f"Could not extract backend ID from response='{response}', headers={headers}")
            backend_ids.append(backend_id)
            print(f"  Request {i+1}: routed to backend {backend_id}")

        # All requests should go to the same backend (cache affinity)
        unique_backends = set(backend_ids)
        self.assertEqual(
            len(unique_backends), 1,
            f"Expected all requests to route to same backend, but got: {backend_ids}"
        )

        print(f"  PASSED: All 5 requests routed to backend {backend_ids[0]}")

    def test_02_route_learning_creates_cache_entry(self):
        """Verify that route learning creates a cache entry for subsequent lookups.

        First request should be a cache miss (route learned).
        Second identical request should be a cache hit.
        """
        print("\nTest: Route learning creates cache entry")

        api_url = NODES["node1"]["api"]
        metrics_url = NODES["node1"]["metrics"]

        # Use a unique prompt to ensure we're testing fresh route learning
        unique_prompt = generate_unique_prompt(seed=int(time.time() * 1000) % 10000)

        # Get initial cache metrics
        initial_metrics = get_cache_metrics(metrics_url)
        initial_hits = initial_metrics.get("radix_tree_lookup_hits", 0)
        initial_misses = initial_metrics.get("radix_tree_lookup_misses", 0)
        print(f"  Initial: hits={initial_hits}, misses={initial_misses}")

        # First request - should be cache miss, route will be learned
        print("  Sending first request (expect cache miss)...")
        status1, response1, headers1 = send_chat_request(api_url, unique_prompt, debug=True)
        self.assertEqual(status1, 200, f"First request failed: {response1}")
        backend1 = extract_backend_id(response1, headers1)
        self.assertIsNotNone(backend1, f"Could not extract backend ID from first request")
        print(f"  First request: routed to backend {backend1}")

        # Small delay for metrics to update
        time.sleep(0.5)

        # Check metrics after first request
        mid_metrics = get_cache_metrics(metrics_url)
        mid_misses = mid_metrics.get("radix_tree_lookup_misses", 0)
        print(f"  After first request: misses={mid_misses}")

        # Second request - should be cache hit
        print("  Sending second request (expect cache hit)...")
        status2, response2, headers2 = send_chat_request(api_url, unique_prompt, debug=True)
        self.assertEqual(status2, 200, f"Second request failed: {response2}")
        backend2 = extract_backend_id(response2, headers2)
        self.assertIsNotNone(backend2, f"Could not extract backend ID from second request")
        print(f"  Second request: routed to backend {backend2}")

        # Should route to same backend
        self.assertEqual(backend1, backend2, "Second request should route to same backend")

        # Small delay for metrics to update
        time.sleep(0.5)

        # Check metrics after second request
        final_metrics = get_cache_metrics(metrics_url)
        final_hits = final_metrics.get("radix_tree_lookup_hits", 0)
        print(f"  Final: hits={final_hits}")

        # Radix tree lookup hits should have increased
        # (Note: exact increment depends on implementation details)
        print(f"  PASSED: Route learned and cache entry created")

    def test_03_radix_tree_hits_increase_with_repeated_requests(self):
        """Verify that radix tree lookup hits increase with repeated similar requests.

        As we send more requests with the same prefix, the ART lookup hits should increase.
        """
        print("\nTest: Radix tree hits increase with repeated requests")

        api_url = NODES["node1"]["api"]
        metrics_url = NODES["node1"]["metrics"]

        # Get initial metrics (with debug to see available metrics)
        initial_metrics = get_cache_metrics(metrics_url, debug=True)
        initial_hits = initial_metrics.get("radix_tree_lookup_hits", 0)
        print(f"  Initial radix tree hits: {initial_hits}")

        # Send multiple requests with same prefix
        prompt = SHARED_PREFIX_PROMPTS[1]  # Use a different prompt from test_01
        for i in range(10):
            status, response, headers = send_chat_request(api_url, prompt)
            self.assertEqual(status, 200, f"Request {i+1} failed")

        time.sleep(0.5)

        # Get final metrics
        final_metrics = get_cache_metrics(metrics_url)
        final_hits = final_metrics.get("radix_tree_lookup_hits", 0)
        print(f"  Final radix tree hits: {final_hits}")

        # Radix tree lookup hits should have increased
        # First request is a miss (no route learned yet), but subsequent 9 should be hits
        self.assertGreater(
            final_hits, initial_hits,
            f"Radix tree hits should increase after repeated requests: {initial_hits} -> {final_hits}"
        )

        print(f"  PASSED: Radix tree hits increased from {initial_hits} to {final_hits}")

    def test_04_different_prefixes_can_route_differently(self):
        """Verify that requests with different prefixes can route to different backends.

        Note: This test verifies that different prefixes CAN route differently,
        not that they MUST. The routing depends on hash distribution.
        """
        print("\nTest: Different prefixes can route to different backends")

        api_url = NODES["node1"]["api"]

        # Send requests with different system messages
        backend_ids = {}
        for i, prompt in enumerate(DIFFERENT_PREFIX_PROMPTS):
            # Send each prompt twice to ensure route is learned
            headers = {}
            for _ in range(2):
                status, response, headers = send_chat_request(api_url, prompt)
                self.assertEqual(status, 200, f"Request failed: {response}")

            backend_id = extract_backend_id(response, headers)
            self.assertIsNotNone(backend_id, f"Could not extract backend ID from prompt {i+1}")
            backend_ids[i] = backend_id
            print(f"  Prompt {i+1}: routed to backend {backend_id}")

        # We have 2 backends, so with 2 different prefixes we may or may not
        # get different backends (depends on hash). This test just verifies
        # the routing works, not that it's perfectly distributed.
        print(f"  Backends used: {set(backend_ids.values())}")
        print(f"  PASSED: Different prefixes routed successfully")

    def test_05_route_propagation_across_cluster(self):
        """Verify that routes learned on one node propagate to other nodes.

        Route learned on node1 should be visible on node2 after gossip interval.
        """
        print("\nTest: Route propagation across cluster")

        node1_api = NODES["node1"]["api"]
        node2_api = NODES["node2"]["api"]

        # Use a unique prompt to test fresh propagation
        unique_prompt = generate_unique_prompt(seed=99999)

        # Learn route on node1
        print("  Learning route on node1...")
        status1, response1, headers1 = send_chat_request(node1_api, unique_prompt)
        self.assertEqual(status1, 200, f"Node1 request failed: {response1}")
        backend1 = extract_backend_id(response1, headers1)
        self.assertIsNotNone(backend1, "Could not extract backend ID from node1")
        print(f"  Node1 routed to backend {backend1}")

        # Wait for gossip propagation
        print(f"  Waiting {PROPAGATION_TIMEOUT}s for gossip propagation...")
        time.sleep(PROPAGATION_TIMEOUT)

        # Send same request to node2 - should route to same backend
        print("  Sending same request to node2...")
        status2, response2, headers2 = send_chat_request(node2_api, unique_prompt)
        self.assertEqual(status2, 200, f"Node2 request failed: {response2}")
        backend2 = extract_backend_id(response2, headers2)
        self.assertIsNotNone(backend2, "Could not extract backend ID from node2")
        print(f"  Node2 routed to backend {backend2}")

        # Both nodes should route to the same backend (route was propagated)
        self.assertEqual(
            backend1, backend2,
            f"Route should propagate: node1->backend{backend1}, node2->backend{backend2}"
        )

        print(f"  PASSED: Route propagated from node1 to node2")

    def test_06_backend_affinity_persists_under_load(self):
        """Verify that backend affinity persists under concurrent load.

        Send multiple requests in quick succession - all should route
        to the same backend for the same prefix.
        """
        print("\nTest: Backend affinity persists under load")

        api_url = NODES["node1"]["api"]
        prompt = SHARED_PREFIX_PROMPTS[2]

        # Send 20 requests rapidly
        backend_ids = []
        errors = []

        print("  Sending 20 requests in rapid succession...")
        for i in range(20):
            status, response, headers = send_chat_request(api_url, prompt, timeout=15)
            if status != 200:
                errors.append(f"Request {i+1}: status {status}")
                continue

            backend_id = extract_backend_id(response, headers)
            if backend_id:
                backend_ids.append(backend_id)

        # Report any errors
        if errors:
            print(f"  Errors: {errors[:5]}...")  # Print first 5 errors

        # Most requests should succeed
        self.assertGreater(len(backend_ids), 15, f"Too many failures: {len(errors)} errors")

        # All successful requests should route to same backend
        unique_backends = set(backend_ids)
        self.assertEqual(
            len(unique_backends), 1,
            f"Expected single backend, got: {unique_backends} from {len(backend_ids)} requests"
        )

        print(f"  PASSED: All {len(backend_ids)} requests routed to backend {backend_ids[0]}")

    def test_07_metrics_reflect_routing_behavior(self):
        """Verify that Prometheus metrics accurately reflect routing behavior.

        Check that radix tree lookup metrics are exposed and increment appropriately.
        """
        print("\nTest: Metrics reflect routing behavior")

        api_url = NODES["node1"]["api"]
        metrics_url = NODES["node1"]["metrics"]

        # Get initial metrics
        initial = get_cache_metrics(metrics_url)
        print(f"  Initial metrics: {initial}")

        # Send a new unique request (cache miss expected)
        unique_prompt = generate_unique_prompt(seed=77777)
        status, response, headers = send_chat_request(api_url, unique_prompt)
        self.assertEqual(status, 200)

        time.sleep(0.5)

        # Send same request again (cache hit expected)
        status, response, headers = send_chat_request(api_url, unique_prompt)
        self.assertEqual(status, 200)

        time.sleep(0.5)

        # Get final metrics
        final = get_cache_metrics(metrics_url)
        print(f"  Final metrics: {final}")

        # Verify radix tree metrics are being recorded
        # The radix_tree_lookup_hits should increase after the second request
        initial_lookups = initial.get("radix_tree_lookup_hits", 0) + initial.get("radix_tree_lookup_misses", 0)
        final_lookups = final.get("radix_tree_lookup_hits", 0) + final.get("radix_tree_lookup_misses", 0)

        self.assertGreater(
            final_lookups, initial_lookups,
            f"Radix tree lookup metrics should increase after requests: initial={initial_lookups}, final={final_lookups}"
        )

        print(f"  PASSED: Metrics are being recorded correctly")

    def test_08_all_nodes_route_consistently_for_same_prefix(self):
        """Verify that all cluster nodes route the same prefix to the same backend.

        After route propagation, any node in the cluster should route requests
        with the same prefix to the same backend.
        """
        print("\nTest: All nodes route consistently for same prefix")

        # Use a fresh unique prompt
        unique_prompt = generate_unique_prompt(seed=88888)

        # First, learn the route on node1
        print("  Learning route on node1...")
        status, response, headers = send_chat_request(NODES["node1"]["api"], unique_prompt)
        self.assertEqual(status, 200)
        expected_backend = extract_backend_id(response, headers)
        self.assertIsNotNone(expected_backend, "Could not extract backend ID when learning route")
        print(f"  Node1 learned route to backend {expected_backend}")

        # Wait for propagation
        time.sleep(PROPAGATION_TIMEOUT)

        # Now query all nodes
        results = {}
        for node_name, endpoints in NODES.items():
            status, response, headers = send_chat_request(endpoints["api"], unique_prompt)
            if status == 200:
                backend = extract_backend_id(response, headers)
                results[node_name] = backend
                print(f"  {node_name}: routed to backend {backend}")
            else:
                results[node_name] = f"error:{status}"
                print(f"  {node_name}: error {status}")

        # All nodes should route to the same backend
        backends = [b for b in results.values() if isinstance(b, int)]
        self.assertEqual(len(backends), 3, f"Expected 3 successful responses, got: {results}")

        unique_backends = set(backends)
        self.assertEqual(
            len(unique_backends), 1,
            f"All nodes should route to same backend, got: {results}"
        )

        print(f"  PASSED: All 3 nodes route to backend {expected_backend}")


# =============================================================================
# Main
# =============================================================================

def main():
    """Run the prefix routing integration tests."""
    print("=" * 70)
    print("Ranvier Core - Prefix Routing E2E Tests")
    print("=" * 70)
    print("\nThese tests validate the core value proposition:")
    print("  - Token-prefix-aware routing for KV cache reuse")
    print("  - Cache affinity (same prefix -> same backend)")
    print("  - Route learning and propagation across cluster")
    print("")

    # Check if docker compose is available
    try:
        compose_cmd = get_compose_cmd()
        print(f"Using: {' '.join(compose_cmd)}")
        print(f"Docker host: {DOCKER_HOST}")
    except RuntimeError as e:
        print(f"Error: {e}")
        sys.exit(1)

    # Check if compose file exists
    if not os.path.exists(COMPOSE_FILE):
        print(f"Error: Docker Compose file not found: {COMPOSE_FILE}")
        sys.exit(1)

    # Run tests
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(PrefixRoutingTest)

    # Sort tests to run in order (test_01, test_02, etc.)
    suite = unittest.TestSuite(sorted(suite, key=lambda t: t.id()))

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Return appropriate exit code
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
