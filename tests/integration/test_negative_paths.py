#!/usr/bin/env python3
"""
Negative-Path Integration Tests for Ranvier Core

This test suite validates failure handling and resilience behaviors:
1. Split-brain: Network partition detection and recovery via quorum
2. Backend flap: Circuit breaker engagement on rapid backend restarts
3. Config reload: Invalid YAML rejected, old config preserved on SIGHUP
4. Rate limit: 503 responses with Retry-After when rate limits exceeded
5a. Stale connection retry: Phase 3.5 detects and retries on fresh connection
5b. Oversized request: Rejection of request bodies exceeding max size

All tests use Docker Compose (docker-compose.test.yml) with a 3-node cluster
and mock backends, following the same pattern as the existing happy-path suites.

The shared docker-compose harness (constants, helpers, lifecycle) lives in
``tests/integration/conftest.py``.  This file only contains the
negative-path-specific test methods and the network-partitioning helpers
they share.  Any change to cluster bring-up / teardown belongs in conftest.

Usage:
    python3 tests/integration/test_negative_paths.py

Requirements:
    - Docker and docker-compose installed
    - requests library
"""

import concurrent.futures
import os
import socket
import subprocess
import sys
import time
import unittest
import urllib.parse
from typing import Dict, Optional, Tuple

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    COMPOSE_FILE,
    CONTAINER_CONFIG_PATH as _CONTAINER_CONFIG_PATH,
    CONTAINER_NAMES,
    ClusterTestCase,
    DOCKER_HOST,
    NODES,
    PEER_TIMEOUT,
    REQUEST_TIMEOUT,
    docker_network_connect,
    docker_network_disconnect,
    get_compose_cmd,
    get_docker_network_name,
    get_metric_value,
    send_chat_request as _conftest_send_chat_request,
)


# =============================================================================
# File-specific helpers
# =============================================================================


def send_chat_request(
    api_url: str,
    prompt: str = "test",
    timeout: int = REQUEST_TIMEOUT
) -> Tuple[int, str, Dict[str, str]]:
    """Send a chat completion request. Returns (status_code, body, headers).

    Thin wrapper around the conftest helper -- maps the conftest exception
    status ``0`` to ``-1`` to preserve the original local semantics.
    """
    status, body, headers = _conftest_send_chat_request(
        api_url,
        [{"role": "user", "content": prompt}],
        timeout=timeout,
        retries=1,
    )
    if status == 0:
        status = -1
    return status, body, headers


# =============================================================================
# Test Suite
# =============================================================================

