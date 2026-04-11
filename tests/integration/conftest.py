"""Shared integration-test harness for Ranvier Core.

This module is the single source of truth for docker-compose lifecycle,
metric polling, and request helpers used by the files in
``tests/integration/``.  It intentionally serves two audiences:

1. **pytest**: defines the ``ranvier_cluster`` session fixture and the
   ``cluster_metrics`` snapshot fixture.  Discovered automatically because
   the file is named ``conftest.py``.

2. **unittest**: exposes a :class:`ClusterTestCase` base class whose
   ``setUpClass`` / ``tearDownClass`` call into the same underlying
   :func:`_bring_up_cluster` / :func:`_tear_down_cluster` helpers as the
   pytest fixture.  This lets the existing unittest-style test files
   migrate one at a time without rewriting their test methods.

The historical pattern across ``test_cluster.py``, ``test_prefix_routing.py``,
``test_load_aware_routing.py``, ``test_negative_paths.py``, and
``test_graceful_shutdown.py`` duplicated ~200 lines of harness each.  Those
files are being migrated incrementally; ``test_cluster.py`` is the first
to use this module.

Build constraint: these tests require Docker and a built Ranvier image.
Do not attempt to run them in the sandbox — the developer builds and runs
them in their Docker environment.
"""

from __future__ import annotations

import json
import os
import re
import socket
import subprocess
import sys
import time
import unittest
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

# pytest is only available when running under pytest.  Unittest-style
# invocation (``python tests/integration/test_cluster.py``) still needs to
# import the harness helpers, so we tolerate a missing pytest module and
# only define fixtures when it is present.
try:
    import pytest  # type: ignore
except ImportError:  # pragma: no cover - pytest always present in CI
    pytest = None  # type: ignore


# =============================================================================
# Configuration
# =============================================================================

COMPOSE_FILE = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "docker-compose.test.yml")
)

# Default project name used by the pytest session fixture.  A deterministic
# name means concurrent pytest invocations collide intentionally (they share
# the same cluster) while concurrent unittest suites can override
# ``ClusterTestCase.PROJECT_NAME`` to stay isolated.
PYTEST_PROJECT_NAME = "ranvier-pytest-session"

# Timeouts (seconds)
STARTUP_TIMEOUT = 60
PROPAGATION_TIMEOUT = 15
PEER_TIMEOUT = 10
REQUEST_TIMEOUT = 30


def get_docker_host() -> str:
    """Return the hostname used to reach Docker-exposed ports.

    When running inside Docker-in-Docker (Docker Desktop, devcontainers),
    host-mapped ports are reachable via ``host.docker.internal``.  On bare
    Linux they are reachable via ``localhost``.
    """
    try:
        socket.gethostbyname("host.docker.internal")
        return "host.docker.internal"
    except socket.gaierror:
        return "localhost"


DOCKER_HOST = get_docker_host()

# Node endpoints (mapped ports from docker-compose.test.yml)
NODES: Dict[str, Dict[str, str]] = {
    "node1": {"api": f"http://{DOCKER_HOST}:8081", "metrics": f"http://{DOCKER_HOST}:9181"},
    "node2": {"api": f"http://{DOCKER_HOST}:8082", "metrics": f"http://{DOCKER_HOST}:9182"},
    "node3": {"api": f"http://{DOCKER_HOST}:8083", "metrics": f"http://{DOCKER_HOST}:9183"},
}

# Mock backends (as seen from inside the Docker network)
BACKENDS: Dict[int, Dict[str, object]] = {
    1: {"ip": "172.28.1.10", "port": 8000},
    2: {"ip": "172.28.1.11", "port": 8000},
}

# docker-compose service / container names
CONTAINER_NAMES: Dict[str, str] = {
    "node1": "ranvier1",
    "node2": "ranvier2",
    "node3": "ranvier3",
    "backend1": "backend1",
    "backend2": "backend2",
}


