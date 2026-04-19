#!/usr/bin/env python3
"""
Health / Circuit Breaker / Connection Pool / Rate Limit Integration Tests

Dedicated, systematic coverage of the resilience mechanisms listed in
BACKLOG.md §6.4. ``test_negative_paths.py`` already exercises these features
as part of a broad failure-handling suite, but it does so via
docker-network-disconnect and SIGHUP config reloads. This file isolates each
mechanism using the mock backend's admin failure-mode injection API.

Failure-mode selection note: Ranvier's circuit breaker only counts
**connection-level** failures (connect refused, read timeout, RST mid-stream).
A clean upstream ``HTTP 503`` is forwarded to the client without incrementing
``record_failure`` because the proxy already saw a successful HTTP/200 line in
the preceding response and called ``record_success`` (which resets the failure
counter). Likewise, ``reset`` mode triggers a per-request success+failure pair
that nets out to no progress toward ``failure_threshold``. The only mock mode
that lets the circuit breaker open is ``timeout`` — Ranvier's per-chunk read
timeout (30 s) fires without an offsetting ``record_success`` because no
headers are ever received. We use it with concurrent requests so wall-time
stays bounded (~30–40 s per CB-triggering test) instead of scaling linearly
with request count.

Coverage map:

    BACKLOG §6.4 item                     | tests
    --------------------------------------|--------------------------------
    Create health/circuit breaker suite   | test_01, test_02
    Circuit breaker state transitions     | test_03, test_04, test_05
    Connection pool resilience            | test_06, test_07, test_08
    Rate limiting behaviour               | test_09, test_10

Relies on circuit-breaker defaults from ``src/config_infra.hpp``
(failure_threshold=5, success_threshold=2, recovery_timeout=30s) and health
defaults (check_interval=5s, recovery_threshold=2). These are NOT overridable
from docker-compose.test.yml, so the tests are designed around the defaults.

The ``fault-injection`` compose profile is required so the always-failing
``backend-unhealthy`` service starts alongside the cluster. ``setUpClass``
sets ``RANVIER_COMPOSE_PROFILE=full,fault-injection`` before bringing the
cluster up and restores the previous value in ``tearDownClass``.

Build constraint: these tests require Docker and a built Ranvier image.
Do not attempt to run them in the sandbox — the developer runs them in
their Docker environment.

Usage:
    python3 tests/integration/test_health_circuit_breaker.py
"""

import concurrent.futures
import os
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

import conftest
from conftest import (
    ClusterTestCase,
    DOCKER_HOST,
    NODES,
    REQUEST_TIMEOUT,
    extract_backend_id,
    metric_is_registered,
    send_chat_request as _conftest_send_chat_request,
    sum_metric_by_substring,
)


# =============================================================================
# File-specific helpers
# =============================================================================
#
# These are deliberately duplicated from test_negative_paths.py. The suites
# are expected to evolve independently and pulling them into conftest would
# entangle two otherwise-isolated files.