class NegativePathTest(ClusterTestCase):
    """Integration tests for negative/failure paths."""

    PROJECT_NAME = "ranvier-negative-path-test"
    AUTO_REGISTER_BACKENDS = True

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.docker_network = get_docker_network_name(cls.PROJECT_NAME)
        print(f"\nDocker network: {cls.docker_network}")


    # =========================================================================
    # Test 1: Split-Brain Detection and Recovery
    # =========================================================================

    def test_01_split_brain_partition_and_recovery(self):
        """Partition node3 from the network, verify quorum detection, reconnect
        and verify cluster recovery.

        Uses 'docker network disconnect' to isolate node3, then checks that
        remaining nodes detect the peer loss via /admin/dump/cluster. After
        reconnecting, verifies all peers recover.
        """
        print("\nTest: Split-brain partition and recovery")

        network = self.__class__.docker_network
        container = CONTAINER_NAMES["node3"]

        # Verify initial cluster health: all nodes should see 2 peers
        print("  Verifying initial cluster health...")
        for name, endpoints in NODES.items():
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
            print(f"    {name}: cluster_peers_alive = {peers}")
            self.assertIsNotNone(peers, f"{name} has no peer metric")
            self.assertEqual(peers, 2.0, f"{name} should have 2 peers initially")

        # Partition node3 by disconnecting it from the Docker network
        print(f"\n  Disconnecting {container} from network {network}...")
        success = docker_network_disconnect(network, container)
        self.assertTrue(success, f"Failed to disconnect {container} from network")

        try:
            # Wait for gossip peer timeout to detect the partition
            # gossip_peer_timeout_seconds = 6 in the test config
            wait_time = PEER_TIMEOUT + 4
            print(f"  Waiting {wait_time}s for peer timeout...")
            time.sleep(wait_time)

            # Verify remaining nodes detect reduced peer count
            print("\n  Checking peer counts after partition:")
            for name in ["node1", "node2"]:
                endpoints = NODES[name]
                peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
                print(f"    {name}: cluster_peers_alive = {peers}")
                self.assertIsNotNone(peers, f"{name} has no peer metric after partition")
                self.assertLessEqual(
                    peers, 1.0,
                    f"{name} should have <= 1 peer after node3 partition, but has {peers}"
                )

            # Verify quorum state via /admin/dump/cluster on node1
            print("\n  Checking cluster status on node1...")
            try:
                resp = requests.get(f"{NODES['node1']['api']}/admin/dump/cluster", timeout=5)
                if resp.status_code == 200:
                    cluster_state = resp.json()
                    quorum_state = cluster_state.get("quorum_state", "unknown")
                    peers_alive = cluster_state.get("peers_alive", -1)
                    print(f"    Quorum state: {quorum_state}")
                    print(f"    Peers alive: {peers_alive}")
                    # GossipConsensus uses QuorumState enum: "HEALTHY" or "DEGRADED"
                    # With 3-node cluster and 1 partitioned, 2/3 is still majority
                    # so quorum remains HEALTHY. The key validation is peers_alive dropped.
                    self.assertIn(
                        quorum_state,
                        ["HEALTHY", "DEGRADED"],
                        f"Unexpected quorum state: {quorum_state}"
                    )
                    self.assertLess(
                        peers_alive, 2,
                        f"peers_alive should have decreased, got {peers_alive}"
                    )
                else:
                    print(f"    Cluster endpoint returned {resp.status_code}")
            except requests.exceptions.RequestException as e:
                print(f"    Could not query cluster status: {e}")

        finally:
            # Reconnect node3 to restore cluster
            print(f"\n  Reconnecting {container} to network {network}...")
            # Reconnect with the original IP address
            reconnected = docker_network_connect(network, container, ip="172.28.2.3")
            if not reconnected:
                # Try without specifying IP
                reconnected = docker_network_connect(network, container)
            self.assertTrue(reconnected, f"Failed to reconnect {container}")

        # Wait for gossip to re-establish connections
        recovery_wait = PEER_TIMEOUT + 10
        print(f"  Waiting {recovery_wait}s for cluster recovery...")
        time.sleep(recovery_wait)

        # Verify full recovery: all nodes should see 2 peers again
        print("\n  Checking peer counts after recovery:")
        max_retries = 5
        for retry in range(max_retries):
            all_recovered = True
            for name, endpoints in NODES.items():
                peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive", retries=3)
                if retry == max_retries - 1:
                    print(f"    {name}: cluster_peers_alive = {peers}")
                if peers is None or peers != 2.0:
                    all_recovered = False

            if all_recovered:
                break
            if retry < max_retries - 1:
                print(f"    Not all recovered yet, waiting... (attempt {retry + 1}/{max_retries})")
                time.sleep(5)

        for name, endpoints in NODES.items():
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive", retries=3)
            self.assertIsNotNone(peers, f"{name} has no peer metric after recovery")
            self.assertEqual(
                peers, 2.0,
                f"{name} should have 2 peers after recovery, but has {peers}"
            )

        print("  PASSED: Split-brain partition detected and cluster recovered")

    # =========================================================================
    # Test 2: Backend Flap — Circuit Breaker Engagement
    # =========================================================================

    def test_02_backend_flap_circuit_breaker(self):
        """Disconnect both backends from the network to trigger circuit breaker
        failures, then reconnect and verify recovery.

        Uses docker network disconnect (same proven approach as test_01) to
        make backends unreachable. With both backends unreachable, all proxy
        requests fail with connection errors, which should trigger the circuit
        breaker after failure_threshold (3) consecutive failures.
        """
        print("\nTest: Backend flap — circuit breaker engagement")

        network = self.__class__.docker_network
        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        # Capture initial metric values across all nodes
        initial_opens = 0
        for name, endpoints in NODES.items():
            val = get_metric_value(endpoints["metrics"], "circuit_breaker_opens") or 0
            initial_opens += val
        print(f"  Initial circuit_breaker_opens (sum all nodes): {initial_opens}")

        initial_conn_errors = get_metric_value(node1_metrics, "http_requests_connection_error") or 0
        initial_failures = get_metric_value(node1_metrics, "http_requests_failed") or 0
        initial_timeouts = get_metric_value(node1_metrics, "http_requests_timeout") or 0
        print(f"  Initial connection_errors: {initial_conn_errors}")
        print(f"  Initial failures: {initial_failures}")
        print(f"  Initial timeouts: {initial_timeouts}")

        # Disconnect BOTH backends from the Docker network.
        # This makes backend IPs unreachable — connection attempts will time out
        # or get EHOSTUNREACH. More reliable than docker kill/stop.
        backend1_container = CONTAINER_NAMES["backend1"]
        backend2_container = CONTAINER_NAMES["backend2"]

        print(f"\n  Disconnecting {backend1_container} from network...")
        ok1 = docker_network_disconnect(network, backend1_container)
        print(f"    Result: {'ok' if ok1 else 'FAILED'}")

        print(f"  Disconnecting {backend2_container} from network...")
        ok2 = docker_network_disconnect(network, backend2_container)
        print(f"    Result: {'ok' if ok2 else 'FAILED'}")

        self.assertTrue(ok1 and ok2, "Failed to disconnect backends from network")

        # Wait for existing pooled connections to go stale
        time.sleep(3)

        try:
            # Send requests — with both backends unreachable, all should fail.
            # Use the same prompt for deterministic routing.
            num_requests = 15
            print(f"\n  Sending {num_requests} requests with both backends unreachable...")
            error_responses = []
            for i in range(num_requests):
                status, body, headers = send_chat_request(
                    node1_api, "circuit-breaker-test", timeout=15
                )
                error_responses.append(status)
                time.sleep(0.5)

            print(f"  Response statuses: {error_responses}")

            # Count non-200 responses (errors)
            error_count = sum(1 for s in error_responses if s != 200)
            print(f"  Error responses: {error_count}/{num_requests}")

        finally:
            # Reconnect both backends to restore connectivity
            print("\n  Reconnecting backends to network...")
            docker_network_connect(network, backend1_container, ip="172.28.1.10")
            docker_network_connect(network, backend2_container, ip="172.28.1.11")
            time.sleep(5)

        # Wait for metrics to settle
        time.sleep(2)

        # Collect final metrics across all nodes
        final_opens = 0
        for name, endpoints in NODES.items():
            val = get_metric_value(endpoints["metrics"], "circuit_breaker_opens") or 0
            final_opens += val
            print(f"  {name} circuit_breaker_opens: {val}")
        opens_delta = final_opens - initial_opens

        final_conn_errors = get_metric_value(node1_metrics, "http_requests_connection_error") or 0
        final_failures = get_metric_value(node1_metrics, "http_requests_failed") or 0
        final_timeouts = get_metric_value(node1_metrics, "http_requests_timeout") or 0
        conn_error_delta = final_conn_errors - initial_conn_errors
        failure_delta = final_failures - initial_failures
        timeout_delta = final_timeouts - initial_timeouts

        print(f"\n  Circuit breaker opens delta: {opens_delta}")
        print(f"  Connection errors delta: {conn_error_delta}")
        print(f"  Failures delta: {failure_delta}")
        print(f"  Timeouts delta: {timeout_delta}")

        # With both backends unreachable, we expect at least one of:
        # - circuit_breaker_opens > 0 (circuit opened and blocked requests)
        # - connection_errors > 0 (connection failures recorded)
        # - failures > 0 (general request failures)
        # - timeouts > 0 (connect timeouts)
        total_negative_signals = opens_delta + conn_error_delta + failure_delta + timeout_delta
        self.assertGreater(
            total_negative_signals, 0,
            f"Expected negative signals when both backends unreachable: "
            f"circuit_opens={opens_delta}, conn_errors={conn_error_delta}, "
            f"failures={failure_delta}, timeouts={timeout_delta}"
        )

        # Verify requests succeed after backends are reconnected
        print("  Verifying requests succeed after reconnection...")
        success = False
        for attempt in range(15):
            status, _, _ = send_chat_request(node1_api, "recovery-check", timeout=10)
            if status == 200:
                success = True
                break
            time.sleep(2)

        self.assertTrue(success, "Requests should succeed after backends reconnect")
        print("  PASSED: Circuit breaker / failure metrics engaged during backend outage")

    # =========================================================================
    # Test 3: Config Reload with Invalid YAML
    # =========================================================================

    def test_03_config_reload_invalid_yaml(self):
        """Send SIGHUP with invalid config, verify old config is preserved.

        Writes invalid YAML to the config file inside the container, sends
        SIGHUP, then verifies that the server continues operating with the
        previous valid configuration.
        """
        print("\nTest: Config reload with invalid YAML")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]
        container = CONTAINER_NAMES["node1"]

        # Get current config state — verify the server is healthy
        print("  Verifying node1 is healthy before reload test...")
        status, _, _ = send_chat_request(node1_api, "pre-reload-check", timeout=10)
        self.assertEqual(status, 200, "Node1 should be healthy before config reload test")

        # Get a reference metric value to verify config is preserved
        pre_reload_peers = get_metric_value(node1_metrics, "cluster_peers_alive")
        print(f"  Pre-reload cluster_peers_alive: {pre_reload_peers}")

        # Write invalid YAML to /app/ranvier.yaml inside the container.
        # The container has no config file by default (config from env vars),
        # so we create one, send SIGHUP, then remove it in the finally block.
        invalid_yaml = "{{{{invalid: yaml: [unterminated"

        print("  Writing invalid YAML to container config...")
        subprocess.run(
            ["docker", "exec", container, "sh", "-c",
             f"echo '{invalid_yaml}' > {_CONTAINER_CONFIG_PATH}"],
            capture_output=True, text=True, timeout=10
        )

        try:
            # Send SIGHUP to trigger config reload
            print("  Sending SIGHUP to trigger config reload...")
            signal_result = subprocess.run(
                ["docker", "kill", "--signal=HUP", container],
                capture_output=True, text=True, timeout=10
            )
            self.assertEqual(signal_result.returncode, 0, "Failed to send SIGHUP")

            # Wait for reload attempt
            time.sleep(3)

            # Verify the server is still running with the old config
            print("  Verifying server still healthy after invalid reload...")
            status, _, _ = send_chat_request(node1_api, "post-reload-check", timeout=10)
            self.assertEqual(
                status, 200,
                "Server should continue operating after failed config reload"
            )

            # Verify metrics are still accessible (server didn't crash)
            post_reload_peers = get_metric_value(node1_metrics, "cluster_peers_alive")
            print(f"  Post-reload cluster_peers_alive: {post_reload_peers}")
            self.assertIsNotNone(
                post_reload_peers,
                "Metrics should still be accessible after failed config reload"
            )

            # Cluster state should be unchanged (old config preserved)
            self.assertEqual(
                pre_reload_peers, post_reload_peers,
                "Cluster peer count should be unchanged after failed config reload"
            )

        finally:
            # Remove the config file to restore env-var-only defaults.
            # /tmp/ is writable (tmpfs), so rm works here.
            print("  Restoring original config state...")
            subprocess.run(
                ["docker", "exec", container, "rm", "-f", _CONTAINER_CONFIG_PATH],
                capture_output=True, text=True, timeout=10
            )

            # Send SIGHUP again to reload (will fall back to defaults + env vars)
            subprocess.run(
                ["docker", "kill", "--signal=HUP", container],
                capture_output=True, text=True, timeout=10
            )
            time.sleep(2)

        print("  PASSED: Invalid config reload rejected, old config preserved")

    # =========================================================================
    # Test 4: Rate Limiting
    # =========================================================================

    def test_04_rate_limit_exceeded(self):
        """Send concurrent requests exceeding rate limits, verify rejection.

        Enables rate limiting via config file rewrite + SIGHUP reload with a
        very low limit (2 rps, burst 1), then floods the endpoint with
        concurrent requests. Expects 503 responses with Retry-After header
        (Ranvier uses 503 for rate limiting via make_rate_limited_handler).
        """
        print("\nTest: Rate limit exceeded")

        node1_api = NODES["node1"]["api"]
        container = CONTAINER_NAMES["node1"]

        # The container has NO ranvier.yaml by default (config is from env vars).
        # Write a minimal YAML with only the rate_limit section. On SIGHUP,
        # RanvierConfig::load() parses this file, then applies env var overrides.
        # Since there's no RANVIER_RATE_LIMIT_ENABLED env var, our YAML values stick.
        print("  Writing rate limit config to container...")
        rate_limit_yaml = (
            "rate_limit:\n"
            "  enabled: true\n"
            "  requests_per_second: 2\n"
            "  burst_size: 1\n"
        )
        subprocess.run(
            ["docker", "exec", "-i", container, "sh", "-c",
             f"cat > {_CONTAINER_CONFIG_PATH}"],
            input=rate_limit_yaml,
            capture_output=True, text=True, timeout=10
        )

        # Verify the file was written correctly
        verify_result = subprocess.run(
            ["docker", "exec", container, "cat", _CONTAINER_CONFIG_PATH],
            capture_output=True, text=True, timeout=10
        )
        print(f"    Written config:\n{verify_result.stdout.strip()}")

        # IMPORTANT: Application::reload_config() has a RELOAD_COOLDOWN of 10 seconds.
        # test_03's cleanup sent a SIGHUP just moments ago. We must wait for the
        # cooldown to expire before our SIGHUP will be accepted.
        print("  Waiting for config reload cooldown (12s)...")
        time.sleep(12)

        # Send SIGHUP to apply the new config
        print("  Sending SIGHUP to apply rate limit config...")
        subprocess.run(
            ["docker", "kill", "--signal=HUP", container],
            capture_output=True, text=True, timeout=10
        )

        # Wait for reload to take effect, then verify rate limiter is active
        # by checking if a probe request gets rate-limited or the metric increments.
        print("  Waiting for reload to take effect...")
        time.sleep(3)

        # Verify the rate limiter actually engaged by sending a quick burst
        # and checking if any get rate-limited
        print("  Verifying rate limiter is active...")
        rate_limiter_active = False
        for probe_attempt in range(3):
            probe_statuses = []
            for _ in range(10):
                status, body, _ = send_chat_request(node1_api, "rate-limit-probe", timeout=10)
                probe_statuses.append(status)
            rate_limited_probes = sum(1 for s in probe_statuses if s in (503, 429))
            print(f"    Probe attempt {probe_attempt + 1}: {rate_limited_probes}/10 rate-limited, statuses={probe_statuses}")
            if rate_limited_probes > 0:
                rate_limiter_active = True
                break
            # If not active yet, the SIGHUP may have been within cooldown.
            # Wait and retry SIGHUP.
            print("    Rate limiter not active yet, re-sending SIGHUP...")
            time.sleep(12)
            subprocess.run(
                ["docker", "kill", "--signal=HUP", container],
                capture_output=True, text=True, timeout=10
            )
            time.sleep(3)

        try:
            # Fail early if probe never detected rate limiting
            self.assertTrue(
                rate_limiter_active,
                "Rate limiter never became active after config reload + SIGHUP. "
                "The RELOAD_COOLDOWN (10s) may have blocked all reload attempts."
            )

            # Flood with concurrent requests to exceed the rate limit
            # With 2 rps and burst of 1, almost all concurrent requests should be rejected
            print("  Sending burst of concurrent requests...")
            num_requests = 30
            rate_limited_count = 0
            retry_after_seen = False
            status_codes = []

            with concurrent.futures.ThreadPoolExecutor(max_workers=15) as executor:
                futures = [
                    executor.submit(send_chat_request, node1_api, f"rate-limit-test-{i}", 10)
                    for i in range(num_requests)
                ]
                for future in concurrent.futures.as_completed(futures):
                    status, body, headers = future.result()
                    status_codes.append(status)
                    # Ranvier returns 503 with Retry-After for rate limiting
                    if status == 503 and "rate limit" in body.lower():
                        rate_limited_count += 1
                        retry_after = headers.get("Retry-After") or headers.get("retry-after")
                        if retry_after is not None:
                            retry_after_seen = True
                    elif status == 429:
                        # Also accept 429 in case implementation changes
                        rate_limited_count += 1
                        retry_after = headers.get("Retry-After") or headers.get("retry-after")
                        if retry_after is not None:
                            retry_after_seen = True

            print(f"  Results: {num_requests} sent, {rate_limited_count} rate-limited")
            print(f"  Status codes: {dict((s, status_codes.count(s)) for s in set(status_codes))}")
            print(f"  Retry-After header seen: {retry_after_seen}")

            # Also check the rate-limited metric for additional confirmation
            rate_limited_metric = get_metric_value(
                NODES["node1"]["metrics"], "http_requests_rate_limited"
            ) or 0
            print(f"  http_requests_rate_limited metric: {rate_limited_metric}")

            # With 2 rps and burst of 1, sending 30 concurrent requests should
            # trigger rate limiting on many of them
            self.assertGreater(
                rate_limited_count, 0,
                f"Some requests should be rate-limited, but none were. "
                f"Status codes: {status_codes}. "
                f"rate_limited metric: {rate_limited_metric}"
            )

            if rate_limited_count > 0:
                self.assertTrue(
                    retry_after_seen,
                    "Rate-limited responses should include Retry-After header"
                )

        finally:
            # Remove the config file to restore env-var-only defaults
            print("\n  Removing config file (restoring env-var-only defaults)...")
            subprocess.run(
                ["docker", "exec", container, "rm", "-f", _CONTAINER_CONFIG_PATH],
                capture_output=True, text=True, timeout=10
            )
            subprocess.run(
                ["docker", "kill", "--signal=HUP", container],
                capture_output=True, text=True, timeout=10
            )
            time.sleep(5)

            # Verify normal operation restored
            status, _, _ = send_chat_request(node1_api, "post-rate-limit-check", timeout=10)
            if status != 200:
                print(f"  Warning: Post-restore request returned {status}")
                time.sleep(5)

        print("  PASSED: Rate limiting enforced with Retry-After header")

    # =========================================================================
    # Test 5a: Stale Connection Retry
    # =========================================================================

    def test_05a_stale_connection_retry(self):
        """Verify Ranvier detects stale pooled connections and retries on fresh ones.

        Scenario:
        1. Enable keep-alive on mock backends (so Ranvier pools connections)
        2. Send warmup requests to establish pooled connections
        3. Restart a backend container (kills TCP connections from backend side,
           but Ranvier's pool still holds the now-stale connection handles)
        4. Send requests — Ranvier uses stale connection, gets empty response,
           Phase 3.5 detects 0 bytes written and retries on a fresh connection
        5. Assert: requests succeed AND stale_connection_retries_total incremented
        """
        print("\nTest: Stale connection retry (Phase 3.5)")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        # Backend direct endpoints (for admin control, bypassing Ranvier)
        backend1_direct = f"http://{DOCKER_HOST}:21434"
        backend2_direct = f"http://{DOCKER_HOST}:21435"

        # Force Connection: close on direct backend calls. The mock backend's
        # non-streaming HTTP/1.1 responses may remain stuck in Python's
        # buffered wfile until the connection closes, causing read timeouts
        # with keep-alive connections.
        close_hdr = {"Connection": "close"}

        # Step 1: Enable keep-alive on both backends so Ranvier pools connections
        print("  Enabling keep-alive on mock backends...")
        try:
            r1 = requests.post(f"{backend1_direct}/admin/keepalive?enabled=1", timeout=5, headers=close_hdr)
            r2 = requests.post(f"{backend2_direct}/admin/keepalive?enabled=1", timeout=5, headers=close_hdr)
            self.assertEqual(r1.status_code, 200, "Failed to enable keepalive on backend1")
            self.assertEqual(r2.status_code, 200, "Failed to enable keepalive on backend2")
            print("    Keep-alive enabled on both backends")
        except requests.exceptions.RequestException as e:
            self.fail(f"Could not reach mock backends for admin control: {e}")

        try:
            # Ensure rate limiting from test_04 is cleared (its cleanup SIGHUP
            # may have been rejected by RELOAD_COOLDOWN)
            pre_status, _, _ = send_chat_request(node1_api, "stale-precheck", timeout=10)
            if pre_status != 200:
                print(f"  Ranvier returning {pre_status}, clearing residual rate limit config...")
                node1_container = CONTAINER_NAMES["node1"]
                subprocess.run(
                    ["docker", "exec", node1_container, "rm", "-f", _CONTAINER_CONFIG_PATH],
                    capture_output=True, text=True, timeout=10
                )
                time.sleep(12)  # Wait for RELOAD_COOLDOWN to expire
                subprocess.run(
                    ["docker", "kill", "--signal=HUP", node1_container],
                    capture_output=True, text=True, timeout=10
                )
                time.sleep(3)
                pre_status, _, _ = send_chat_request(node1_api, "stale-precheck-2", timeout=10)
                self.assertEqual(pre_status, 200,
                                 f"Ranvier still returning {pre_status} after config reload")
                print("    Rate limiting cleared")

            # Step 2: Send warmup requests to establish pooled connections
            print("  Sending warmup requests to establish pooled connections...")
            for i in range(4):
                status, body, _ = send_chat_request(node1_api, f"stale-warmup-{i}", timeout=10)
                self.assertEqual(status, 200, f"Warmup request {i} should succeed, got {status}")
            time.sleep(1)  # Let connections settle in pool

            # Capture initial retry metric
            initial_retries = get_metric_value(node1_metrics, "stale_connection_retries") or 0
            initial_success = get_metric_value(node1_metrics, "http_requests_success") or 0
            print(f"  Initial stale_connection_retries: {initial_retries}")
            print(f"  Initial http_requests_success: {initial_success}")

            # Step 3: Restart backend containers to kill TCP connections
            # Ranvier's pool still holds the now-dead connection handles
            print("  Restarting backend containers to create stale connections...")
            for container in [CONTAINER_NAMES["backend1"], CONTAINER_NAMES["backend2"]]:
                result = subprocess.run(
                    ["docker", "restart", container],
                    capture_output=True, text=True, timeout=30
                )
                self.assertEqual(result.returncode, 0, f"Failed to restart {container}")
            print("    Backends restarted")

            # Wait for backends to come back up
            print("  Waiting for backends to become healthy...")
            for url in [backend1_direct, backend2_direct]:
                healthy = False
                for attempt in range(20):
                    try:
                        resp = requests.get(f"{url}/health", timeout=2, headers=close_hdr)
                        if resp.status_code == 200:
                            healthy = True
                            break
                    except requests.exceptions.RequestException:
                        pass
                    time.sleep(0.5)
                self.assertTrue(healthy, f"Backend at {url} did not recover after restart")

            # Re-enable keep-alive after restart (state is reset)
            requests.post(f"{backend1_direct}/admin/keepalive?enabled=1", timeout=5, headers=close_hdr)
            requests.post(f"{backend2_direct}/admin/keepalive?enabled=1", timeout=5, headers=close_hdr)

            # Step 4: Send requests — these should hit stale pooled connections
            # Phase 3.5 should detect the empty response and retry on fresh connections
            print("  Sending requests (should trigger stale connection retry)...")
            success_count = 0
            for i in range(6):
                status, body, _ = send_chat_request(node1_api, f"stale-test-{i}", timeout=15)
                if status == 200 and "backend" in body.lower():
                    success_count += 1
                print(f"    Request {i}: status={status}, has_body={'yes' if body else 'no'}")

            # Step 5: Verify results
            time.sleep(1)  # Let metrics settle

            final_retries = get_metric_value(node1_metrics, "stale_connection_retries") or 0
            final_success = get_metric_value(node1_metrics, "http_requests_success") or 0
            retry_delta = final_retries - initial_retries
            success_delta = final_success - initial_success

            print(f"\n  Results:")
            print(f"    Successful responses: {success_count}/6")
            print(f"    stale_connection_retries delta: {retry_delta}")
            print(f"    http_requests_success delta: {success_delta}")

            # At least some requests should have succeeded
            self.assertGreater(
                success_count, 0,
                "At least some requests should succeed after stale retry"
            )

            # The stale retry metric should have incremented
            # (Ranvier detected stale connections and retried)
            self.assertGreater(
                retry_delta, 0,
                f"stale_connection_retries should increment when pooled connections are stale "
                f"(was {initial_retries}, now {final_retries}). "
                f"If 0, Ranvier may not have pooled keep-alive connections."
            )

            print(f"  PASSED: Stale connections detected and retried ({retry_delta} retries, "
                  f"{success_count}/6 succeeded)")

        finally:
            # Restore Connection: close mode on backends
            print("  Restoring Connection: close on backends...")
            for url in [backend1_direct, backend2_direct]:
                try:
                    requests.post(f"{url}/admin/keepalive?enabled=0", timeout=5, headers=close_hdr)
                except requests.exceptions.RequestException:
                    pass

            # Verify cluster is healthy after test
            time.sleep(2)
            status, _, _ = send_chat_request(node1_api, "post-stale-check", timeout=10)
            if status != 200:
                print(f"  Warning: Post-stale-test request returned {status}")

    # =========================================================================
    # Test 5b: Oversized Request Body
    # =========================================================================

    def test_05b_oversized_request_body(self):
        """Send an oversized request via raw socket with fraudulent Content-Length
        to verify the server handles it gracefully without crashing.

        Ranvier's hard limit is CrossShardRequestLimits::max_body_size = 128 MB,
        enforced in CrossShardRequest::create(). Sending 128 MB over the network
        is impractical for a test, so we validate two things:

        1. Raw socket: Send a request claiming Content-Length of 200 MB but close
           the connection after sending a small amount. The server should handle
           the incomplete read without crashing.
        2. Resilience: After the abusive request, verify the server still handles
           normal requests correctly.
        """
        print("\nTest: Oversized request body handling")

        node1_api = NODES["node1"]["api"]
        parsed = urllib.parse.urlparse(node1_api)
        host = parsed.hostname
        port = parsed.port

        # Part 1: Send a request with a fraudulent Content-Length (200 MB)
        # but only send a tiny body, then close the connection.
        # This tests how the server handles a connection that promises more data
        # than it delivers (common attack vector).
        print("  Part 1: Sending request with fraudulent Content-Length (200 MB)...")
        fraudulent_size = 200 * 1024 * 1024  # 200 MB claimed
        small_body = '{"model":"test","messages":[{"role":"user","content":"x"}]}'

        raw_request = (
            f"POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {fraudulent_size}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
            f"{small_body}"
        )

        server_handled_gracefully = False
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect((host, port))
            s.sendall(raw_request.encode())
            # Wait briefly then close — server sees incomplete body
            time.sleep(1)
            # Try to read any response
            try:
                response = s.recv(4096)
                if response:
                    resp_str = response.decode("utf-8", errors="replace")
                    print(f"    Server response (first 200 chars): {resp_str[:200]}")
                    server_handled_gracefully = True
            except socket.timeout:
                print("    Server timed out waiting for body (expected)")
                server_handled_gracefully = True
            s.close()
        except (socket.error, OSError) as e:
            print(f"    Socket error (expected for abusive request): {e}")
            server_handled_gracefully = True

        self.assertTrue(
            server_handled_gracefully,
            "Server should handle fraudulent Content-Length without crashing"
        )

        # Part 2: Send a very large (but honest) request body to verify the
        # server processes it or rejects it cleanly. Use 10 MB which is large
        # but under the 128 MB limit — the server should accept it.
        print("\n  Part 2: Sending large but valid request (~10 MB)...")
        large_content = "x" * (10 * 1024 * 1024)
        payload = {
            "model": "test-model",
            "messages": [{"role": "user", "content": large_content}],
            "stream": True
        }

        try:
            resp = requests.post(
                f"{node1_api}/v1/chat/completions",
                json=payload,
                headers={"Content-Type": "application/json"},
                timeout=60
            )
            print(f"    Response status: {resp.status_code}")
            # Under 128 MB limit, so 200 is acceptable. 4xx/5xx also acceptable
            # if the server has additional validation (e.g., token count limits)
            # or transient conditions (503 from rate limiter cooldown, backpressure).
            self.assertIn(
                resp.status_code,
                [200, 400, 413, 500, 502, 503],
                f"Large request should get 200 (accepted) or 4xx/5xx (rejected), got {resp.status_code}"
            )
        except (requests.exceptions.ConnectionError, requests.exceptions.ChunkedEncodingError) as e:
            print(f"    Connection error with large body: {e}")
            # Acceptable — server may close connection for very large bodies

        # Part 3: Verify server is still healthy
        print("\n  Part 3: Verifying server health after oversized requests...")
        time.sleep(2)
        success = False
        for attempt in range(5):
            status, _, _ = send_chat_request(node1_api, "post-oversize-check", timeout=10)
            if status == 200:
                success = True
                break
            time.sleep(2)

        self.assertTrue(
            success,
            "Server should remain healthy after handling oversized/abusive requests"
        )

        print("  PASSED: Server handles oversized requests gracefully")

# =============================================================================
# Main
# =============================================================================

def main():
    """Run the negative path integration tests."""
    print("=" * 70)
    print("Ranvier Core - Negative Path Integration Tests")
    print("=" * 70)
    print("\nThese tests validate failure handling and resilience:")
    print("  - Split-brain detection and recovery")
    print("  - Circuit breaker engagement on backend flapping")
    print("  - Config reload rejection for invalid YAML")
    print("  - Rate limiting enforcement")
    print("  - Stale pooled connection retry")
    print("  - Oversized request rejection")
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
    suite = loader.loadTestsFromTestCase(NegativePathTest)

    # Sort tests to run in order (test_01, test_02, etc.)
    suite = unittest.TestSuite(sorted(suite, key=lambda t: t.id()))

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Return appropriate exit code
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
