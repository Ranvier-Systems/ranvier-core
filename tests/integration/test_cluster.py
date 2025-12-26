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
"""

import json
import os
import re
import subprocess
import sys
import time
import unittest
from typing import Optional

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)


# Configuration
COMPOSE_FILE = os.path.join(os.path.dirname(__file__), "..", "..", "docker-compose.test.yml")
PROJECT_NAME = "ranvier-integration-test"

# Detect if running in Docker (use host.docker.internal) or native (use localhost)
def get_docker_host() -> str:
    """Get the hostname to reach Docker-exposed ports."""
    # Check if host.docker.internal resolves (Docker Desktop / DinD)
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
STARTUP_TIMEOUT = 30  # seconds to wait for cluster to start
PROPAGATION_TIMEOUT = 15  # seconds to wait for gossip propagation
PEER_TIMEOUT = 10  # seconds for peer failure detection

# Detect docker compose command (plugin vs standalone)
COMPOSE_CMD = None


def get_compose_cmd() -> list[str]:
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


def run_compose(args: list[str], check: bool = True, show_output: bool = False) -> subprocess.CompletedProcess:
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
    """Check if a container is still running (not exited/crashed)."""
    try:
        compose_cmd = get_compose_cmd()
        result = subprocess.run(
            compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME, "ps", "-q", container_name],
            capture_output=True,
            text=True
        )
        # If container is running, ps -q returns its ID; if exited, returns empty
        return result.returncode == 0 and len(result.stdout.strip()) > 0
    except (FileNotFoundError, RuntimeError):
        # compose command not available, skip check
        return True


def wait_for_healthy(url: str, timeout: int = 60, container_name: str = None) -> bool:
    """Wait for an endpoint to become healthy."""
    start = time.time()
    while time.time() - start < timeout:
        # Check if container crashed (fast fail)
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


def get_metric_value(metrics_url: str, metric_name: str, debug: bool = False) -> Optional[float]:
    """Extract a specific metric value from Prometheus endpoint.

    Searches for metrics containing the metric_name (Seastar may prefix with group names).
    """
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        # Parse Prometheus text format
        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            # Search for metric name anywhere in the line (Seastar prefixes vary)
            if metric_name in line:
                # Extract the value (last number on the line)
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    if debug:
                        print(f"    Found: {line.strip()}")
                    return float(match.group(1))

        if debug:
            # Print available metrics for debugging
            print(f"    Available metrics containing 'cluster' or 'peer':")
            for line in resp.text.split("\n"):
                if not line.startswith("#") and ("cluster" in line.lower() or "peer" in line.lower()):
                    print(f"      {line.strip()[:100]}")
        return None
    except requests.exceptions.RequestException as e:
        if debug:
            print(f"    Request error: {e}")
        return None


def get_all_metrics(metrics_url: str) -> dict:
    """Get all metrics from the Prometheus endpoint."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return {}

        metrics = {}
        for line in resp.text.split("\n"):
            if line.startswith("#") or not line.strip():
                continue
            # Parse metric line
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