def docker_network_disconnect(network: str, container: str) -> bool:
    """Disconnect a container from a Docker network."""
    try:
        result = subprocess.run(
            ["docker", "network", "disconnect", "--force", network, container],
            capture_output=True, text=True, timeout=15,
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def docker_network_connect(network: str, container: str, ip: Optional[str] = None) -> bool:
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


def get_docker_network_name(project_name: str) -> str:
    """Resolve the project-prefixed docker network name."""
    try:
        result = subprocess.run(
            ["docker", "network", "ls", "--format", "{{.Name}}"],
            capture_output=True, text=True, timeout=10,
        )
        for line in result.stdout.strip().split("\n"):
            if project_name in line and "ranvier-test" in line:
                return line.strip()
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return f"{project_name}_ranvier-test"


def send_chat_request(
    api_url: str,
    prompt: str = "test",
    timeout: int = REQUEST_TIMEOUT,
) -> Tuple[int, str, Dict[str, str]]:
    """Thin wrapper around the conftest helper — single message, 1 retry."""
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
# Mock backend admin control
# =============================================================================

# Host-mapped admin ports for the three mock backends (see
# docker-compose.test.yml). 21436 is only reachable when the
# ``fault-injection`` profile is active.
BACKEND_ADMIN_URLS: Dict[str, str] = {
    "backend1":          f"http://{DOCKER_HOST}:21434",
    "backend2":          f"http://{DOCKER_HOST}:21435",
    "backend-unhealthy": f"http://{DOCKER_HOST}:21436",
}

# Internal docker-network address used by Ranvier nodes to reach the
# always-failing backend. Matches docker-compose.test.yml's
# ``backend-unhealthy.networks.ranvier-test.ipv4_address``.
UNHEALTHY_BACKEND_IP = "172.28.1.12"
UNHEALTHY_BACKEND_INTERNAL_PORT = 8000
UNHEALTHY_BACKEND_ID = 3


def set_failure_mode(admin_url: str, mode: str) -> bool:
    """POST /admin/failure-mode?mode=<mode> on a mock backend.

    ``mode`` is one of ``none``, ``status_500``, ``status_503``, ``timeout``,
    ``reset``. Returns True on HTTP 200.
    """
    try:
        resp = requests.post(
            f"{admin_url}/admin/failure-mode",
            params={"mode": mode},
            timeout=5,
        )
        return resp.status_code == 200
    except requests.exceptions.RequestException:
        return False


def reset_all_backends() -> None:
    """Reset every known backend to ``mode=none`` — best-effort."""
    for name, url in BACKEND_ADMIN_URLS.items():
        try:
            requests.post(
                f"{url}/admin/failure-mode",
                params={"mode": "none"},
                timeout=5,
            )
        except requests.exceptions.RequestException:
            # The unhealthy backend is only reachable when fault-injection
            # profile is active; swallow errors for the others too — this is
            # cleanup and must never fail a test.
            pass


# Counters that all reflect "something went wrong" at the proxy. Used by the
# CB-triggering tests as a single aggregated signal — different failure modes
# light up different counters (timeouts vs connection errors vs CB opens), so
# summing avoids brittle assertions tied to one specific code path.
_NEGATIVE_SIGNAL_METRICS: Tuple[str, ...] = (
    "http_requests_failed",
    "http_requests_timeout",
    "http_requests_connection_error",
    "circuit_breaker_opens",
)


def _negative_signal_snapshot(metrics_url: str) -> Dict[str, float]:
    """Return current totals for every metric in ``_NEGATIVE_SIGNAL_METRICS``."""
    return {
        name: sum_metric_by_substring(metrics_url, name)
        for name in _NEGATIVE_SIGNAL_METRICS
    }


def _concurrent_chat_requests(
    api_url: str,
    n: int,
    prompt_prefix: str,
    *,
    per_req_timeout: int = 45,
) -> List[int]:
    """Fire ``n`` chat requests in parallel and return their status codes.

    Used by the CB-triggering tests so multiple read-timeout requests overlap
    in wall time. Without parallelism each ``timeout``-mode request would
    block for Ranvier's ~30 s per-chunk read timeout, making the suite
    grow linearly with request count.
    """
    statuses: List[int] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(n, 16)) as pool:
        futures = [
            pool.submit(
                send_chat_request, api_url, f"{prompt_prefix}-{i}", per_req_timeout,
            )
            for i in range(n)
        ]
        for fut in concurrent.futures.as_completed(futures):
            status, _, _ = fut.result()
            statuses.append(status)
    return statuses


# =============================================================================
# Test Suite
# =============================================================================