# =============================================================================
# Docker Compose helpers
# =============================================================================

_COMPOSE_CMD_CACHE: Optional[List[str]] = None


def get_compose_cmd() -> List[str]:
    """Detect and cache the docker compose invocation.

    Prefers the ``docker compose`` plugin; falls back to the standalone
    ``docker-compose`` binary.  Raises :class:`RuntimeError` if neither is
    available.
    """
    global _COMPOSE_CMD_CACHE
    if _COMPOSE_CMD_CACHE is not None:
        return _COMPOSE_CMD_CACHE

    try:
        result = subprocess.run(
            ["docker", "compose", "version"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            _COMPOSE_CMD_CACHE = ["docker", "compose"]
            return _COMPOSE_CMD_CACHE
    except FileNotFoundError:
        pass

    try:
        result = subprocess.run(
            ["docker-compose", "--version"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            _COMPOSE_CMD_CACHE = ["docker-compose"]
            return _COMPOSE_CMD_CACHE
    except FileNotFoundError:
        pass

    raise RuntimeError(
        "Neither 'docker compose' nor 'docker-compose' found. "
        "Install Docker with Compose plugin or docker-compose standalone."
    )


def run_compose(
    args: List[str],
    *,
    project_name: str,
    check: bool = True,
    show_output: bool = False,
) -> subprocess.CompletedProcess:
    """Run a docker-compose command against ``docker-compose.test.yml``.

    ``project_name`` is required so each test suite can use an isolated
    compose project.  When ``check`` is True the call raises
    :class:`subprocess.CalledProcessError` on non-zero exit.
    """
    compose_cmd = get_compose_cmd()
    cmd = compose_cmd + ["-f", COMPOSE_FILE, "-p", project_name] + args
    print(f"  Running: {' '.join(cmd)}")

    result = subprocess.run(cmd, capture_output=True, text=True)

    if show_output or result.returncode != 0:
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr)

    if check and result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode, cmd, result.stdout, result.stderr
        )

    return result


def check_container_running(container_name: str, project_name: str) -> bool:
    """Return True if the named compose service has a running container.

    Used by :func:`wait_for_healthy` as a fast-fail when a Ranvier container
    exits mid-startup (e.g. crashes on an invalid config).
    """
    try:
        compose_cmd = get_compose_cmd()
        result = subprocess.run(
            compose_cmd
            + ["-f", COMPOSE_FILE, "-p", project_name, "ps", "-q", container_name],
            capture_output=True,
            text=True,
        )
        return result.returncode == 0 and len(result.stdout.strip()) > 0
    except (FileNotFoundError, RuntimeError) as e:
        # If compose is unavailable for some reason, don't abort the wait
        # loop — let the caller's own timeout decide.
        print(f"    check_container_running: compose unavailable ({e})")
        return True


def wait_for_healthy(
    url: str,
    *,
    timeout: int = 60,
    container_name: Optional[str] = None,
    project_name: Optional[str] = None,
) -> bool:
    """Poll ``url`` until it returns HTTP 200 or ``timeout`` seconds elapse.

    When both ``container_name`` and ``project_name`` are given, the function
    also fast-fails if the container exits during the wait.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if container_name and project_name and not check_container_running(
            container_name, project_name
        ):
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
# Metrics helpers
# =============================================================================


def get_metric_value(
    metrics_url: str,
    metric_name: str,
    *,
    labels: Optional[Dict[str, str]] = None,
    debug: bool = False,
    retries: int = 3,
) -> Optional[float]:
    """Return a single Prometheus metric value from a Ranvier node.

    Two matching modes are supported:

    * **Label matching** (``labels`` given): find the line containing
      ``metric_name`` where every ``k=v`` pair appears as the Prometheus
      quoted label syntax ``k="v"``.  This is the correct, precise mode and
      is used by the new smoke tests.

    * **Substring fallback** (``labels`` is None): return the first line
      whose text contains ``metric_name`` anywhere — including any Seastar
      prefix such as ``seastar_ranvier_``.  This preserves the behaviour of
      the old ``test_cluster.py`` helper where exact names were unknown.

    ``retries`` retries transient HTTP failures with a 1s backoff.
    """
    for attempt in range(retries):
        try:
            resp = requests.get(f"{metrics_url}/metrics", timeout=5)
            if resp.status_code != 200:
                if attempt < retries - 1:
                    time.sleep(1)
                    continue
                return None

            for line in resp.text.split("\n"):
                if line.startswith("#") or not line.strip():
                    continue
                if metric_name not in line:
                    continue
                if labels:
                    if not all(f'{k}="{v}"' in line for k, v in labels.items()):
                        continue
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    if debug:
                        print(f"    Found: {line.strip()}")
                    return float(match.group(1))

            if debug:
                print(f"    Metric {metric_name!r} not found in response")
            return None
        except requests.exceptions.RequestException as e:
            if debug:
                print(f"    Request error (attempt {attempt + 1}): {e}")
            if attempt < retries - 1:
                time.sleep(1)
                continue
            return None
    return None


def get_all_metrics(metrics_url: str) -> Dict[str, List[float]]:
    """Return every metric value exposed by a Ranvier node.

    The result maps metric name to a list of observed values (multiple values
    appear when a metric has per-label or per-shard series).  Comment lines
    and empty lines are ignored.
    """
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return {}

        metrics: Dict[str, List[float]] = {}
        for line in resp.text.split("\n"):
            if line.startswith("#") or not line.strip():
                continue
            match = re.match(
                r"^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{([^}]*)\})?\s+([\d.eE+-]+)",
                line,
            )
            if match:
                name = match.group(1)
                value = float(match.group(3))
                metrics.setdefault(name, []).append(value)
        return metrics
    except requests.exceptions.RequestException as e:
        print(f"    get_all_metrics: request error: {e}")
        return {}


def sum_metric_by_substring(metrics_url: str, substring: str) -> float:
    """Return the sum of every metric line whose name contains ``substring``.

    Useful when a metric is emitted per shard (Seastar exposes one series
    per shard) and the test only cares about the cluster-wide total.
    """
    all_metrics = get_all_metrics(metrics_url)
    total = 0.0
    for name, values in all_metrics.items():
        if substring in name:
            total += sum(values)
    return total


def metric_is_registered(metrics_url: str, substring: str) -> bool:
    """Return True if any metric line contains ``substring``.

    Used to distinguish "counter exists but hasn't incremented" (legit
    assertion failure) from "counter isn't even registered" (the running
    binary predates the feature — likely a stale Docker image). Both cases
    produce a ``0`` from :func:`sum_metric_by_substring`, so tests that
    care about the distinction should consult this helper first.
    """
    all_metrics = get_all_metrics(metrics_url)
    return any(substring in name for name in all_metrics)


def sum_metric_by_labels(
    metrics_url: str,
    metric_substring: str,
    labels: Dict[str, str],
    *,
    retries: int = 3,
) -> float:
    """Sum every metric line that contains ``metric_substring`` AND matches every label.

    Unlike :func:`get_metric_value`, this iterates over every matching line
    and sums them — useful when Seastar emits one series per shard.  The
    substring match is deliberately loose so tests don't have to hard-code
    the Seastar ``seastar_ranvier_`` prefix.

    Returns 0.0 if no matching line exists after ``retries`` attempts.
    """
    for attempt in range(retries):
        try:
            resp = requests.get(f"{metrics_url}/metrics", timeout=5)
            if resp.status_code != 200:
                if attempt < retries - 1:
                    time.sleep(1)
                    continue
                return 0.0

            total = 0.0
            for line in resp.text.split("\n"):
                if line.startswith("#") or not line.strip():
                    continue
                if metric_substring not in line:
                    continue
                if not all(f'{k}="{v}"' in line for k, v in labels.items()):
                    continue
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    total += float(match.group(1))
            return total
        except requests.exceptions.RequestException as e:
            print(f"    sum_metric_by_labels error (attempt {attempt + 1}): {e}")
            if attempt < retries - 1:
                time.sleep(1)
                continue
            return 0.0
    return 0.0


# =============================================================================
# Request helpers
# =============================================================================


def register_backends(api_url: str) -> bool:
    """Register every :data:`BACKENDS` entry on a Ranvier node.

    Returns True on full success, False if any registration failed.  Errors
    are printed with context — this is intentionally chatty because failures
    here usually mean the node came up healthy-enough to serve /metrics but
    not the admin API, which is worth flagging loudly.
    """
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
                print(
                    f"    Failed to register backend {backend_id} on {api_url}: "
                    f"{resp.status_code} {resp.text}"
                )
                return False
        except requests.exceptions.RequestException as e:
            print(f"    Failed to register backend {backend_id} on {api_url}: {e}")
            return False
    return True


def send_chat_request(
    api_url: str,
    messages: List[Dict[str, str]],
    *,
    stream: bool = True,
    timeout: int = REQUEST_TIMEOUT,
    retries: int = 3,
    extra_headers: Optional[Dict[str, str]] = None,
) -> Tuple[int, str, Dict[str, str]]:
    """Send a ``/v1/chat/completions`` request and collect the response.

    Returns ``(status_code, response_text, response_headers)``.
    ``response_text`` is the concatenation of SSE ``delta.content`` chunks
    for streaming responses, or the raw body for non-streaming calls.

    Retries up to ``retries`` times when the backend returns 200 with an
    empty body or when a request raises a :class:`RequestException`.
    """
    request_body = {
        "model": "test-model",
        "messages": messages,
        "stream": stream,
    }
    headers = {"Content-Type": "application/json"}
    if extra_headers:
        headers.update(extra_headers)

    for attempt in range(retries):
        try:
            resp = requests.post(
                f"{api_url}/v1/chat/completions",
                json=request_body,
                headers=headers,
                stream=stream,
                timeout=timeout,
            )

            response_headers = dict(resp.headers)
            response_text = ""

            if stream and resp.status_code == 200:
                for line in resp.iter_lines():
                    if not line:
                        continue
                    decoded = line.decode("utf-8")
                    if decoded.startswith("data: ") and decoded != "data: [DONE]":
                        try:
                            chunk = json.loads(decoded[6:])
                            if chunk.get("choices"):
                                delta = chunk["choices"][0].get("delta", {})
                                if "content" in delta:
                                    response_text += delta["content"]
                        except json.JSONDecodeError:
                            pass
            else:
                response_text = resp.text

            if response_text or resp.status_code != 200:
                return resp.status_code, response_text, response_headers

            if attempt < retries - 1:
                time.sleep(0.5)
                continue
            return resp.status_code, response_text, response_headers

        except requests.exceptions.RequestException as e:
            print(f"    send_chat_request error (attempt {attempt + 1}): {e}")
            if attempt < retries - 1:
                time.sleep(0.5)
                continue
            return 0, str(e), {}

    return 0, "Max retries exceeded", {}


def extract_backend_id(
    response_text: str, headers: Optional[Dict[str, str]] = None
) -> Optional[int]:
    """Extract the mock-backend identifier from a response.

    The mock backend (``tests/integration/mock_backend.py``) sets an
    ``X-Backend-ID`` header on every response and embeds ``Response from
    backend N`` in the body.  We prefer the header because it survives
    intermediate rewriting.
    """
    if headers:
        for k in ("X-Backend-ID", "x-backend-id"):
            v = headers.get(k)
            if v is not None:
                try:
                    return int(v)
                except ValueError:
                    pass

    if response_text:
        match = re.search(r"backend\s+(\d+)", response_text.lower())
        if match:
            return int(match.group(1))

    return None


# =============================================================================
# Cluster lifecycle
# =============================================================================


@dataclass
class ClusterHandle:
    """Value yielded by the ``ranvier_cluster`` fixture.

    Carries the compose project name and the cluster topology.  Tests read
    endpoints via ``handle.nodes["node1"]["api"]`` and container names via
    ``handle.container_names["node1"]``.
    """

    project_name: str
    nodes: Dict[str, Dict[str, str]] = field(default_factory=lambda: NODES)
    backends: Dict[int, Dict[str, object]] = field(default_factory=lambda: BACKENDS)
    container_names: Dict[str, str] = field(default_factory=lambda: CONTAINER_NAMES)


def _bring_up_cluster(project_name: str) -> None:
    """Start the 3-node cluster and wait for every node to serve /metrics.

    Idempotent with respect to previous runs: always issues ``down -v`` first
    so stale volumes from a failed previous session can't leak in.  Honors
    ``SKIP_BUILD=1`` to skip the image build step, and tries a no-build
    ``create`` probe to detect pre-built images automatically.

    Raises :class:`RuntimeError` on any failure so the caller's ``try/finally``
    can tear down partial state.
    """
    print("\n" + "=" * 70)
    print(f"Setting up test cluster (project={project_name})...")
    print("=" * 70)

    run_compose(
        ["down", "-v", "--remove-orphans"],
        project_name=project_name,
        check=False,
    )

    skip_build = os.environ.get("SKIP_BUILD", "").lower() in ("1", "true", "yes")

    if not skip_build:
        try:
            compose_cmd = get_compose_cmd()
            create_result = subprocess.run(
                compose_cmd
                + ["-f", COMPOSE_FILE, "-p", project_name, "create", "--no-build"],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if create_result.returncode == 0:
                print("\nDocker images already exist — skipping build.")
                print("  (Set SKIP_BUILD=0 to force rebuild)")
                skip_build = True
                subprocess.run(
                    compose_cmd
                    + ["-f", COMPOSE_FILE, "-p", project_name, "rm", "-f"],
                    capture_output=True,
                    timeout=30,
                )
        except (subprocess.TimeoutExpired, OSError) as e:
            print(f"  No-build probe failed ({e}); will build.")

    if not skip_build:
        print("\nBuilding containers (this may take a while on first run)...")
        result = run_compose(
            ["build"],
            project_name=project_name,
            check=False,
            show_output=True,
        )
        if result.returncode != 0:
            raise RuntimeError("Failed to build containers")

    print("\nStarting cluster...")
    result = run_compose(
        ["up", "-d"],
        project_name=project_name,
        check=False,
        show_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError("Failed to start cluster")

    print("\nWaiting for nodes to become healthy...")
    all_healthy = True
    for name, endpoints in NODES.items():
        container = CONTAINER_NAMES.get(name)
        print(f"  Waiting for {name}...")
        if not wait_for_healthy(
            f"{endpoints['metrics']}/metrics",
            timeout=STARTUP_TIMEOUT,
            container_name=container,
            project_name=project_name,
        ):
            print(f"  ERROR: {name} did not become healthy")
            all_healthy = False
        else:
            print(f"  {name} is healthy")

    if not all_healthy:
        print("\nContainer logs:")
        run_compose(
            ["logs", "--tail=50"],
            project_name=project_name,
            check=False,
            show_output=True,
        )
        raise RuntimeError("Not all nodes became healthy")

    print("\nWaiting for gossip connections to establish...")
    time.sleep(5)
    print("\nCluster is ready.")
    print("=" * 70 + "\n")


def _tear_down_cluster(project_name: str) -> None:
    """Stop and remove the compose project, swallowing errors.

    Failing to print an error here would be bad (we'd silently leak
    containers) but raising would mask the real test failure, so we log and
    move on.
    """
    print("\n" + "=" * 70)
    print(f"Tearing down test cluster (project={project_name})...")
    print("=" * 70)
    try:
        run_compose(
            ["down", "-v", "--remove-orphans"],
            project_name=project_name,
            check=False,
        )
    except Exception as e:
        print(f"  Teardown error (non-fatal): {e}")
    print("Cleanup complete")


# =============================================================================
# Pytest fixtures
# =============================================================================


if pytest is not None:

    @pytest.fixture(scope="session")
    def ranvier_cluster():
        """Session-scoped fixture: brings up the 3-node cluster once.

        Registers the mock backends on every node before yielding so tests
        can route requests immediately.  Always tears down, even on setup
        failure, via ``try/finally``.
        """
        project_name = PYTEST_PROJECT_NAME
        try:
            _bring_up_cluster(project_name)
        except Exception:
            _tear_down_cluster(project_name)
            raise

        try:
            print("Registering backends on all nodes...")
            for node_name, endpoints in NODES.items():
                if not register_backends(endpoints["api"]):
                    raise RuntimeError(
                        f"Failed to register backends on {node_name}"
                    )
            yield ClusterHandle(project_name=project_name)
        finally:
            _tear_down_cluster(project_name)

    @pytest.fixture
    def cluster_metrics(
        ranvier_cluster: "ClusterHandle",
    ) -> Callable[[str], Dict[str, float]]:
        """Return a snapshot callable: ``(node_name) -> {metric_name: total}``.

        Each call reads the node's /metrics endpoint and returns a fresh
        dict.  Per-shard series are summed into a single cluster-wide
        total, so tests get ``dict[str, float]`` semantics rather than the
        raw ``dict[str, list[float]]`` produced by :func:`get_all_metrics`.

        Tests typically snapshot-before, perform an action, and
        snapshot-after to diff counter values.
        """

        def _snapshot(node_name: str) -> Dict[str, float]:
            if node_name not in ranvier_cluster.nodes:
                raise KeyError(f"Unknown node: {node_name}")
            raw = get_all_metrics(ranvier_cluster.nodes[node_name]["metrics"])
            return {name: sum(values) for name, values in raw.items()}

        return _snapshot


# =============================================================================
# Unittest base class
# =============================================================================


class ClusterTestCase(unittest.TestCase):
    """Base class for unittest-style integration tests.

    Brings up the docker-compose cluster in :meth:`setUpClass` and tears it
    down in :meth:`tearDownClass`, sharing the same helpers as the pytest
    fixture above.  Subclasses may override:

    * ``PROJECT_NAME`` — unique compose project so multiple unittest suites
      can run serially without stale-state leaks.
    * ``AUTO_REGISTER_BACKENDS`` — set to True to register mock backends
      during ``setUpClass``.  Defaults to False so legacy suites whose test
      methods register backends themselves stay functionally identical
      after migration.
    """

    PROJECT_NAME: str = "ranvier-unittest"
    AUTO_REGISTER_BACKENDS: bool = False

    # Re-export constants so subclasses can reach them as class attributes.
    NODES = NODES
    BACKENDS = BACKENDS
    CONTAINER_NAMES = CONTAINER_NAMES

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        try:
            _bring_up_cluster(cls.PROJECT_NAME)
        except Exception:
            _tear_down_cluster(cls.PROJECT_NAME)
            raise

        if cls.AUTO_REGISTER_BACKENDS:
            try:
                print("Registering backends on all nodes...")
                for node_name, endpoints in NODES.items():
                    if not register_backends(endpoints["api"]):
                        raise RuntimeError(
                            f"Failed to register backends on {node_name}"
                        )
            except Exception:
                _tear_down_cluster(cls.PROJECT_NAME)
                raise

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            _tear_down_cluster(cls.PROJECT_NAME)
        finally:
            super().tearDownClass()
