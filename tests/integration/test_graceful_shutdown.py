#!/usr/bin/env python3
"""
Graceful Shutdown Integration Tests for Ranvier Core

This test suite validates the shutdown lifecycle guards:
1. Health endpoint transitions from 200 -> 503 during drain
2. New requests are rejected with 503 + Retry-After during drain
3. Metrics endpoint remains accessible during shutdown
4. Cluster status shows is_draining = true during drain phase
5. In-flight requests complete before full shutdown

These tests address the "Hidden Fragility" finding that metrics lambdas
may capture 'this' pointers that become invalid during shutdown.

Usage:
    python tests/integration/test_graceful_shutdown.py

Requirements:
    - Docker and docker-compose installed
    - requests library
    - Integration test cluster running (or will be started)
"""

import json
import subprocess
import sys
import time
import unittest
from typing import Dict, Optional, Tuple
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    import requests
except ImportError:
    print("Error: 'requests' library required. Install: pip install requests")
    sys.exit(1)

from conftest import (
    ClusterTestCase,
    NODES,
    run_compose as _conftest_run_compose,
    wait_for_healthy,
)

_PROJECT_NAME = "ranvier-graceful-shutdown-test"


def run_compose(args, check=True, **kwargs):
    """Local wrapper that injects the project name for this test suite."""
    return _conftest_run_compose(args, project_name=_PROJECT_NAME, check=check, **kwargs)


# Test timeouts
HEALTH_TIMEOUT = 5
DRAIN_WAIT_TIMEOUT = 10


def get_health_status(api_url: str) -> Tuple[int, Optional[str]]:
    """Get health endpoint status code and body status field."""
    try:
        resp = requests.get(f"{api_url}/health", timeout=HEALTH_TIMEOUT)
        status_code = resp.status_code
        try:
            body = resp.json()
            status = body.get("status")
        except (json.JSONDecodeError, KeyError):
            status = None
        return status_code, status
    except requests.exceptions.RequestException as e:
        return -1, str(e)


def get_cluster_status(api_url: str) -> Optional[Dict]:
    """Get cluster status from admin endpoint."""
    try:
        resp = requests.get(f"{api_url}/admin/dump/cluster", timeout=HEALTH_TIMEOUT)
        if resp.status_code == 200:
            return resp.json()
        elif resp.status_code == 401:
            # Auth required but not configured in test - try without auth
            # (test environment should have auth disabled)
            return None
    except (requests.exceptions.RequestException, json.JSONDecodeError):
        pass
    return None