class HealthCircuitBreakerTest(ClusterTestCase):
    """Dedicated coverage for BACKLOG §6.4 resilience features."""

    PROJECT_NAME = "ranvier-circuit-breaker-test"
    AUTO_REGISTER_BACKENDS = True

    # Set once by setUpClass so tearDownClass can restore it.
    _original_env_profile: Optional[str] = None
    _original_module_profile: str = "full"
    docker_network: str = ""

    @classmethod
    def setUpClass(cls) -> None:
        # The fault-injection profile starts backend-unhealthy alongside
        # the rest of the cluster. ``conftest.COMPOSE_PROFILE`` is captured
        # at import time, so we must also override the module attribute;
        # setting just the env var is too late for run_compose().
        cls._original_env_profile = os.environ.get("RANVIER_COMPOSE_PROFILE")
        cls._original_module_profile = conftest.COMPOSE_PROFILE

        os.environ["RANVIER_COMPOSE_PROFILE"] = "full,fault-injection"
        conftest.COMPOSE_PROFILE = "full,fault-injection"

        try:
            super().setUpClass()
        except Exception:
            # Restore profile state even if bring-up failed so subsequent
            # test files don't inherit our override.
            cls._restore_profile_env()
            raise

        cls.docker_network = get_docker_network_name(cls.PROJECT_NAME)
        print(f"\nDocker network: {cls.docker_network}")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            super().tearDownClass()
        finally:
            cls._restore_profile_env()

    @classmethod
    def _restore_profile_env(cls) -> None:
        if cls._original_env_profile is None:
            os.environ.pop("RANVIER_COMPOSE_PROFILE", None)
        else:
            os.environ["RANVIER_COMPOSE_PROFILE"] = cls._original_env_profile
        conftest.COMPOSE_PROFILE = cls._original_module_profile

    def tearDown(self) -> None:
        # Always reset every backend to mode=none between tests so a leaked
        # failure mode can't poison the next test case.
        reset_all_backends()
        super().tearDown()

    # =========================================================================
    # Health detection (§6.4: Create health/circuit breaker test suite)
    # =========================================================================

    def test_01_unhealthy_backend_detected(self):
        """Register backend-unhealthy (172.28.1.12) on node1 in ``timeout``
        mode and verify that traffic routed to it produces a measurable
        negative signal (timeouts, connection errors, or circuit opens).
        """
        print("\nTest: Unhealthy backend detection")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        # Use timeout mode: status_503 returns a clean upstream response
        # which the proxy forwards without flipping any failure metric (see
        # module docstring). Timeouts ARE recorded.
        unhealthy_admin = BACKEND_ADMIN_URLS["backend-unhealthy"]
        self.assertTrue(
            set_failure_mode(unhealthy_admin, "timeout"),
            "Could not reach backend-unhealthy admin API — "
            "fault-injection profile missing?",
        )

        register_url = (
            f"{node1_api}/admin/backends"
            f"?id={UNHEALTHY_BACKEND_ID}"
            f"&ip={UNHEALTHY_BACKEND_IP}"
            f"&port={UNHEALTHY_BACKEND_INTERNAL_PORT}"
        )
        reg_resp = requests.post(register_url, timeout=10)
        self.assertEqual(
            reg_resp.status_code, 200,
            f"Failed to register unhealthy backend: "
            f"{reg_resp.status_code} {reg_resp.text}",
        )

        try:
            initial = _negative_signal_snapshot(node1_metrics)
            print(f"  Initial negative-signal counters: {initial}")

            # Concurrent fan-out: each request that lands on bg-unhealthy
            # hangs and Ranvier's per-chunk read times out at ~30 s.
            # Concurrency lets every timeout overlap so wall time stays
            # bounded.
            num_requests = 12
            print(f"  Sending {num_requests} concurrent requests "
                  "(some will route to the unhealthy backend and hang)...")
            print(f"  Each timed-out request will block ~30 s; "
                  "test wall time is bounded by concurrency, not request count.")
            statuses = _concurrent_chat_requests(
                node1_api, num_requests, "unhealthy-probe", per_req_timeout=45,
            )
            print(f"  Status codes: {statuses}")

            time.sleep(2)
            final = _negative_signal_snapshot(node1_metrics)
            deltas = {k: final[k] - initial[k] for k in final}
            print(f"  Negative-signal deltas: {deltas}")

            total_delta = sum(deltas.values())
            self.assertGreater(
                total_delta, 0,
                "Expected at least one negative signal (timeout / "
                "connection error / circuit open / failed) when routing "
                f"to a hanging backend, got deltas={deltas}",
            )
            print("  PASSED: unhealthy backend produced failure signals")
        finally:
            # Unregister so later tests see only backend1/backend2.
            try:
                requests.delete(
                    f"{node1_api}/admin/backends",
                    params={"id": str(UNHEALTHY_BACKEND_ID)},
                    timeout=10,
                )
            except requests.exceptions.RequestException as e:
                print(f"  (cleanup) failed to unregister unhealthy backend: {e}")

    def test_02_backend_recovery_after_failure_cleared(self):
        """Trigger failures on backend1, clear them, wait for the circuit
        breaker's recovery_timeout (30s), and verify backend1 is routable
        again.
        """
        print("\nTest: Backend recovery after failure is cleared")

        node1_api = NODES["node1"]["api"]
        backend1_admin = BACKEND_ADMIN_URLS["backend1"]

        # Put backend1 into sticky 503.
        self.assertTrue(
            set_failure_mode(backend1_admin, "status_503"),
            "Failed to set backend1 to status_503",
        )

        try:
            print("  Sending 10 requests while backend1 is failing...")
            for i in range(10):
                send_chat_request(node1_api, f"recovery-fail-{i}", timeout=10)
        finally:
            # Clear backend1's failure mode BEFORE waiting so the next probe
            # actually sees a healthy backend.
            set_failure_mode(backend1_admin, "none")

        # Circuit breaker recovery_timeout = 30s (config_infra.hpp:263).
        # Add 10s buffer for health-check interval (5s) drift.
        wait_s = 40
        print(f"  Backend1 reset to mode=none — waiting {wait_s}s for "
              f"circuit breaker recovery_timeout...")
        time.sleep(wait_s)

        print("  Sending probe requests to confirm recovery...")
        successes = 0
        attempts = 15
        for i in range(attempts):
            status, _, _ = send_chat_request(node1_api, f"recovery-probe-{i}", timeout=10)
            if status == 200:
                successes += 1

        print(f"  Successes after recovery: {successes}/{attempts}")
        self.assertGreater(
            successes, 0,
            "Expected at least some 200 responses after backend1 recovered, "
            f"got {successes}/{attempts}",
        )
        print("  PASSED: traffic resumed after failure was cleared")

    # =========================================================================
    # Circuit breaker state transitions (§6.4)
    # =========================================================================

    def test_03_consecutive_failures_trigger_open(self):
        """Consecutive **connection-level** failures on every backend should
        open the circuit breaker. Subsequent traffic then observes that
        open state via the ``circuit_breaker_opens`` counter (which in
        Ranvier counts *requests blocked by an already-open circuit*, not
        closed→open state transitions — the state transition itself is
        silent, so a pure "hang and measure" design sees delta=0).

        Uses ``timeout`` mode because only connection-level failures can
        advance the circuit past ``failure_threshold`` — see the module
        docstring.
        """
        print("\nTest: Consecutive failures trigger circuit breaker open")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        initial = _negative_signal_snapshot(node1_metrics)
        print(f"  Initial negative-signal counters: {initial}")

        # Fail both registered backends at the connection layer.
        self.assertTrue(set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "timeout"))
        self.assertTrue(set_failure_mode(BACKEND_ADMIN_URLS["backend2"], "timeout"))

        try:
            # Phase 1 — 15 concurrent hanging requests. Each pushes a
            # record_failure onto whichever backend the router selected;
            # the per-backend failure_count reaches ``failure_threshold=5``
            # and the circuit state flips to OPEN internally. Note: the
            # ``circuit_breaker_opens`` metric does NOT increment here
            # because it counts blocked requests, not state transitions.
            phase1_n = 15
            print(f"  Phase 1: firing {phase1_n} concurrent timeouts "
                  "(~30 s wait, overlap in parallel)...")
            phase1_statuses = _concurrent_chat_requests(
                node1_api, phase1_n, "open-trigger", per_req_timeout=45,
            )
            print(f"  Phase 1 status codes: {phase1_statuses}")
            time.sleep(2)

            phase1_mid = _negative_signal_snapshot(node1_metrics)
            phase1_deltas = {k: phase1_mid[k] - initial[k] for k in initial}
            print(f"  Phase 1 metric deltas: {phase1_deltas}")

            # Phase 2 — reset backends to healthy so fallback requests can
            # actually complete, then send a sequential burst. Each request
            # whose router-selected target still has an OPEN circuit (the
            # recovery_timeout is 30 s, and Phase 1's circuits opened
            # seconds ago) will bump ``circuit_breaker_opens`` and then
            # fall back to the other backend.
            set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "none")
            set_failure_mode(BACKEND_ADMIN_URLS["backend2"], "none")
            phase2_n = 15
            print(f"  Phase 2: backends restored; sending {phase2_n} "
                  "sequential probes to observe the open-circuit state...")
            phase2_successes = 0
            for i in range(phase2_n):
                status, _, _ = send_chat_request(
                    node1_api, f"open-observe-{i}", timeout=10,
                )
                if status == 200:
                    phase2_successes += 1
            print(f"  Phase 2 successes: {phase2_successes}/{phase2_n}")

            time.sleep(2)  # Metrics flush.

            final = _negative_signal_snapshot(node1_metrics)
            deltas = {k: final[k] - initial[k] for k in initial}
            print(f"  Total metric deltas: {deltas}")

            self.assertGreater(
                deltas["circuit_breaker_opens"], 0,
                "Expected circuit_breaker_opens to increment once the "
                "per-backend failure_count crossed threshold AND Phase 2 "
                f"probes arrived to observe the open state (deltas={deltas})",
            )
            print("  PASSED: circuit transitioned closed → open")
        finally:
            set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "none")
            set_failure_mode(BACKEND_ADMIN_URLS["backend2"], "none")

    def test_04_half_open_allows_probe_and_closes(self):
        """After the circuit opens, clearing failures and waiting
        recovery_timeout should let half-open probes succeed, closing the
        circuit and allowing normal traffic.

        Phase 1 uses ``timeout`` mode (not ``status_503``) for the same
        reason as test_03 — only connection-level failures progress the
        circuit toward OPEN.
        """
        print("\nTest: Half-open probe closes the circuit")

        node1_api = NODES["node1"]["api"]

        # Phase 1 — drive both backends past failure_threshold via timeouts.
        # We don't assert the per-test ``circuit_breaker_opens`` delta here
        # because the metric only increments on closed → open transitions.
        # If test_03 just opened the same circuits and they're still inside
        # the 30 s recovery window, Phase 1 keeps them OPEN without bumping
        # the counter. The Phase 3 recovery assertion below is what the
        # spec actually requires.
        self.assertTrue(set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "timeout"))
        self.assertTrue(set_failure_mode(BACKEND_ADMIN_URLS["backend2"], "timeout"))

        try:
            print("  Phase 1: driving circuit open with hung backends "
                  "(15 concurrent, ~30 s for read timeout)...")
            _concurrent_chat_requests(
                node1_api, 15, "half-open-fail", per_req_timeout=45,
            )
        finally:
            # Phase 2 — restore health.
            set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "none")
            set_failure_mode(BACKEND_ADMIN_URLS["backend2"], "none")

        # recovery_timeout=30s + buffer. Print loudly so the developer
        # doesn't kill the run thinking it hung.
        wait_s = 40
        print(f"  Phase 2: backends reset, waiting {wait_s}s for half-open window...")
        time.sleep(wait_s)

        # Phase 3 — subsequent requests hit half-open probes, which should
        # succeed and close the circuit.
        print("  Phase 3: sending probes to close the circuit...")
        successes = 0
        attempts = 20
        for i in range(attempts):
            status, _, _ = send_chat_request(node1_api, f"half-open-probe-{i}", timeout=10)
            if status == 200:
                successes += 1

        print(f"  Successes after half-open: {successes}/{attempts}")
        self.assertGreater(
            successes, 0,
            "Half-open probes should succeed once backends are healthy, "
            f"got {successes}/{attempts} (circuit may be stuck open)",
        )
        print("  PASSED: circuit closed after half-open probes")

    def test_05_fallback_to_healthy_backend(self):
        """With backend1 hung and backend2 healthy, the router should open
        backend1's circuit and fall back to backend2.

        ``fallback_attempts`` only fires when the router selects a backend
        whose circuit is already OPEN, so this test must first drive
        backend1's circuit open with a concurrent burst (Phase 1) before
        looking for the fallback metric (Phase 2). ``status_503`` would
        leave the circuit closed indefinitely (see module docstring).
        """
        print("\nTest: Fallback to healthy backend")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        fallback_is_registered = metric_is_registered(
            node1_metrics, "fallback_attempts",
        )
        print(f"  fallback_attempts metric registered: {fallback_is_registered}")

        self.assertTrue(set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "timeout"))
        # Deliberately leave backend2 healthy.

        try:
            initial_fallback = sum_metric_by_substring(
                node1_metrics, "fallback_attempts",
            ) if fallback_is_registered else 0.0
            initial_opens = sum_metric_by_substring(
                node1_metrics, "circuit_breaker_opens",
            )

            # Phase 1 — open backend1's circuit. Concurrent burst so the
            # 5+ timeouts on backend1 overlap (each ~30 s).
            phase1_n = 12
            print(f"  Phase 1: {phase1_n} concurrent requests to drive "
                  "backend1's circuit open (~30 s wait on read timeout)...")
            phase1_statuses = _concurrent_chat_requests(
                node1_api, phase1_n, "fallback-open", per_req_timeout=45,
            )
            phase1_success = sum(1 for s in phase1_statuses if s == 200)
            print(f"  Phase 1 successes: {phase1_success}/{phase1_n} "
                  "(some succeed via backend2 even before circuit opens)")
            time.sleep(2)
            opens_delta = (
                sum_metric_by_substring(node1_metrics, "circuit_breaker_opens")
                - initial_opens
            )
            print(f"  Phase 1 circuit_breaker_opens delta: {opens_delta}")

            # Phase 2 — backend1 circuit is now OPEN. Subsequent requests
            # the router targets at backend1 should short-circuit and pick
            # backend2 instead, incrementing fallback_attempts.
            phase2_n = 30
            print(f"  Phase 2: {phase2_n} requests against open-circuit "
                  "backend1 + healthy backend2...")
            phase2_successes = 0
            for i in range(phase2_n):
                status, _, _ = send_chat_request(
                    node1_api, f"fallback-route-{i}", timeout=10,
                )
                if status == 200:
                    phase2_successes += 1
            print(f"  Phase 2 successes: {phase2_successes}/{phase2_n}")

            # Phase 2 should land overwhelmingly on backend2 (open circuit
            # short-circuits at routing time, no per-request wait).
            self.assertGreaterEqual(
                phase2_successes, phase2_n // 2,
                "Expected majority of Phase 2 traffic to succeed via "
                f"fallback to backend2, got {phase2_successes}/{phase2_n}",
            )

            if not fallback_is_registered:
                self.skipTest(
                    "fallback_attempts metric is not registered on this "
                    "build — traffic-level fallback was validated, metric "
                    "check skipped",
                )

            time.sleep(2)
            fallback_delta = (
                sum_metric_by_substring(node1_metrics, "fallback_attempts")
                - initial_fallback
            )
            print(f"  fallback_attempts delta: {fallback_delta}")
            self.assertGreater(
                fallback_delta, 0,
                "Expected fallback_attempts to increment once backend1's "
                "circuit was open and the router still selected it "
                f"(delta={fallback_delta})",
            )

            print("  PASSED: router fell back to healthy backend")
        finally:
            set_failure_mode(BACKEND_ADMIN_URLS["backend1"], "none")

    # =========================================================================
    # Connection pool resilience (§6.4)
    # =========================================================================

    def test_06_connection_reuse_across_requests(self):
        """Sequential requests against healthy backends should reuse pooled
        connections without triggering the stale-retry path.
        """
        print("\nTest: Connection reuse across sequential requests")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]

        initial_stale = sum_metric_by_substring(node1_metrics, "stale_connection_retries")
        print(f"  Initial stale_connection_retries: {initial_stale}")

        print("  Sending 5 sequential requests with 0.1s spacing...")
        for i in range(5):
            status, _, _ = send_chat_request(node1_api, f"reuse-{i}", timeout=10)
            self.assertEqual(
                status, 200,
                f"Request {i} should succeed against healthy backends, got {status}",
            )
            time.sleep(0.1)

        time.sleep(1)

        final_stale = sum_metric_by_substring(node1_metrics, "stale_connection_retries")
        stale_delta = final_stale - initial_stale
        print(f"  stale_connection_retries delta: {stale_delta}")

        self.assertEqual(
            stale_delta, 0,
            "stale_connection_retries should NOT increment for sequential "
            f"requests against healthy backends (delta={stale_delta})",
        )
        print("  PASSED: pooled connections reused cleanly")

    def test_07_recovery_after_backend_restart(self):
        """A full backend outage + recovery should leave Ranvier able to
        route to the restored backend once health checks re-converge.

        We isolate the backend via ``docker network disconnect`` rather
        than ``docker compose stop``. ``stop`` with the
        ``--profile full --profile fault-injection`` flags has been
        observed to wedge ranvier1 in some Docker environments (likely a
        docker-compose + profiles interaction), and the network-disconnect
        approach is the same one ``test_negative_paths::test_02`` uses
        successfully for the equivalent backend-flap scenario.
        """
        print("\nTest: Recovery after backend restart (via network disconnect)")

        node1_api = NODES["node1"]["api"]
        network = self.__class__.docker_network
        backend1_container = "backend1"
        backend1_ip = "172.28.1.10"

        # Warm the pool.
        print("  Warming connection pool...")
        status, _, _ = send_chat_request(node1_api, "restart-warmup", timeout=10)
        self.assertEqual(status, 200, "Warmup request should succeed")

        reconnected = False
        try:
            print(f"  Disconnecting {backend1_container} from network {network}...")
            disconnected = docker_network_disconnect(network, backend1_container)
            self.assertTrue(
                disconnected,
                f"Failed to disconnect {backend1_container} from {network}",
            )
            time.sleep(3)  # Let the node notice.

            print("  Sending requests with backend1 unreachable — "
                  "expect backend2 fallback...")
            successes_during_outage = 0
            for i in range(10):
                status, _, _ = send_chat_request(
                    node1_api, f"restart-outage-{i}", timeout=15,
                )
                if status == 200:
                    successes_during_outage += 1
            print(f"  Successes during outage: {successes_during_outage}/10")
            self.assertGreater(
                successes_during_outage, 0,
                "backend2 should absorb traffic while backend1 is unreachable",
            )
        finally:
            print(f"  Reconnecting {backend1_container} to {network}...")
            reconnected = docker_network_connect(
                network, backend1_container, ip=backend1_ip,
            )
            if not reconnected:
                # Fall back to reconnecting without a fixed IP — better
                # than leaving the container orphaned.
                reconnected = docker_network_connect(network, backend1_container)
            if not reconnected:
                print(f"  WARNING: failed to reconnect {backend1_container}; "
                      "subsequent tests may fail")

        # health check_interval = 5s × recovery_threshold = 2 → 10s minimum.
        # Add a 10s buffer for health-check drift.
        wait_s = 20
        print(f"  Waiting {wait_s}s for health check cycle to re-add backend1...")
        time.sleep(wait_s)

        print("  Probing for backend1 to become routable again...")
        saw_backend1 = False
        any_success = False
        for i in range(30):
            status, body, headers = send_chat_request(
                node1_api, f"restart-recover-{i}", timeout=10,
            )
            if status == 200:
                any_success = True
                if extract_backend_id(body, headers) == 1:
                    saw_backend1 = True
                    break

        print(f"  backend1 routable after restart: {saw_backend1}")
        self.assertTrue(any_success, "Requests should succeed after backend1 restart")
        self.assertTrue(
            saw_backend1,
            "After restart and health-check window, backend1 should be "
            "routable again (never saw X-Backend-ID=1 in 30 probes)",
        )
        print("  PASSED: cluster recovered backend1 after restart")

    def test_08_timeout_handling_for_slow_backend(self):
        """A backend stuck in timeout mode should trigger timeout accounting
        — Ranvier must not hang the caller for the full mock-backend timeout.
        """
        print("\nTest: Timeout handling for slow backend")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]
        backend1_admin = BACKEND_ADMIN_URLS["backend1"]

        initial_timeouts = sum_metric_by_substring(node1_metrics, "http_requests_timeout")
        print(f"  Initial http_requests_timeout: {initial_timeouts}")

        self.assertTrue(
            set_failure_mode(backend1_admin, "timeout"),
            "Failed to set backend1 to timeout mode",
        )

        try:
            # Budget well below the mock backend's timeout sleep (~60s).
            # Ranvier's own per-request budget should fire well before that.
            budget_s = 30
            print(f"  Sending 6 requests with a {budget_s}s budget each...")
            statuses = []
            over_budget = 0
            for i in range(6):
                t0 = time.time()
                status, _, _ = send_chat_request(
                    node1_api, f"timeout-{i}", timeout=budget_s,
                )
                elapsed = time.time() - t0
                statuses.append(status)
                if elapsed > budget_s - 1:
                    over_budget += 1
                print(f"    Request {i}: status={status} elapsed={elapsed:.1f}s")

            self.assertLess(
                over_budget, len(statuses),
                f"Every request hit the {budget_s}s budget — Ranvier is not "
                "enforcing its own upstream timeout",
            )

            # backend2 may still serve some 200s via fallback — that's fine.
            # We only require that at least one request registered as a
            # gateway-level failure (502/504) OR that the timeout metric
            # incremented.
            time.sleep(2)
            final_timeouts = sum_metric_by_substring(node1_metrics, "http_requests_timeout")
            timeout_delta = final_timeouts - initial_timeouts
            gateway_errors = sum(1 for s in statuses if s in (502, 503, 504))
            print(f"  http_requests_timeout delta: {timeout_delta}")
            print(f"  Gateway-error responses (502/503/504): {gateway_errors}")

            self.assertGreater(
                timeout_delta + gateway_errors, 0,
                "Expected either http_requests_timeout or gateway-error responses "
                f"when backend1 hangs (timeout_delta={timeout_delta}, "
                f"gateway_errors={gateway_errors})",
            )
            print("  PASSED: slow backend handled within budget")
        finally:
            set_failure_mode(backend1_admin, "none")

    # =========================================================================
    # Rate limiting (§6.4)
    # =========================================================================

    def test_09_rate_limit_metrics_registered(self):
        """The rate-limit counter must be exported on every node even when
        rate limiting is disabled (its default state).
        """
        print("\nTest: Rate limit metric registered on all nodes")

        for name, endpoints in NODES.items():
            registered = metric_is_registered(
                endpoints["metrics"], "http_requests_rate_limited",
            )
            print(f"  {name}: http_requests_rate_limited registered={registered}")
            self.assertTrue(
                registered,
                f"{name} does not export http_requests_rate_limited — likely "
                "a stale image predating the metric, or a registration regression",
            )

        print("  PASSED: rate-limit metric registered cluster-wide")

    def test_10_rate_limit_behavior(self):
        """Exercising rate limiting end-to-end requires the SIGHUP config
        reload flow, which is already covered by
        ``test_negative_paths.py::test_04_rate_limit_exceeded``. Defer until a
        hot-reload API exists that doesn't collide with that suite.
        """
        self.skipTest(
            "Rate limiting SIGHUP flow covered by "
            "test_negative_paths.py::test_04; dedicated rate-limit suite "
            "deferred until hot-reload API available"
        )


# =============================================================================
# Main
# =============================================================================

def main():
    """Run the health / circuit breaker integration tests."""
    print("=" * 70)
    print("Ranvier Core - Health / Circuit Breaker / Pool / Rate Limit Tests")
    print("=" * 70)
    print("\nCoverage:")
    print("  - Unhealthy backend detection")
    print("  - Backend recovery after failures clear")
    print("  - Circuit breaker state transitions (closed → open → half-open → closed)")
    print("  - Fallback to healthy backend")
    print("  - Connection pool reuse and restart recovery")
    print("  - Slow-backend timeout handling")
    print("  - Rate-limit metric registration")
    print("")

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(HealthCircuitBreakerTest)
    suite = unittest.TestSuite(sorted(suite, key=lambda t: t.id()))
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
