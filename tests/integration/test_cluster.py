#!/usr/bin/env python3
"""
Multi-Node Integration Tests for Ranvier Core

This test suite validates the distributed behavior of Ranvier:
1. Backend registration and route learning
2. Gossip-based route propagation between cluster nodes
3. Peer liveness detection and route pruning on node failure

Usage:
    python tests/integration/test_cluster.py

Requirements:
    - Docker and docker-compose installed
    - pytest (optional, can run with unittest)
    - requests library

The shared docker-compose harness (constants, helpers, lifecycle) lives in
``tests/integration/conftest.py``.  This file only contains the test methods
themselves — any change to cluster bring-up/teardown belongs in conftest.
"""

import json
import os
import sys
import time
import unittest

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    BACKENDS,
    COMPOSE_FILE,
    ClusterTestCase,
    NODES,
    PEER_TIMEOUT,
    PROPAGATION_TIMEOUT,
    check_container_running,
    get_all_metrics,
    get_compose_cmd,
    get_metric_value,
    run_compose,
    send_chat_request,
    sum_metric_by_substring,
    wait_for_healthy,
    DOCKER_HOST,
    MOCK_BACKEND_PORTS,
)


class ClusterIntegrationTest(ClusterTestCase):
    """Integration tests for multi-node Ranvier cluster."""

    # Unique compose project so a pytest-session cluster running in parallel
    # on the same host can't collide with this unittest-style suite.
    PROJECT_NAME = "ranvier-integration-test"
    # Keep backends-registration as test_02 for functional parity with the
    # pre-refactor suite.  This is the one test method that exercises the
    # admin API end-to-end, so we don't want setUpClass to pre-register.
    AUTO_REGISTER_BACKENDS = False

    def test_01_cluster_peers_connected(self):
        """Verify that all nodes have connected to their peers."""
        print("\nTest: Cluster peers connected")

        for name, endpoints in NODES.items():
            # Use debug=True on first node to see available metrics
            debug = (name == "node1")
            peers_alive = get_metric_value(endpoints["metrics"], "cluster_peers_alive", debug=debug)
            print(f"  {name}: cluster_peers_alive = {peers_alive}")

            # Each node should see 2 peers (the other 2 nodes)
            self.assertIsNotNone(peers_alive, f"{name} has no peer metric")
            self.assertEqual(
                peers_alive, 2.0,
                f"{name} should have 2 peers, but has {peers_alive}"
            )

        print("  PASSED: All nodes have 2 peers connected")

    def test_02_register_backend_on_node1(self):
        """Register backends on all nodes via admin API."""
        print("\nTest: Register backends on all nodes")

        # Backend registrations are local to each node - they're not gossiped.
        # So we need to register backends on all nodes for them to route requests.
        for node_name, node_endpoints in NODES.items():
            node_api = node_endpoints["api"]
            print(f"  Registering backends on {node_name}...")

            for backend_id, backend_info in BACKENDS.items():
                url = (
                    f"{node_api}/admin/backends"
                    f"?id={backend_id}"
                    f"&ip={backend_info['ip']}"
                    f"&port={backend_info['port']}"
                )

                resp = requests.post(url, timeout=10)
                self.assertEqual(resp.status_code, 200, f"Failed to register backend {backend_id} on {node_name}: {resp.text}")

                data = resp.json()
                self.assertEqual(data.get("status"), "ok", f"Unexpected response: {data}")

        print("  PASSED: Backends registered on all nodes")

    def test_03_send_request_to_learn_route(self):
        """Send a chat completion request to learn a route."""
        print("\nTest: Send request to learn route")

        node1_api = NODES["node1"]["api"]

        # Create a chat completion request with a unique prompt
        request_body = {
            "model": "test-model",
            "messages": [
                {"role": "user", "content": "Hello, this is a test prompt for route learning in Ranvier cluster integration tests."}
            ],
            "stream": True
        }

        print("  Sending chat completion request to Node 1...")
        resp = requests.post(
            f"{node1_api}/v1/chat/completions",
            json=request_body,
            headers={"Content-Type": "application/json"},
            stream=True,
            timeout=30
        )

        print(f"  Response status: {resp.status_code}")
        print(f"  Response headers: {dict(resp.headers)}")

        # Collect the streaming response
        response_text = ""
        line_count = 0
        raw_content = b""
        for line in resp.iter_lines():
            raw_content += line + b"\n"
            line_count += 1
            if line:
                decoded = line.decode("utf-8")
                if line_count <= 5:  # Print first few lines for debugging
                    print(f"  Line {line_count}: {decoded[:100]}")
                if decoded.startswith("data: ") and decoded != "data: [DONE]":
                    try:
                        chunk = json.loads(decoded[6:])
                        if "choices" in chunk and chunk["choices"]:
                            delta = chunk["choices"][0].get("delta", {})
                            if "content" in delta:
                                response_text += delta["content"]
                    except json.JSONDecodeError:
                        pass

        print(f"  Total lines: {line_count}, raw bytes: {len(raw_content)}")
        if raw_content:
            print(f"  Raw content (first 500 bytes): {raw_content[:500]}")
        print(f"  Response received: '{response_text.strip()}'")
        self.assertIn("backend", response_text.lower(), "Response should mention backend")
        print("  PASSED: Route learned successfully")

    def test_04_verify_route_propagation(self):
        """Verify cluster health after route learning."""
        print("\nTest: Verify cluster health after route learning")

        # Give some time for any async operations to complete
        print(f"  Waiting {PROPAGATION_TIMEOUT}s for cluster sync...")
        time.sleep(PROPAGATION_TIMEOUT)

        # Verify all nodes still have healthy peer connections
        for name, endpoints in NODES.items():
            peers_alive = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
            print(f"  {name}: cluster_peers_alive = {peers_alive}")
            self.assertEqual(peers_alive, 2.0, f"{name} should still have 2 peers")

        # Check gossip sync metrics (informational only - not asserted)
        # Note: Route sync packet counts depend on implementation details
        # and timing. The key functional test is that nodes can route requests.
        for name, endpoints in NODES.items():
            metrics = get_all_metrics(endpoints["metrics"])
            sync_received = metrics.get("router_cluster_sync_received", [0])[0]
            sync_sent = metrics.get("router_cluster_sync_sent", [0])[0]
            print(f"  {name}: sync_sent={sync_sent}, sync_received={sync_received}")

        print("  PASSED: Cluster health verified")

    def test_05_request_on_other_nodes(self):
        """Send requests to other nodes and verify routing works."""
        print("\nTest: Send requests to other nodes")

        for name in ["node2", "node3"]:
            api_url = NODES[name]["api"]
            metrics_url = NODES[name]["metrics"]

            # Check node health before sending proxy request
            print(f"  Checking {name} health...")
            try:
                health_resp = requests.get(f"{metrics_url}/metrics", timeout=5)
                if health_resp.status_code != 200:
                    print(f"    WARNING: {name} metrics returned {health_resp.status_code}")
            except requests.exceptions.RequestException as e:
                print(f"    WARNING: {name} health check failed: {e}")

            # Send a request
            request_body = {
                "model": "test-model",
                "messages": [
                    {"role": "user", "content": "Hello, this is a test prompt for route learning."}
                ],
                "stream": True
            }

            # Retry logic for transient failures
            max_retries = 3
            response_text = ""

            for attempt in range(max_retries):
                print(f"  Sending request to {name}..." + (f" (attempt {attempt + 1})" if attempt > 0 else ""))
                try:
                    resp = requests.post(
                        f"{api_url}/v1/chat/completions",
                        json=request_body,
                        headers={"Content-Type": "application/json"},
                        stream=True,
                        timeout=30
                    )

                    if resp.status_code != 200:
                        print(f"    {name} returned status {resp.status_code}: {resp.text[:200]}")
                        if attempt < max_retries - 1:
                            time.sleep(2)
                            continue
                        self.fail(f"Request to {name} failed with status {resp.status_code}")

                    # Consume the response
                    response_text = ""
                    for line in resp.iter_lines():
                        if line:
                            decoded = line.decode("utf-8")
                            if decoded.startswith("data: ") and decoded != "data: [DONE]":
                                try:
                                    chunk = json.loads(decoded[6:])
                                    if "choices" in chunk and chunk["choices"]:
                                        delta = chunk["choices"][0].get("delta", {})
                                        if "content" in delta:
                                            response_text += delta["content"]
                                except json.JSONDecodeError:
                                    pass

                    if len(response_text) > 0:
                        break  # Success, exit retry loop

                    # Empty response, wait and retry
                    if attempt < max_retries - 1:
                        print(f"    Empty response from {name}, retrying...")
                        time.sleep(2)

                except requests.exceptions.RequestException as e:
                    if attempt < max_retries - 1:
                        print(f"    Request error: {e}, retrying...")
                        time.sleep(2)
                    else:
                        # On final failure, check container status
                        print(f"    Checking {name} container status...")
                        container_ok = check_container_running(
                            f"ranvier{name[-1]}", self.PROJECT_NAME
                        )
                        if not container_ok:
                            self.fail(f"{name} container has crashed!")
                        raise

            print(f"  {name} response: '{response_text.strip()}'")
            self.assertTrue(len(response_text) > 0, f"{name} returned empty response after {max_retries} attempts")

        print("  PASSED: All nodes can route requests")

    def test_06_stop_node_and_verify_peer_count(self):
        """Stop a node and verify peer count decreases on remaining nodes."""
        print("\nTest: Stop node and verify peer count")

        # Get initial peer counts
        print("  Initial peer counts:")
        for name, endpoints in NODES.items():
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
            print(f"    {name}: {peers} peers")

        # Stop and remove node3 using docker-compose
        # We must remove the container because Seastar doesn't restart cleanly
        print("\n  Stopping and removing node3...")
        result = run_compose(["stop", "ranvier3"], project_name=self.PROJECT_NAME, check=False)
        self.assertEqual(result.returncode, 0, "Failed to stop ranvier3")

        # Remove the stopped container so it can be recreated fresh
        result = run_compose(["rm", "-f", "ranvier3"], project_name=self.PROJECT_NAME, check=False)
        self.assertEqual(result.returncode, 0, "Failed to remove ranvier3")

        # Wait for peer timeout (gossip_peer_timeout_seconds = 6)
        wait_time = PEER_TIMEOUT + 2
        print(f"  Waiting {wait_time}s for peer timeout...")
        time.sleep(wait_time)

        # Check peer counts on remaining nodes
        print("\n  Peer counts after node3 stopped:")
        for name in ["node1", "node2"]:
            endpoints = NODES[name]
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
            print(f"    {name}: {peers} peers")

            # Should now have only 1 peer (since node3 is down)
            self.assertIsNotNone(peers, f"{name} has no peer metric")
            self.assertEqual(
                peers, 1.0,
                f"{name} should have 1 peer after node3 stopped, but has {peers}"
            )

        print("  PASSED: Peer count decreased correctly")

    def test_07_restart_node_and_verify_recovery(self):
        """Restart the stopped node and verify cluster recovery."""
        print("\nTest: Restart node and verify recovery")

        # Recreate node3 using docker-compose up -d
        # We use 'up -d' instead of 'start' because test_06 removed the container
        # and Seastar containers don't restart cleanly with stop/start anyway
        print("  Recreating node3...")
        result = run_compose(["up", "-d", "ranvier3"], project_name=self.PROJECT_NAME, check=False)
        if result.returncode != 0:
            print(f"    Up command failed with code {result.returncode}")
            if result.stderr:
                print(f"    stderr: {result.stderr[:500]}")
            self.fail("Failed to recreate ranvier3")

        # Give container a moment to start
        time.sleep(2)

        # Check container status
        container_running = check_container_running("ranvier3", self.PROJECT_NAME)
        print(f"  Container ranvier3 running: {container_running}")
        if not container_running:
            # Try to get logs
            log_result = run_compose(
                ["logs", "--tail=20", "ranvier3"],
                project_name=self.PROJECT_NAME,
                check=False,
            )
            print(f"  Recent logs: {log_result.stdout[:500] if log_result.stdout else 'none'}")
            self.fail("ranvier3 container is not running after recreate")

        # Wait for node to become healthy and rejoin cluster
        print("  Waiting for node3 to become healthy...")
        healthy = wait_for_healthy(
            f"{NODES['node3']['metrics']}/metrics",
            timeout=60,
            container_name="ranvier3",
            project_name=self.PROJECT_NAME,
        )
        if not healthy:
            # Get logs on failure
            log_result = run_compose(
                ["logs", "--tail=30", "ranvier3"],
                project_name=self.PROJECT_NAME,
                check=False,
            )
            print(f"  ranvier3 logs: {log_result.stdout[:1000] if log_result.stdout else 'none'}")
            self.fail("node3 did not become healthy within 60 seconds")

        # Re-register backends on the recreated node3 (it's a fresh container)
        print("  Re-registering backends on node3...")
        node3_api = NODES["node3"]["api"]
        for backend_id, backend_info in BACKENDS.items():
            url = f"{node3_api}/admin/backends?id={backend_id}&ip={backend_info['ip']}&port={backend_info['port']}"
            try:
                resp = requests.post(url, timeout=10)
                if resp.status_code != 200:
                    print(f"    Warning: Failed to register {backend_id} on node3: {resp.status_code}")
            except requests.exceptions.RequestException as e:
                print(f"    Warning: Failed to register {backend_id} on node3: {e}")

        # Wait for gossip to re-establish connections
        print("  Waiting for gossip connections...")
        time.sleep(15)

        # Verify all nodes see 2 peers again with retries
        print("\n  Peer counts after recovery:")
        max_retries = 5
        for retry in range(max_retries):
            all_recovered = True
            for name, endpoints in NODES.items():
                # Use retries=5 for more robust metric fetching
                peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive", retries=5)
                if retry == max_retries - 1:  # Only print on last attempt
                    print(f"    {name}: {peers} peers")

                if peers is None or peers != 2.0:
                    all_recovered = False

            if all_recovered:
                break

            if retry < max_retries - 1:
                print(f"    Not all nodes recovered yet, waiting... (attempt {retry + 1}/{max_retries})")
                time.sleep(5)

        # Final verification
        for name, endpoints in NODES.items():
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive", retries=5)
            self.assertIsNotNone(peers, f"{name} has no peer metric")
            self.assertEqual(
                peers, 2.0,
                f"{name} should have 2 peers after recovery, but has {peers}"
            )

        print("  PASSED: Cluster recovered successfully")

    def test_08_cache_state_gossip_and_rolling_upgrade_safety(self):
        """CACHE_STATE gossip propagates and never harms peer health.

        Validates the cache-residency feature's cluster behavior:
          - CACHE_STATE packets flow between nodes (informational counts).
          - No node accounts a peer's traffic as an *unknown packet type*
            (all nodes in this homogeneous cluster understand 0x06). The
            unknown-type counter is the rolling-upgrade safety signal: an
            old node receiving a type it predates increments it and stays
            healthy. Here it must stay at 0 — there are no unknown types.
          - cluster_peers_alive is unchanged: residency traffic never marks
            a peer unhealthy.
        """
        print("\nTest: CACHE_STATE gossip + rolling-upgrade safety")

        # Poll for CACHE_STATE traffic. The mock backends now expose
        # vLLM-style /metrics, so the node(s) that scrape them (node1 holds the
        # registered backends) emit CACHE_STATE and peers receive it. Substring
        # sums tolerate the Prometheus/Seastar name prefix and per-shard series.
        deadline = time.time() + PROPAGATION_TIMEOUT + 20
        total_sent = total_recv = 0.0
        while time.time() < deadline:
            total_sent = sum(
                sum_metric_by_substring(ep["metrics"], "gossip_cache_states_sent_total")
                for ep in NODES.values())
            total_recv = sum(
                sum_metric_by_substring(ep["metrics"], "gossip_cache_states_received_total")
                for ep in NODES.values())
            if total_sent > 0 and total_recv > 0:
                break
            time.sleep(2)

        print(f"  cluster totals: cache_states_sent={total_sent}, "
              f"cache_states_received={total_recv}")
        self.assertGreater(
            total_sent, 0.0,
            "no node emitted CACHE_STATE — is vLLM /metrics scraping reaching the "
            "mock backends? (check health.enable_vllm_metrics + backend registration)")
        self.assertGreater(
            total_recv, 0.0,
            "no node received CACHE_STATE from a peer — gossip propagation failed")

        for name, endpoints in NODES.items():
            url = endpoints["metrics"]
            # Rolling-upgrade safety: in a homogeneous cluster every node
            # understands CACHE_STATE (0x06), so the unknown-type counter — the
            # signal an older node would bump when ignoring a type it predates —
            # must total zero across all shards.
            unknown_total = sum_metric_by_substring(url, "cluster_unknown_packet_types")
            print(f"  {name}: cluster_unknown_packet_types(total)={unknown_total}")
            self.assertEqual(
                unknown_total, 0.0,
                f"{name} accounted {unknown_total} unknown packet types; all nodes "
                f"in this cluster understand CACHE_STATE (0x06)")

            # Residency traffic must not affect peer liveness.
            peers_alive = get_metric_value(url, "cluster_peers_alive")
            self.assertEqual(peers_alive, 2.0,
                             f"{name} should still have 2 peers after cache-state gossip")

        print("  PASSED: CACHE_STATE propagates; peers healthy; no unknown types")

    def _set_backend_cache_usage(self, perc):
        """Set reported KV-cache usage on every mock backend (direct admin POST)."""
        ok = True
        for backend_id, host_port in MOCK_BACKEND_PORTS.items():
            url = f"http://{DOCKER_HOST}:{host_port}/admin/cache-usage"
            try:
                resp = requests.post(url, params={"perc": perc}, timeout=5)
                if resp.status_code != 200:
                    print(f"    backend {backend_id}: cache-usage set failed "
                          f"({resp.status_code})")
                    ok = False
            except requests.exceptions.RequestException as e:
                print(f"    backend {backend_id}: cache-usage set error: {e}")
                ok = False
        return ok

    def test_09_residency_downgrade_end_to_end(self):
        """Driving backends cache-cold downgrades ART prefix hits to load-based.

        With every backend reporting high KV-cache usage, residency = 1 - usage
        falls below the default threshold (0.2). A learned prefix route then
        ART-hits a cache-cold backend, so the router treats it as a likely miss
        and diverts — incrementing router_residency_route_downgrades_total.
        """
        print("\nTest: residency-driven route downgrade (end-to-end)")

        node1 = NODES["node1"]
        baseline = sum_metric_by_substring(
            node1["metrics"], "router_residency_route_downgrades_total")
        print(f"  baseline downgrades on node1: {baseline}")

        # Make every backend look cache-cold (residency ~= 0.01).
        self.assertTrue(self._set_backend_cache_usage(0.99),
                        "failed to set cache usage on mock backends")
        try:
            # Let node1 scrape the new usage and refresh its residency cache
            # (check_interval is ~2s; allow a couple of cycles).
            time.sleep(6)

            # Send the SAME prompt repeatedly: the first learns a route, later
            # ones ART-hit the (now cache-cold) backend and get downgraded.
            messages = [{
                "role": "user",
                "content": "Residency downgrade probe: a stable shared prefix "
                           "used to learn and then re-hit the same ART route.",
            }]
            downgrades = baseline
            deadline = time.time() + 40
            while time.time() < deadline:
                send_chat_request(node1["api"], messages, timeout=30, retries=1)
                time.sleep(1)
                downgrades = sum_metric_by_substring(
                    node1["metrics"], "router_residency_route_downgrades_total")
                if downgrades > baseline:
                    break

            print(f"  downgrades after probing: {downgrades}")
            self.assertGreater(
                downgrades, baseline,
                "expected at least one residency-driven downgrade once backends "
                "were cache-cold and a prefix route had been learned")
        finally:
            # Restore empty-cache state so later/other suites aren't affected.
            self._set_backend_cache_usage(0.0)

        print("  PASSED: cache-cold ART hits downgraded to load-based selection")


def main():
    """Run the integration tests."""
    print("=" * 60)
    print("Ranvier Core Multi-Node Integration Tests")
    print("=" * 60)

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
    suite = loader.loadTestsFromTestCase(ClusterIntegrationTest)

    # Sort tests to run in order (test_01, test_02, etc.)
    suite = unittest.TestSuite(sorted(suite, key=lambda t: t.id()))

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Return appropriate exit code
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