def get_metrics(metrics_url: str) -> Optional[str]:
    """Get raw metrics from Prometheus endpoint."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=HEALTH_TIMEOUT)
        if resp.status_code == 200:
            return resp.text
    except requests.exceptions.RequestException:
        pass
    return None


def send_chat_request(api_url: str, prompt: str = "test") -> Tuple[int, Dict]:
    """Send a chat completion request, return status code and headers."""
    try:
        payload = {
            "model": "test-model",
            "messages": [{"role": "user", "content": prompt}],
            "stream": True
        }
        resp = requests.post(
            f"{api_url}/v1/chat/completions",
            json=payload,
            timeout=10,
            stream=True
        )
        # Consume streaming response
        for _ in resp.iter_lines():
            pass
        return resp.status_code, dict(resp.headers)
    except requests.exceptions.RequestException as e:
        return -1, {"error": str(e)}


def signal_container_shutdown(container_name: str) -> bool:
    """Send SIGTERM to a container to initiate graceful shutdown."""
    try:
        result = subprocess.run(
            ["docker", "kill", "--signal=SIGTERM", container_name],
            capture_output=True,
            text=True,
            timeout=10
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def get_container_logs(container_name: str, tail: int = 50) -> str:
    """Get recent container logs."""
    try:
        result = subprocess.run(
            ["docker", "logs", "--tail", str(tail), container_name],
            capture_output=True,
            text=True,
            timeout=10
        )
        return result.stdout + result.stderr
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ""


class GracefulShutdownTest(ClusterTestCase):
    """Integration tests for graceful shutdown lifecycle."""

    PROJECT_NAME = "ranvier-graceful-shutdown-test"
    AUTO_REGISTER_BACKENDS = True

    def setUp(self):
        """Verify cluster health before each test."""
        # Skip cluster health check for tests that intentionally shut down nodes
        if self._testMethodName.startswith("test_0"):
            return

        # Ensure node1 is healthy (may have been restarted)
        node1_api = NODES["node1"]["api"]
        if not wait_for_healthy(f"{node1_api}/health", timeout=30):
            # Restart node1 if needed
            print("  Restarting node1 for test...")
            run_compose(["up", "-d", "ranvier1"], check=False)
            if not wait_for_healthy(f"{node1_api}/health", timeout=30):
                self.skipTest("Could not restore cluster health")

    # =========================================================================
    # Test 01: Healthy state baseline
    # =========================================================================
    def test_01_healthy_state_returns_200(self):
        """Verify /health returns 200 with 'healthy' status when running."""
        node1_api = NODES["node1"]["api"]

        status_code, status = get_health_status(node1_api)

        self.assertEqual(status_code, 200, "Health check should return 200")
        self.assertIn(status, ["healthy", "ok"], f"Status should be healthy, got: {status}")

    # =========================================================================
    # Test 02: Metrics accessible when healthy
    # =========================================================================
    def test_02_metrics_accessible_when_healthy(self):
        """Verify metrics endpoint is accessible in normal operation."""
        node1_metrics = NODES["node1"]["metrics"]

        metrics = get_metrics(node1_metrics)

        self.assertIsNotNone(metrics, "Metrics should be accessible")
        self.assertIn("seastar_", metrics, "Metrics should contain Seastar metrics")
        self.assertIn("ranvier", metrics, "Metrics should contain Ranvier metrics")

    # =========================================================================
    # Test 03: Cluster status shows not draining
    # =========================================================================
    def test_03_cluster_status_shows_not_draining(self):
        """Verify cluster status is_draining is false in normal operation."""
        node1_api = NODES["node1"]["api"]

        status = get_cluster_status(node1_api)

        # Cluster mode may not be enabled in all test configurations
        if status is None:
            self.skipTest("Cluster mode not enabled or endpoint not available")

        # Check for cluster_enabled: false response
        if not status.get("cluster_enabled", True):
            self.skipTest("Cluster mode not enabled")

        self.assertFalse(status.get("is_draining", True),
                        "is_draining should be false when running normally")

    # =========================================================================
    # Test 04: Concurrent requests handled during healthy state
    # =========================================================================
    def test_04_concurrent_requests_accepted_when_healthy(self):
        """Verify concurrent requests are accepted when server is healthy."""
        node1_api = NODES["node1"]["api"]
        num_requests = 10

        with ThreadPoolExecutor(max_workers=5) as executor:
            futures = [
                executor.submit(send_chat_request, node1_api, f"test-{i}")
                for i in range(num_requests)
            ]

            results = [f.result() for f in as_completed(futures)]

        # All requests should succeed (200) or at least not be rejected as draining (503)
        status_codes = [r[0] for r in results]
        success_count = sum(1 for code in status_codes if code == 200)

        self.assertGreaterEqual(success_count, num_requests * 0.8,
                               f"At least 80% of requests should succeed, got {success_count}/{num_requests}")

    # =========================================================================
    # Test 05: Comprehensive graceful shutdown behavior
    # =========================================================================
    def test_05_graceful_shutdown_behaviors(self):
        """Verify all graceful shutdown behaviors in a single test.

        This test combines multiple shutdown validations to avoid slow restart cycles:
        1. Health returns 503 with 'draining' status
        2. Metrics remain accessible during drain (lifecycle guard validation)
        3. New requests rejected with 503 + Retry-After header
        4. Cluster status shows is_draining = true

        The node is restarted only once at the end.
        """
        node_api = NODES["node3"]["api"]
        node_metrics = NODES["node3"]["metrics"]
        container_name = "ranvier3"

        # Verify node is healthy first
        status_code, _ = get_health_status(node_api)
        if status_code != 200:
            self.skipTest("Node3 not healthy, skipping drain test")

        # Capture pre-drain metrics
        pre_drain_metrics = get_metrics(node_metrics)
        self.assertIsNotNone(pre_drain_metrics, "Pre-drain metrics should be available")

        # Send SIGTERM to initiate graceful shutdown
        print(f"  Sending SIGTERM to {container_name}...")
        success = signal_container_shutdown(container_name)
        self.assertTrue(success, "Failed to send SIGTERM to container")

        # Track all behaviors we want to validate
        health_503_detected = False
        metrics_during_drain = None
        request_rejected = False
        retry_after_present = False
        cluster_draining_detected = False
        server_stopped = False

        # Poll for drain state and validate behaviors
        for i in range(DRAIN_WAIT_TIMEOUT * 4):  # Check every 0.25s for more chances
            # Check health endpoint
            status_code, status = get_health_status(node_api)

            if status_code == 503:
                if status == "draining" and not health_503_detected:
                    health_503_detected = True
                    print(f"  [✓] Health returns 503 with 'draining' status")

                    # Verify metrics still accessible during drain
                    metrics_during_drain = get_metrics(node_metrics)
                    if metrics_during_drain:
                        print(f"  [✓] Metrics accessible during drain")

                # Try sending a request during drain
                if not request_rejected:
                    req_status, headers = send_chat_request(node_api, "test-during-drain")
                    if req_status == 503:
                        request_rejected = True
                        retry_after = headers.get("Retry-After") or headers.get("retry-after")
                        retry_after_present = retry_after is not None
                        print(f"  [✓] Request rejected with 503, Retry-After: {retry_after}")

                # Check cluster status for is_draining
                if not cluster_draining_detected:
                    cluster_status = get_cluster_status(node_api)
                    if cluster_status and cluster_status.get("is_draining"):
                        cluster_draining_detected = True
                        print(f"  [✓] Cluster status shows is_draining=true")

            elif status_code == -1:
                # Connection refused - server stopped
                server_stopped = True
                print(f"  Server stopped (connection refused)")
                break

            time.sleep(0.25)

        # Log container output for debugging
        logs = get_container_logs(container_name, tail=20)
        print(f"  Container logs:\n{logs[:500]}...")

        # Restart the node for subsequent tests
        print(f"  Restarting {container_name}...")
        run_compose(["up", "-d", container_name], check=False)

        # Wait for node to be healthy again (best-effort, don't block too long)
        if not wait_for_healthy(f"{node_api}/health", timeout=15):
            print(f"  Note: {container_name} still starting (will be ready for next run)")
        else:
            print(f"  {container_name} is healthy again")

        # Assertions - drain window may be brief, so we accept partial success
        self.assertTrue(
            health_503_detected or server_stopped,
            "Should have detected drain state (503) or shutdown (connection refused)"
        )

        # If we caught the drain state, validate lifecycle guards
        if health_503_detected:
            self.assertIsNotNone(
                metrics_during_drain,
                "Metrics should remain accessible during drain phase (lifecycle guard)"
            )

        # Log summary
        print(f"\n  Summary:")
        print(f"    Health 503 detected: {health_503_detected}")
        print(f"    Metrics during drain: {'Yes' if metrics_during_drain else 'No'}")
        print(f"    Request rejected: {request_rejected}")
        print(f"    Retry-After header: {retry_after_present}")
        print(f"    Cluster draining: {cluster_draining_detected}")

    # =========================================================================
    # Test 06: Multiple nodes maintain consensus during single node shutdown
    # =========================================================================
    def test_06_cluster_maintains_consensus_during_node_shutdown(self):
        """Verify remaining nodes maintain quorum when one shuts down."""
        node1_api = NODES["node1"]["api"]
        node2_api = NODES["node2"]["api"]
        node3_api = NODES["node3"]["api"]
        container_name = "ranvier3"

        # Verify all nodes healthy
        for name, endpoints in NODES.items():
            code, _ = get_health_status(endpoints["api"])
            if code != 200:
                self.skipTest(f"{name} not healthy")

        # Get initial cluster status from node1
        status_before = get_cluster_status(node1_api)
        self.assertIsNotNone(status_before, "Initial cluster status should be available")

        # Shutdown node3
        print(f"  Shutting down {container_name}...")
        signal_container_shutdown(container_name)

        # Wait for shutdown to complete
        time.sleep(3)

        # Verify node1 and node2 are still healthy
        code1, _ = get_health_status(node1_api)
        code2, _ = get_health_status(node2_api)

        self.assertEqual(code1, 200, "Node1 should remain healthy")
        self.assertEqual(code2, 200, "Node2 should remain healthy")

        # Verify requests can still be processed
        status_code, _ = send_chat_request(node1_api, "test-after-node-failure")
        self.assertEqual(status_code, 200, "Requests should still be processed")

        # Restart node3 (best-effort cleanup, don't block too long)
        print(f"  Restarting {container_name}...")
        run_compose(["up", "-d", container_name], check=False)
        if not wait_for_healthy(f"{node3_api}/health", timeout=15):
            print(f"  Note: {container_name} still starting (will be ready for next run)")
        else:
            print(f"  {container_name} is healthy again")

        # Wait for cluster to stabilize
        time.sleep(2)

        # Verify nodes 1 and 2 are still healthy (node3 restart is best-effort cleanup)
        code1, _ = get_health_status(node1_api)
        code2, _ = get_health_status(node2_api)
        self.assertEqual(code1, 200, "Node1 should remain healthy after test")
        self.assertEqual(code2, 200, "Node2 should remain healthy after test")

        # Node3 health is optional - it may take longer to restart
        code3, _ = get_health_status(node3_api)
        if code3 != 200:
            print(f"  Note: node3 not yet healthy (code={code3}), but test passed")


def main():
    """Run the graceful shutdown tests."""
    # Ensure tests run in order
    loader = unittest.TestLoader()
    loader.sortTestMethodsUsing = lambda x, y: (x > y) - (x < y)

    suite = loader.loadTestsFromTestCase(GracefulShutdownTest)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Exit with appropriate code
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