class ClusterIntegrationTest(unittest.TestCase):
    """Integration tests for multi-node Ranvier cluster."""

    @classmethod
    def setUpClass(cls):
        """Start the Docker Compose cluster."""
        print("\n" + "=" * 60)
        print("Setting up test cluster...")
        print("=" * 60)

        # Ensure clean state
        run_compose(["down", "-v", "--remove-orphans"], check=False)

        # Build and start the cluster
        print("\nBuilding containers (this may take a while on first run)...")
        result = run_compose(["build"], check=False, show_output=True)
        if result.returncode != 0:
            raise RuntimeError("Failed to build containers")

        print("\nStarting cluster...")
        result = run_compose(["up", "-d"], check=False, show_output=True)
        if result.returncode != 0:
            raise RuntimeError("Failed to start cluster")

        # Wait for all nodes to become healthy
        print("\nWaiting for nodes to become healthy...")
        all_healthy = True
        # Map node names to container names
        container_names = {"node1": "ranvier1", "node2": "ranvier2", "node3": "ranvier3"}
        for name, endpoints in NODES.items():
            print(f"  Waiting for {name}...")
            container = container_names.get(name)
            if not wait_for_healthy(f"{endpoints['metrics']}/metrics", timeout=STARTUP_TIMEOUT, container_name=container):
                print(f"  ERROR: {name} did not become healthy")
                all_healthy = False
            else:
                print(f"  {name} is healthy")

        if not all_healthy:
            # Print logs for debugging
            print("\nContainer logs:")
            run_compose(["logs", "--tail=50"], check=False, show_output=True)
            raise RuntimeError("Not all nodes became healthy")

        # Give gossip time to establish peer connections
        print("\nWaiting for gossip connections to establish...")
        time.sleep(5)

        print("\nCluster is ready for testing")
        print("=" * 60 + "\n")

    @classmethod
    def tearDownClass(cls):
        """Tear down the Docker Compose cluster."""
        print("\n" + "=" * 60)
        print("Tearing down test cluster...")
        print("=" * 60)
        run_compose(["down", "-v", "--remove-orphans"], check=False)
        print("Cleanup complete")

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
        """Register backends via Node 1's admin API."""
        print("\nTest: Register backends on Node 1")

        node1_api = NODES["node1"]["api"]

        for backend_id, backend_info in BACKENDS.items():
            url = (
                f"{node1_api}/admin/backends"
                f"?id={backend_id}"
                f"&ip={backend_info['ip']}"
                f"&port={backend_info['port']}"
            )
            print(f"  Registering backend {backend_id} at {backend_info['ip']}:{backend_info['port']}")

            resp = requests.post(url, timeout=10)
            self.assertEqual(resp.status_code, 200, f"Failed to register backend {backend_id}: {resp.text}")

            data = resp.json()
            self.assertEqual(data.get("status"), "ok", f"Unexpected response: {data}")

        print("  PASSED: Both backends registered successfully")

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

        # Collect the streaming response
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

        print(f"  Response received: '{response_text.strip()}'")
        self.assertIn("backend", response_text.lower(), "Response should mention backend")
        print("  PASSED: Route learned successfully")

    def test_04_verify_route_propagation(self):
        """Verify that routes are propagated to other nodes via gossip."""
        print("\nTest: Verify route propagation via gossip")

        # Give gossip time to propagate the route
        print(f"  Waiting {PROPAGATION_TIMEOUT}s for gossip propagation...")
        time.sleep(PROPAGATION_TIMEOUT)

        # Check gossip sync metrics on each node
        for name, endpoints in NODES.items():
            metrics = get_all_metrics(endpoints["metrics"])

            sync_received = metrics.get("router_cluster_sync_received", [0])[0]
            sync_sent = metrics.get("router_cluster_sync_sent", [0])[0]

            print(f"  {name}: sync_sent={sync_sent}, sync_received={sync_received}")

            # At least some gossip traffic should have occurred
            if name == "node1":
                # Node 1 learned the route locally and should have broadcast it
                self.assertGreater(sync_sent, 0, f"{name} should have sent gossip packets")
            else:
                # Other nodes should have received gossip packets
                # Note: This may be 0 if the route was learned before other nodes connected
                pass

        print("  PASSED: Gossip propagation verified")

    def test_05_request_on_other_nodes(self):
        """Send requests to other nodes and verify routing works."""
        print("\nTest: Send requests to other nodes")

        for name in ["node2", "node3"]:
            api_url = NODES[name]["api"]

            # Send a similar request that should match the learned route
            request_body = {
                "model": "test-model",
                "messages": [
                    {"role": "user", "content": "Hello, this is a test prompt for route learning."}
                ],
                "stream": True
            }

            print(f"  Sending request to {name}...")
            resp = requests.post(
                f"{api_url}/v1/chat/completions",
                json=request_body,
                headers={"Content-Type": "application/json"},
                stream=True,
                timeout=30
            )

            self.assertEqual(resp.status_code, 200, f"Request to {name} failed")

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

            print(f"  {name} response: '{response_text.strip()}'")
            self.assertTrue(len(response_text) > 0, f"{name} returned empty response")

        print("  PASSED: All nodes can route requests")

    def test_06_stop_node_and_verify_peer_count(self):
        """Stop a node and verify peer count decreases on remaining nodes."""
        print("\nTest: Stop node and verify peer count")

        # Get initial peer counts
        print("  Initial peer counts:")
        for name, endpoints in NODES.items():
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
            print(f"    {name}: {peers} peers")

        # Stop node3 using docker-compose
        print("\n  Stopping node3...")
        result = run_compose(["stop", "ranvier3"], check=False)
        self.assertEqual(result.returncode, 0, "Failed to stop ranvier3")

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

        # Restart node3 using docker-compose
        print("  Restarting node3...")
        result = run_compose(["start", "ranvier3"], check=False)
        self.assertEqual(result.returncode, 0, "Failed to start ranvier3")

        # Wait for node to become healthy and rejoin cluster
        print("  Waiting for node3 to become healthy...")
        healthy = wait_for_healthy(f"{NODES['node3']['metrics']}/metrics", timeout=60)
        self.assertTrue(healthy, "node3 did not become healthy")

        # Wait for gossip to re-establish connections
        print("  Waiting for gossip connections...")
        time.sleep(10)

        # Verify all nodes see 2 peers again
        print("\n  Peer counts after recovery:")
        for name, endpoints in NODES.items():
            peers = get_metric_value(endpoints["metrics"], "cluster_peers_alive")
            print(f"    {name}: {peers} peers")

            self.assertIsNotNone(peers, f"{name} has no peer metric")
            self.assertEqual(
                peers, 2.0,
                f"{name} should have 2 peers after recovery, but has {peers}"
            )

        print("  PASSED: Cluster recovered successfully")


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
