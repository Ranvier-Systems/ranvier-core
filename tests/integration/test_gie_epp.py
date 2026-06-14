"""Integration test for the GIE Endpoint-Picker (EPP) ext_proc gRPC server.

Drives a real, running EPP-enabled Ranvier (WITH_GIE_EPP=ON, gie_epp.enabled)
via a gRPC ext_proc client and asserts the picker's behaviour end-to-end:

  * no backend registered           -> ImmediateResponse 503
  * backend registered + prompt     -> x-gateway-destination-endpoint header
                                       (+ matching dynamic_metadata) == backend
  * bodyless request (headers EOS)  -> still routed (load/hash path)

This closes the gap left by the unit/CI coverage, which only exercises the pure
decision helper and the compile/link of the gRPC TU — nothing actually drove
the server. Uses docker-compose.epp-test.yml (a standalone EPP node built from
the Dockerfile.gie-epp builder stage + a mock backend).

Build constraint: requires Docker + a WITH_GIE_EPP=ON build and the Python
gRPC deps (tests/integration/requirements.txt: grpcio, grpcio-tools). Skipped
automatically when those are absent. The developer runs this in Docker.
"""

from __future__ import annotations

import os
import subprocess
import time

import pytest
import requests

# Skip the whole module cleanly if the gRPC Python tooling isn't installed.
pytest.importorskip("grpc")
pytest.importorskip("grpc_tools")

from conftest import get_compose_cmd, get_docker_host, wait_for_healthy  # noqa: E402
import epp_client  # noqa: E402

# ---------------------------------------------------------------------------
# Topology (mirrors docker-compose.epp-test.yml)
# ---------------------------------------------------------------------------
EPP_COMPOSE_FILE = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "docker-compose.epp-test.yml")
)
EPP_PROJECT = "ranvier-epp-test"

DOCKER_HOST = get_docker_host()
EPP_TARGET = f"{DOCKER_HOST}:9002"               # ext_proc gRPC
EPP_API = f"http://{DOCKER_HOST}:8091"           # admin/data HTTP
EPP_METRICS = f"http://{DOCKER_HOST}:9191"       # Prometheus

# The backend's in-network address — what the picker returns (a gateway would
# forward there). Matches epp-backend's ipv4_address:port in the compose file.
BACKEND_ID = 1
BACKEND_ENDPOINT = "172.29.1.10:8000"

STARTUP_TIMEOUT = 120
REGISTER_POLL_SECONDS = 20


def _epp_compose(args, *, check=True):
    cmd = get_compose_cmd() + ["-f", EPP_COMPOSE_FILE, "-p", EPP_PROJECT] + args
    print(f"  Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr)
        if check:
            raise subprocess.CalledProcessError(result.returncode, cmd)
    return result


@pytest.fixture(scope="module")
def epp_node():
    """Bring up the standalone EPP node + mock backend; yield once, then tear down."""
    _epp_compose(["down", "-v", "--remove-orphans"], check=False)
    _epp_compose(["up", "-d", "--build"], check=True)
    try:
        # Poll the metrics URL only. (Don't pass container_name to
        # wait_for_healthy: its container-running fast-fail runs `compose ps`
        # against conftest's COMPOSE_FILE — the main cluster compose — which has
        # no ranvier-epp service, so it would always report "exited".)
        if not wait_for_healthy(f"{EPP_METRICS}/metrics", timeout=STARTUP_TIMEOUT):
            _epp_compose(["logs", "--tail=80"], check=False)
            raise RuntimeError("EPP node did not become healthy")
        yield
    finally:
        _epp_compose(["down", "-v", "--remove-orphans"], check=False)


def _register_backend() -> None:
    """Register the mock backend on the EPP node (idempotent)."""
    ip, port = BACKEND_ENDPOINT.split(":")
    resp = requests.post(
        f"{EPP_API}/admin/backends?id={BACKEND_ID}&ip={ip}&port={port}", timeout=10
    )
    assert resp.status_code == 200, f"register failed: {resp.status_code} {resp.text}"


def _pick_until_endpoint(*, send_body: bool = True):
    """Poll pick_endpoint until it returns an endpoint (registration propagates
    across shards asynchronously) or the budget elapses."""
    deadline = time.time() + REGISTER_POLL_SECONDS
    last = None
    while time.time() < deadline:
        last = epp_client.pick_endpoint(EPP_TARGET, send_body=send_body)
        if last.has_endpoint:
            return last
        time.sleep(1)
    return last


def test_gie_epp_endpoint_picker(epp_node):
    """End-to-end EPP scenario on one node: 503 → register → routed → bodyless."""

    # 1. No backend registered yet -> the picker sheds with ImmediateResponse 503.
    empty = epp_client.pick_endpoint(EPP_TARGET)
    assert not empty.has_endpoint, f"unexpected endpoint with no backends: {empty}"
    assert empty.immediate_status == 503, (
        f"expected ImmediateResponse 503 with no backends, got {empty.immediate_status} "
        f"(raw_responses={empty.raw_responses})"
    )

    # 2. Register a backend; the picker should now return it via the header.
    _register_backend()
    picked = _pick_until_endpoint(send_body=True)
    assert picked is not None and picked.has_endpoint, (
        f"EPP did not return an endpoint after registration: {picked}"
    )
    assert picked.endpoint == BACKEND_ENDPOINT, (
        f"expected {BACKEND_ENDPOINT}, got {picked.endpoint}"
    )

    # 3. dynamic_metadata must carry the SAME endpoint (GIE spec: no disagreement).
    assert picked.metadata_endpoint == picked.endpoint, (
        f"dynamic_metadata endpoint {picked.metadata_endpoint!r} != header "
        f"{picked.endpoint!r}"
    )

    # 4. A bodyless request (headers end_of_stream) still routes (load/hash path).
    bodyless = _pick_until_endpoint(send_body=False)
    assert bodyless is not None and bodyless.has_endpoint, (
        f"bodyless request was not routed: {bodyless}"
    )
    assert bodyless.endpoint == BACKEND_ENDPOINT, (
        f"bodyless expected {BACKEND_ENDPOINT}, got {bodyless.endpoint}"
    )
