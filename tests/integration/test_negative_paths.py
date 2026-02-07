#!/usr/bin/env python3
"""
Negative-Path Integration Tests for Ranvier Core

This test suite validates failure handling and resilience behaviors:
1. Split-brain: Network partition detection and recovery via quorum
2. Backend flap: Circuit breaker engagement on rapid backend restarts
3. Config reload: Invalid YAML rejected, old config preserved on SIGHUP
4. Rate limit: 503 responses with Retry-After when rate limits exceeded
5. Oversized request: Rejection of request bodies exceeding max size

All tests use Docker Compose (docker-compose.test.yml) with a 3-node cluster
and mock backends, following the same pattern as the existing happy-path suites.

Usage:
    python tests/integration/test_negative_paths.py

Requirements:
    - Docker and docker-compose installed
    - requests library
"""

import concurrent.futures
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
PROJECT_NAME = "ranvier-negative-path-test"

DOCKER_NETWORK = "ranvier-negative-path-test_ranvier-test"


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

# Container names used in docker-compose
CONTAINER_NAMES = {
    "node1": "ranvier1",
    "node2": "ranvier2",
    "node3": "ranvier3",
    "backend1": "backend1",
    "backend2": "backend2",
}

# Timeouts
STARTUP_TIMEOUT = 60
PEER_TIMEOUT = 10
REQUEST_TIMEOUT = 30


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


def docker_exec(container_name: str, cmd: List[str], timeout: int = 30) -> subprocess.CompletedProcess:
    """Execute a command inside a running container."""
    full_cmd = ["docker", "exec", container_name] + cmd
    return subprocess.run(full_cmd, capture_output=True, text=True, timeout=timeout)


