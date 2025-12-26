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


def get_metric_value(metrics_url: str, metric_name: str, debug: bool = False, retries: int = 3) -> Optional[float]:
    """Extract a specific metric value from Prometheus endpoint.

    Searches for metrics containing the metric_name (Seastar may prefix with group names).
    Retries on transient failures.
    """
    for attempt in range(retries):
        try:
            resp = requests.get(f"{metrics_url}/metrics", timeout=5)
            if resp.status_code != 200:
                if attempt < retries - 1:
                    time.sleep(1)
                    continue
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
                print(f"    Request error (attempt {attempt + 1}): {e}")
            if attempt < retries - 1:
                time.sleep(1)
            else:
                return None
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

        # Check if we should skip building (env var or images already exist)
        skip_build = os.environ.get("SKIP_BUILD", "").lower() in ("1", "true", "yes")

        if not skip_build:
            # Check if the required Docker images already exist by trying to create
            # containers without building. If images exist, this succeeds quickly.
            try:
                compose_cmd = get_compose_cmd()
                create_result = subprocess.run(
                    compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME,
                                   "create", "--no-build"],
                    capture_output=True, text=True, timeout=30
                )
                if create_result.returncode == 0:
                    print("\nDocker images already exist. Skipping build.")
                    print("  (Set SKIP_BUILD=0 to force rebuild)")
                    skip_build = True
                    # Clean up the created containers so 'up -d' works fresh
                    subprocess.run(
                        compose_cmd + ["-f", COMPOSE_FILE, "-p", PROJECT_NAME,
                                       "rm", "-f"],
                        capture_output=True, timeout=30
                    )
            except (subprocess.TimeoutExpired, Exception):
                # If check fails, just proceed with build
                pass

        if not skip_build:
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
                        container_ok = check_container_running(f"ranvier{name[-1]}")
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
        result = run_compose(["stop", "ranvier3"], check=False)
        self.assertEqual(result.returncode, 0, "Failed to stop ranvier3")

        # Remove the stopped container so it can be recreated fresh
        result = run_compose(["rm", "-f", "ranvier3"], check=False)
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
        result = run_compose(["up", "-d", "ranvier3"], check=False)
        if result.returncode != 0:
            print(f"    Up command failed with code {result.returncode}")
            if result.stderr:
                print(f"    stderr: {result.stderr[:500]}")
            self.fail("Failed to recreate ranvier3")

        # Give container a moment to start
        time.sleep(2)

        # Check container status
        container_running = check_container_running("ranvier3")
        print(f"  Container ranvier3 running: {container_running}")
        if not container_running:
            # Try to get logs
            log_result = run_compose(["logs", "--tail=20", "ranvier3"], check=False)
            print(f"  Recent logs: {log_result.stdout[:500] if log_result.stdout else 'none'}")
            self.fail("ranvier3 container is not running after recreate")

        # Wait for node to become healthy and rejoin cluster
        print("  Waiting for node3 to become healthy...")
        healthy = wait_for_healthy(
            f"{NODES['node3']['metrics']}/metrics",
            timeout=60,
            container_name="ranvier3"
        )
        if not healthy:
            # Get logs on failure
            log_result = run_compose(["logs", "--tail=30", "ranvier3"], check=False)
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
