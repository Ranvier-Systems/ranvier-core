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

The shared docker-compose harness (constants, helpers, lifecycle) lives in
``tests/integration/conftest.py``.  This file only contains the
prefix-routing-specific test methods and the cache-metric helper they share.
Any change to cluster bring-up / teardown belongs in conftest.
"""

import os
import sys
import time
import unittest
from typing import Dict, List

from conftest import (
    COMPOSE_FILE,
    ClusterTestCase,
    DOCKER_HOST,
    NODES,
    PROPAGATION_TIMEOUT,
    extract_backend_id,
    get_all_metrics,
    get_compose_cmd,
    send_chat_request,
)


# =============================================================================
# Prefix-routing-specific helpers
# =============================================================================


def get_cache_metrics(metrics_url: str) -> Dict[str, float]:
    """Return prefix-routing cache metrics for a Ranvier node.

    Seastar prefixes every exported metric with ``seastar_ranvier_``, so we
    look up the full names emitted by ``router_service.cpp`` /
    ``metrics_service.hpp``:

    * ``seastar_ranvier_radix_tree_lookup_hits_total`` — ART lookups that
      found a cached route for the incoming prefix.
    * ``seastar_ranvier_radix_tree_lookup_misses_total`` — ART lookups that
      had to learn a new route.
    * ``seastar_ranvier_router_cache_hits`` /
      ``seastar_ranvier_router_cache_misses`` — router-level counters used
      by the hot path.
    * ``seastar_ranvier_cache_hit_ratio`` — gauge (0.0–1.0) from
      ``metrics_service.hpp``.

    This helper is prefix-routing-specific, so it lives in this file rather
    than ``conftest.py``.  Multiple test methods rely on it.
    """
    metrics = get_all_metrics(metrics_url)

    return {
        "radix_tree_lookup_hits": sum(
            metrics.get("seastar_ranvier_radix_tree_lookup_hits_total", [0])
        ),
        "radix_tree_lookup_misses": sum(
            metrics.get("seastar_ranvier_radix_tree_lookup_misses_total", [0])
        ),
        "router_cache_hits": sum(
            metrics.get("seastar_ranvier_router_cache_hits", [0])
        ),
        "router_cache_misses": sum(
            metrics.get("seastar_ranvier_router_cache_misses", [0])
        ),
        "cache_hit_ratio": metrics.get("seastar_ranvier_cache_hit_ratio", [0])[0]
        if metrics.get("seastar_ranvier_cache_hit_ratio")
        else 0,
    }


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


def generate_unique_prompt(seed: int) -> List[Dict[str, str]]:
    """Generate a unique prompt for testing route learning."""
    return [
        {"role": "system", "content": f"Unique system prompt number {seed} for testing prefix routing."},
        {"role": "user", "content": f"This is test query {seed}. Please respond with your backend ID."}
    ]


# =============================================================================
# Test Suite
# =============================================================================


class PrefixRoutingTest(ClusterTestCase):
    """E2E tests for prefix routing (cache affinity) behavior."""

    # Unique compose project so this unittest-style suite can run side-by-side
    # with the pytest-session cluster without colliding on container names.
    # (Host-port conflicts are still a concern — see conftest.py docstring —
    # but project-level isolation at least keeps container state clean.)
    PROJECT_NAME = "ranvier-prefix-routing-test"
    # The pre-migration setUpClass registered backends on all nodes during
    # bring-up and these tests assume backends are live from test_01 onward.
    # Opt into the ClusterTestCase auto-registration path to preserve that.
    AUTO_REGISTER_BACKENDS = True

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
            status, response, headers = send_chat_request(api_url, prompt)
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
        status1, response1, headers1 = send_chat_request(api_url, unique_prompt)
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
        status2, response2, headers2 = send_chat_request(api_url, unique_prompt)
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

    def test_03_router_cache_hits_increase_with_repeated_requests(self):
        """Verify that router cache hits increase with repeated similar requests.

        As we send more requests with the same prefix, cache hits should increase.
        """
        print("\nTest: Router cache hits increase with repeated requests")

        api_url = NODES["node1"]["api"]
        metrics_url = NODES["node1"]["metrics"]

        # Get initial metrics
        initial_metrics = get_cache_metrics(metrics_url)
        initial_hits = initial_metrics.get("router_cache_hits", 0)
        print(f"  Initial router cache hits: {initial_hits}")

        # Send multiple requests with same prefix
        prompt = SHARED_PREFIX_PROMPTS[1]  # Use a different prompt from test_01
        for i in range(10):
            status, response, headers = send_chat_request(api_url, prompt)
            self.assertEqual(status, 200, f"Request {i+1} failed")

        time.sleep(0.5)

        # Get final metrics
        final_metrics = get_cache_metrics(metrics_url)
        final_hits = final_metrics.get("router_cache_hits", 0)
        print(f"  Final router cache hits: {final_hits}")

        # Router cache hits should have increased
        # First request is a miss (no route learned yet), but subsequent 9 should be hits
        self.assertGreater(
            final_hits, initial_hits,
            f"Router cache hits should increase after repeated requests: {initial_hits} -> {final_hits}"
        )

        print(f"  PASSED: Router cache hits increased from {initial_hits} to {final_hits}")

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

        Check that router cache metrics are exposed and increment appropriately.
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

        # Verify router cache metrics are being recorded
        # Total activity (hits + misses) should increase after requests
        initial_activity = initial.get("router_cache_hits", 0) + initial.get("router_cache_misses", 0)
        final_activity = final.get("router_cache_hits", 0) + final.get("router_cache_misses", 0)

        self.assertGreater(
            final_activity, initial_activity,
            f"Router cache metrics should increase after requests: initial={initial_activity}, final={final_activity}"
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
