#!/usr/bin/env python3
"""
Native KV-event alignment probe: plumbing test (BACKLOG section 30 Step 5).

Ranvier asks each backend with a live native KV-event stream whether its
POST /tokenize agrees with Ranvier's routing tokens, and under
``kv_events.alignment_probe: quarantine`` (the default) stops trusting a
misaligned backend's stream. The mock backend cannot tokenize like a real
model, so this suite covers the plumbing, not the aligned verdict (that is the
unit tests plus the GPU check, Step 7):

* the mock's ZMQ publisher (``MOCK_KV_EVENTS_PORT``) makes a stream live;
* its /tokenize returns ids no conversation tokenizes to, so the probe reads
  the backend as misaligned at token 0, reports it on /admin/backends/metrics
  and in the ``router_native_*_backends`` gauges, and quarantines it;
* switching /tokenize to 404 and restarting the stream (an AllBlocksCleared)
  re-probes: unsupported, and the quarantine lifts.

Setup: node 1 runs with RANVIER_KV_EVENTS_ENABLED=true and backend1 binds the
publisher on 5557 (docker-compose.test.yml). The test registers its own
backend id at backend1's address with kv_events_port=5557, so no other suite
sees a stream. It skips on an image without the probe or a mock without the
publisher.

Build constraint: requires Docker and a built Ranvier image. Do not run it in
the sandbox; the developer runs it in their Docker environment.

Usage:
    python3 tests/integration/test_kv_alignment_probe.py
"""

import sys
import time
import unittest
from typing import Optional

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    BACKENDS,
    ClusterTestCase,
    DOCKER_HOST,
    MOCK_BACKEND_PORTS,
    NODES,
    metric_is_registered,
    sum_metric_by_substring,
)

PROBE_BACKEND_ID = 41          # a fresh id at backend1's address
KV_EVENTS_PORT = 5557          # docker-compose.test.yml: backend1 MOCK_KV_EVENTS_PORT
VERDICT_TIMEOUT_S = 60         # health cycle 5 s, plus stream sync and backoff
NODE1_API = NODES["node1"]["api"]
NODE1_METRICS = NODES["node1"]["metrics"]
BACKEND1_ADMIN = f"http://{DOCKER_HOST}:{MOCK_BACKEND_PORTS[1]}"


def _mock(path: str) -> requests.Response:
    return requests.post(f"{BACKEND1_ADMIN}{path}", timeout=10)


def _backend_entry(backend_id: int) -> Optional[dict]:
    resp = requests.get(f"{NODE1_API}/admin/backends/metrics", timeout=10)
    resp.raise_for_status()
    for b in resp.json().get("backends", []):
        if b.get("id") == backend_id:
            return b
    return None


def _wait_for(predicate, timeout_s: float, what: str) -> dict:
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        last = _backend_entry(PROBE_BACKEND_ID)
        if last is not None and predicate(last):
            return last
        time.sleep(1.0)
    raise AssertionError(f"timed out after {timeout_s}s waiting for {what}; last entry: {last}")


class KvAlignmentProbeTest(ClusterTestCase):
    PROJECT_NAME = "ranvier-kv-alignment"
    AUTO_REGISTER_BACKENDS = False

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls._registered = False
        # unittest does not call tearDownClass when setUpClass raises (a skip
        # included), so tear the cluster down here before re-raising.
        try:
            cls._set_up_probe_backend()
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def _set_up_probe_backend(cls) -> None:
        if not metric_is_registered(NODE1_METRICS, "router_native_aligned_backends"):
            raise unittest.SkipTest("no alignment probe on node 1 (kv_events off, or an image "
                                    "before BACKLOG section 30)")
        if _mock("/admin/tokenize-mode?mode=ids").status_code != 200:
            raise unittest.SkipTest("mock backend image without /tokenize")
        ip = BACKENDS[1]["ip"]
        port = BACKENDS[1]["port"]
        resp = requests.post(
            f"{NODE1_API}/admin/backends?id={PROBE_BACKEND_ID}&ip={ip}&port={port}"
            f"&kv_events_port={KV_EVENTS_PORT}",
            timeout=10,
        )
        if resp.status_code != 200:
            raise AssertionError(f"registering the probe backend failed: "
                                 f"{resp.status_code} {resp.text}")
        cls._registered = True
        if "unavailable" in resp.text:
            raise unittest.SkipTest("image built without native KV events (WITH_KV_EVENTS=OFF)")
        start = _mock("/admin/kv-events?action=start")
        if start.status_code != 200:
            raise unittest.SkipTest(f"mock KV-event publisher unavailable: {start.text}")

    @classmethod
    def tearDownClass(cls) -> None:
        try:
            if getattr(cls, "_registered", False):
                requests.delete(f"{NODE1_API}/admin/backends?id={PROBE_BACKEND_ID}", timeout=10)
            _mock("/admin/kv-events?action=stop")
            _mock("/admin/tokenize-mode?mode=ids")
        except requests.exceptions.RequestException as e:
            print(f"    teardown: {e}")
        finally:
            super().tearDownClass()

    def test_01_misaligned_backend_is_quarantined(self):
        entry = _wait_for(lambda b: str(b.get("kv_alignment", "")).startswith("misaligned@"),
                          VERDICT_TIMEOUT_S, "a misaligned verdict")
        # The mock's ids are all zero; Ranvier's first probe token is not.
        self.assertEqual(entry["kv_alignment"], "misaligned@0")
        self.assertTrue(entry["kv_quarantined"])
        self.assertGreaterEqual(
            sum_metric_by_substring(NODE1_METRICS, "router_native_misaligned_backends"), 1)
        self.assertGreaterEqual(
            sum_metric_by_substring(NODE1_METRICS, "router_native_quarantined_backends"), 1)
        self.assertGreaterEqual(
            sum_metric_by_substring(NODE1_METRICS,
                                    "router_native_alignment_probes_misaligned_total"), 1)

    def test_02_unsupported_after_restart_lifts_quarantine(self):
        self.assertEqual(_mock("/admin/tokenize-mode?mode=404").status_code, 200)
        # AllBlocksCleared marks the stream complete again: a new epoch, so the
        # probe re-runs, as it would after an engine restart.
        self.assertEqual(_mock("/admin/kv-events?action=clear").status_code, 200)
        entry = _wait_for(lambda b: b.get("kv_alignment") == "unsupported",
                          VERDICT_TIMEOUT_S, "an unsupported verdict")
        self.assertFalse(entry["kv_quarantined"])
        self.assertEqual(
            sum_metric_by_substring(NODE1_METRICS, "router_native_quarantined_backends"), 0)
        self.assertGreaterEqual(
            sum_metric_by_substring(NODE1_METRICS, "router_native_unsupported_backends"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