def docker_network_disconnect(network: str, container: str) -> bool:
    """Disconnect a container from a Docker network."""
    try:
        result = subprocess.run(
            ["docker", "network", "disconnect", "--force", network, container],
            capture_output=True, text=True, timeout=15
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def docker_network_connect(network: str, container: str, ip: str = None) -> bool:
    """Reconnect a container to a Docker network."""
    try:
        cmd = ["docker", "network", "connect"]
        if ip:
            cmd += ["--ip", ip]
        cmd += [network, container]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


# =============================================================================
# Metrics Helpers
# =============================================================================

def get_metric_value(metrics_url: str, metric_name: str, retries: int = 3) -> Optional[float]:
    """Extract a specific metric value from Prometheus endpoint."""
    for attempt in range(retries):
        try:
            resp = requests.get(f"{metrics_url}/metrics", timeout=5)
            if resp.status_code != 200:
                if attempt < retries - 1:
                    time.sleep(1)
                    continue
                return None

            for line in resp.text.split("\n"):
                if line.startswith("#"):
                    continue
                if metric_name in line:
                    match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                    if match:
                        return float(match.group(1))
            return None
        except requests.exceptions.RequestException:
            if attempt < retries - 1:
                time.sleep(1)
            else:
                return None
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


# =============================================================================
# Request Helpers
# =============================================================================

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


def send_chat_request(
    api_url: str,
    prompt: str = "test",
    timeout: int = REQUEST_TIMEOUT
) -> Tuple[int, str, Dict[str, str]]:
    """Send a chat completion request. Returns (status_code, body, headers)."""
    payload = {
        "model": "test-model",
        "messages": [{"role": "user", "content": prompt}],
        "stream": True
    }
    try:
        resp = requests.post(
            f"{api_url}/v1/chat/completions",
            json=payload,
            headers={"Content-Type": "application/json"},
            stream=True,
            timeout=timeout
        )

        response_text = ""
        if resp.status_code == 200:
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
        else:
            response_text = resp.text

        return resp.status_code, response_text, dict(resp.headers)

    except requests.exceptions.RequestException as e:
        return -1, str(e), {}


def get_docker_network_name() -> str:
    """Get the actual Docker network name for the test project.

    Docker Compose creates networks with the project name prefix.
    The network defined in docker-compose.test.yml is 'ranvier-test',
    so the full name is '{PROJECT_NAME}_ranvier-test'.
    """
    try:
        result = subprocess.run(
            ["docker", "network", "ls", "--format", "{{.Name}}"],
            capture_output=True, text=True, timeout=10
        )
        for line in result.stdout.strip().split("\n"):
            if PROJECT_NAME in line and "ranvier-test" in line:
                return line.strip()
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    # Fallback to the expected name
    return f"{PROJECT_NAME}_ranvier-test"


# =============================================================================
# Test Suite
# =============================================================================

class NegativePathTest(unittest.TestCase):
    """Integration tests for negative/failure paths."""

    @classmethod
    def setUpClass(cls):
        """Start the Docker Compose cluster."""
        print("\n" + "=" * 70)
        print("Setting up test cluster for Negative Path tests...")
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
        all_healthy = True
        for name, endpoints in NODES.items():
            container = CONTAINER_NAMES.get(name)
            print(f"  Waiting for {name}...")
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
        time.sleep(5)

        # Discover the actual Docker network name
        cls.docker_network = get_docker_network_name()
        print(f"\nDocker network: {cls.docker_network}")

        print("\nCluster is ready for negative path tests")
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
                    print(f"    Quorum state: {quorum_state}")
                    # With 3-node cluster and 1 partitioned, quorum may still hold
                    # (2 out of 3 is majority). The key check is that peers_alive dropped.
                    self.assertIn(
                        quorum_state,
                        ["quorum", "degraded", "no_quorum"],
                        f"Unexpected quorum state: {quorum_state}"
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
        """Rapidly stop/start a mock backend to trigger the circuit breaker.

        The circuit breaker should open after repeated failures, observable via
        the circuit_breaker_opens metric on the metrics endpoint.
        """
        print("\nTest: Backend flap — circuit breaker engagement")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        # Get initial circuit breaker opens count
        initial_opens = get_metric_value(node1_metrics, "circuit_breaker_opens") or 0
        print(f"  Initial circuit_breaker_opens: {initial_opens}")

        # Rapidly stop and start backend1 to cause failures
        flap_cycles = 3
        print(f"\n  Flapping backend1 ({flap_cycles} cycles)...")

        for cycle in range(flap_cycles):
            print(f"    Cycle {cycle + 1}: stopping backend1...")
            run_compose(["stop", "backend1"], check=False)

            # Send requests while backend is down to trigger failures
            print(f"    Sending requests while backend1 is down...")
            for i in range(5):
                send_chat_request(node1_api, f"flap-test-{cycle}-{i}", timeout=10)
                time.sleep(0.2)

            print(f"    Cycle {cycle + 1}: starting backend1...")
            run_compose(["start", "backend1"], check=False)

            # Brief pause to let the backend partially come up
            time.sleep(2)

        # Wait for metrics to settle
        time.sleep(2)

        # Check circuit breaker metric
        final_opens = get_metric_value(node1_metrics, "circuit_breaker_opens") or 0
        opens_delta = final_opens - initial_opens
        print(f"\n  Final circuit_breaker_opens: {final_opens}")
        print(f"  Circuit breaker opens during test: {opens_delta}")

        # The circuit breaker should have opened at least once due to backend failures
        self.assertGreater(
            opens_delta, 0,
            f"Circuit breaker should have opened at least once during backend flapping, "
            f"but opens delta was {opens_delta}"
        )

        # Ensure backend1 is fully back up for subsequent tests
        print("\n  Ensuring backend1 is running...")
        run_compose(["start", "backend1"], check=False)
        time.sleep(3)

        # Verify requests succeed after backends stabilize
        # May need a few retries as circuit breaker recovers
        print("  Verifying requests succeed after stabilization...")
        success = False
        for attempt in range(10):
            status, _, _ = send_chat_request(node1_api, "recovery-check", timeout=10)
            if status == 200:
                success = True
                break
            time.sleep(2)

        self.assertTrue(success, "Requests should succeed after backend stabilizes")
        print("  PASSED: Circuit breaker engaged during backend flapping")

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

        # Write invalid YAML to a temporary config file inside the container
        # The server reads its config from the config path specified at startup.
        # We write invalid YAML there, send SIGHUP, then restore.
        invalid_yaml = "{{{{invalid: yaml: [unterminated"

        print("  Writing invalid YAML to container config...")
        # First, find the config file path by checking the container's command
        # The test config is typically at /app/ranvier.yaml or similar
        # Use a more robust approach: write to a known path and use env override
        write_result = subprocess.run(
            ["docker", "exec", container, "sh", "-c",
             "cp /app/ranvier.yaml /app/ranvier.yaml.bak 2>/dev/null; "
             f"echo '{invalid_yaml}' > /app/ranvier.yaml"],
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
            # Restore the original config
            print("  Restoring original config...")
            subprocess.run(
                ["docker", "exec", container, "sh", "-c",
                 "cp /app/ranvier.yaml.bak /app/ranvier.yaml 2>/dev/null || true"],
                capture_output=True, text=True, timeout=10
            )

            # Send SIGHUP again to reload the valid config
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

        Enables rate limiting via SIGHUP config reload with a very low limit,
        then floods the endpoint with requests. Expects 503 responses with
        Retry-After header (Ranvier uses 503 for rate limiting).
        """
        print("\nTest: Rate limit exceeded")

        node1_api = NODES["node1"]["api"]
        container = CONTAINER_NAMES["node1"]

        # Enable rate limiting with very low limits via config file update
        # RateLimitConfig: enabled=true, requests_per_second=2, burst_size=1
        rate_limit_yaml = (
            "rate_limit:\\n"
            "  enabled: true\\n"
            "  requests_per_second: 2\\n"
            "  burst_size: 1\\n"
        )

        print("  Enabling rate limiting with low limits (2 rps, burst 1)...")
        # Read current config, append rate limit section
        update_result = subprocess.run(
            ["docker", "exec", container, "sh", "-c",
             "cp /app/ranvier.yaml /app/ranvier.yaml.bak 2>/dev/null; "
             f"sed -i 's/rate_limit:/rate_limit:/' /app/ranvier.yaml; "
             f"sed -i 's/  enabled: false/  enabled: true\\n  requests_per_second: 2\\n  burst_size: 1/' /app/ranvier.yaml"],
            capture_output=True, text=True, timeout=10
        )

        # Send SIGHUP to apply the new config
        print("  Sending SIGHUP to apply rate limit config...")
        subprocess.run(
            ["docker", "kill", "--signal=HUP", container],
            capture_output=True, text=True, timeout=10
        )
        time.sleep(3)

        try:
            # Flood with concurrent requests to exceed the rate limit
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

            # With 2 rps and burst of 1, sending 30 concurrent requests should
            # trigger rate limiting on many of them
            self.assertGreater(
                rate_limited_count, 0,
                f"Some requests should be rate-limited, but none were. "
                f"Status codes: {status_codes}"
            )

            if rate_limited_count > 0:
                self.assertTrue(
                    retry_after_seen,
                    "Rate-limited responses should include Retry-After header"
                )

        finally:
            # Restore original config (rate limiting disabled)
            print("\n  Restoring original config (rate limiting disabled)...")
            subprocess.run(
                ["docker", "exec", container, "sh", "-c",
                 "cp /app/ranvier.yaml.bak /app/ranvier.yaml 2>/dev/null || true"],
                capture_output=True, text=True, timeout=10
            )
            subprocess.run(
                ["docker", "kill", "--signal=HUP", container],
                capture_output=True, text=True, timeout=10
            )
            time.sleep(3)

            # Verify normal operation restored
            status, _, _ = send_chat_request(node1_api, "post-rate-limit-check", timeout=10)
            if status != 200:
                print(f"  Warning: Post-restore request returned {status}")
                time.sleep(5)

        print("  PASSED: Rate limiting enforced with Retry-After header")

    # =========================================================================
    # Test 5: Oversized Request Body
    # =========================================================================

    def test_05_oversized_request_body(self):
        """Send a request body exceeding max_request_body_size, verify rejection.

        Ranvier enforces a maximum body size (CrossShardRequestLimits::max_body_size
        = 128 MB). Seastar's HTTP server also has its own content length limit.
        We send a large payload and verify the server rejects it with an error
        status (413 or 400).
        """
        print("\nTest: Oversized request body rejection")

        node1_api = NODES["node1"]["api"]

        # Seastar's default HTTP content length limit is typically small (~2MB).
        # We try with a payload that should exceed limits.
        # Generate a payload of ~5 MB (well over typical HTTP limits but not
        # so large that it takes forever to transmit)
        oversized_content = "x" * (5 * 1024 * 1024)  # 5 MB of data

        payload = {
            "model": "test-model",
            "messages": [{"role": "user", "content": oversized_content}],
            "stream": True
        }

        print(f"  Sending oversized request (~5 MB body)...")
        try:
            resp = requests.post(
                f"{node1_api}/v1/chat/completions",
                json=payload,
                headers={"Content-Type": "application/json"},
                timeout=30
            )

            print(f"  Response status: {resp.status_code}")
            print(f"  Response body (first 200 chars): {resp.text[:200]}")

            # Server should reject with 413 (Payload Too Large), 400 (Bad Request),
            # or close the connection. Seastar may also return 500 for oversized payloads.
            self.assertIn(
                resp.status_code,
                [400, 413, 500],
                f"Oversized request should be rejected with 400/413/500, got {resp.status_code}"
            )

        except requests.exceptions.ConnectionError as e:
            # Server may close the connection for oversized payloads
            print(f"  Connection closed by server (expected for oversized body): {e}")
            # This is acceptable behavior — the server refused the payload

        except requests.exceptions.ChunkedEncodingError as e:
            # Server may reset connection during chunked transfer
            print(f"  Chunked encoding error (expected for oversized body): {e}")

        # Verify the server is still healthy after handling the oversized request
        print("  Verifying server health after oversized request...")
        status, _, _ = send_chat_request(node1_api, "post-oversize-check", timeout=10)
        self.assertEqual(
            status, 200,
            "Server should remain healthy after rejecting oversized request"
        )

        print("  PASSED: Oversized request rejected, server remains healthy")


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
