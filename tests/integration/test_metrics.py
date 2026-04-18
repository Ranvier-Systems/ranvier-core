#!/usr/bin/env python3
"""Metrics exposition integration tests for Ranvier Core.

Covers two BACKLOG items (§6.5 Observability Tests):
- "Create metrics test suite" — Prometheus format, core counter/histogram/gauge
  registration, request-count increment, latency-histogram recording,
  backend-health gauges.
- "Test cluster metrics" — cluster_peers_alive reflects topology, gossip
  counters increment, per-shard metrics are exposed.

This suite exercises the ``/metrics`` endpoint rather than functional routing:
it's a regression gate against a broken or reduced-scope exporter (Seastar
prefixes ``ranvier`` metrics at scrape time as ``seastar_ranvier_*``).
"""

from __future__ import annotations

import re
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
    ClusterTestCase,
    NODES,
    get_all_metrics,
    metric_is_registered,
    run_compose,
    send_chat_request,
    sum_metric_by_substring,
)


# Prometheus line regex: metric_name{labels}? value.
# Applied after stripping ``# HELP``/``# TYPE`` comments and blank lines.
_PROM_LINE_RE = re.compile(
    r"^[a-zA-Z_:][a-zA-Z0-9_:]*(\{.*\})?\s+[\d.eE+-]+$"
)

# Projects that hold 172.28.0.0/16 — tear them down before bringing up this
# class so a prior suite (pytest session or another unittest class) can't
# keep the subnet pinned.  Mirrors test_streaming.py / test_http_pipeline.py.
_SUBNET_HOLDING_PROJECTS = (
    "ranvier-pytest-session",
    "ranvier-integration-test",
    "ranvier-http-pipeline-test",
    "ranvier-http-pipeline-nobackend-test",
    "ranvier-http-pipeline-tokenfwd-test",
    "ranvier-streaming-test",
    "ranvier-metrics-test",
)


def _free_docker_subnet():
    for project in _SUBNET_HOLDING_PROJECTS:
        run_compose(
            ["down", "-v", "--remove-orphans"],
            project_name=project,
            check=False,
        )


def _fetch_raw_metrics(metrics_url: str) -> str:
    """Return the raw ``/metrics`` text, asserting a successful scrape."""
    resp = requests.get(f"{metrics_url}/metrics", timeout=5)
    resp.raise_for_status()
    return resp.text


class MetricsTest(ClusterTestCase):
    """Metrics exposition and cluster-metric correctness."""

    PROJECT_NAME = "ranvier-metrics-test"
    AUTO_REGISTER_BACKENDS = True

    @classmethod
    def setUpClass(cls):
        _free_docker_subnet()
        super().setUpClass()

    # ------------------------------------------------------------------
    # Prometheus format validation
    # ------------------------------------------------------------------

    def test_01_metrics_endpoint_returns_valid_prometheus(self):
        """Every node's /metrics responds 200, text/plain, and parses cleanly."""
        print("\nTest: /metrics returns valid Prometheus format on every node")
        for name, endpoints in NODES.items():
            resp = requests.get(f"{endpoints['metrics']}/metrics", timeout=5)
            self.assertEqual(resp.status_code, 200, f"{name}: HTTP {resp.status_code}")

            content_type = resp.headers.get("Content-Type", "")
            self.assertIn(
                "text/plain", content_type,
                f"{name}: Content-Type {content_type!r} missing 'text/plain'",
            )

            metric_names = set()
            for lineno, line in enumerate(resp.text.split("\n"), start=1):
                if not line.strip():
                    continue
                if line.startswith("# HELP ") or line.startswith("# TYPE "):
                    continue
                if line.startswith("#"):
                    # Other comments are uncommon but harmless.
                    continue
                self.assertRegex(
                    line, _PROM_LINE_RE,
                    f"{name} line {lineno} not valid Prometheus: {line!r}",
                )
                metric_names.add(line.split("{", 1)[0].split(" ", 1)[0])

            self.assertGreaterEqual(
                len(metric_names), 20,
                f"{name}: only {len(metric_names)} unique metric names — exporter looks stubbed",
            )
            print(f"  {name}: {len(metric_names)} unique metric names")

    def test_02_core_metrics_registered(self):
        """Regression gate: every documented core metric is exported somewhere."""
        print("\nTest: core metrics are registered")
        expected = [
            "http_requests_total",
            "http_requests_success",
            "http_requests_failed",
            "http_request_duration_seconds",
            "active_proxy_requests",
            "backend_connect_duration_seconds",
            "backend_response_duration_seconds",
            "router_routing_latency_seconds",
            "cache_hit_ratio",
            "circuit_breaker_opens",
            "cluster_peers_alive",
        ]
        # Pick any node — each should register the same set.  node1 is
        # representative and is the node the later tests drive traffic to.
        metrics_url = NODES["node1"]["metrics"]
        missing = [
            name for name in expected
            if not metric_is_registered(metrics_url, name)
        ]
        self.assertFalse(
            missing,
            f"node1 is missing expected metrics: {missing}. "
            "Likely a stale Docker image or a dropped registration.",
        )
        print(f"  All {len(expected)} core metrics present on node1")

    # ------------------------------------------------------------------
    # Request count increments
    # ------------------------------------------------------------------

    def test_03_request_count_increments(self):
        """Sending chat requests increments http_requests_total on that node."""
        print("\nTest: http_requests_total increments per request")
        metrics_url = NODES["node1"]["metrics"]
        api_url = NODES["node1"]["api"]

        before = sum_metric_by_substring(metrics_url, "http_requests_total")
        print(f"  before: http_requests_total = {before}")

        num_requests = 3
        for i in range(num_requests):
            status, _body, _headers = send_chat_request(
                api_url,
                [{"role": "user", "content": f"metrics probe {i}"}],
            )
            self.assertEqual(status, 200, f"request {i}: HTTP {status}")

        # Allow a brief moment for the counter to publish.  Seastar metrics
        # are shard-local uint64s, so the scrape reads the current value —
        # but a tiny sleep removes any race with the last response close.
        time.sleep(0.2)

        after = sum_metric_by_substring(metrics_url, "http_requests_total")
        print(f"  after:  http_requests_total = {after}")

        delta = after - before
        self.assertGreaterEqual(
            delta, num_requests,
            f"Expected delta >= {num_requests}, got {delta} "
            f"(before={before}, after={after})",
        )

    # ------------------------------------------------------------------
    # Latency histograms
    # ------------------------------------------------------------------

    def test_04_latency_histograms_recorded(self):
        """Histogram observations recorded after test_03's traffic."""
        print("\nTest: latency histograms recorded (_bucket / _count / _sum)")
        raw = _fetch_raw_metrics(NODES["node1"]["metrics"])

        histograms = [
            "http_request_duration_seconds",
            "backend_response_duration_seconds",
            "router_routing_latency_seconds",
        ]

        for hist in histograms:
            bucket_found = False
            count_value = None
            sum_value = None

            for line in raw.split("\n"):
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                if hist not in stripped:
                    continue

                # Histogram buckets carry the ``le="..."`` label.
                if f"{hist}_bucket" in stripped and 'le="' in stripped:
                    bucket_found = True
                elif f"{hist}_count" in stripped:
                    match = re.search(
                        r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", stripped,
                    )
                    if match:
                        value = float(match.group(1))
                        count_value = (count_value or 0.0) + value
                elif f"{hist}_sum" in stripped:
                    match = re.search(
                        r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", stripped,
                    )
                    if match:
                        value = float(match.group(1))
                        sum_value = (sum_value or 0.0) + value

            self.assertTrue(
                bucket_found,
                f"{hist}: no _bucket line with le= label found",
            )
            self.assertIsNotNone(
                count_value, f"{hist}: no _count line found",
            )
            self.assertGreaterEqual(
                count_value, 1.0,
                f"{hist}: _count summed to {count_value}, expected >= 1",
            )
            self.assertIsNotNone(
                sum_value, f"{hist}: no _sum line found",
            )
            self.assertGreater(
                sum_value, 0.0,
                f"{hist}: _sum summed to {sum_value}, expected > 0",
            )
            print(
                f"  {hist}: count={count_value}, sum={sum_value:.6f}",
            )

    # ------------------------------------------------------------------
    # Backend health metrics
    # ------------------------------------------------------------------

    def test_05_backend_health_metrics(self):
        """active_proxy_requests present everywhere; backend_active_requests labels match."""
        print("\nTest: backend health metrics")
        for name, endpoints in NODES.items():
            self.assertTrue(
                metric_is_registered(endpoints["metrics"], "active_proxy_requests"),
                f"{name}: active_proxy_requests gauge missing",
            )
        print(f"  active_proxy_requests present on all {len(NODES)} nodes")

        metrics_url = NODES["node1"]["metrics"]
        if not metric_is_registered(metrics_url, "backend_active_requests"):
            self.skipTest(
                "backend_active_requests not registered — running against an "
                "older binary that predates per-backend gauges."
            )

        raw = _fetch_raw_metrics(metrics_url)
        registered_ids = set()
        for line in raw.split("\n"):
            if "backend_active_requests" not in line:
                continue
            match = re.search(r'backend_id="(\d+)"', line)
            if match:
                registered_ids.add(int(match.group(1)))

        expected_ids = set(BACKENDS.keys())
        self.assertTrue(
            expected_ids.issubset(registered_ids),
            f"backend_active_requests missing backend_id labels. "
            f"expected={sorted(expected_ids)}, got={sorted(registered_ids)}",
        )
        print(
            f"  backend_active_requests exposed for backend_ids={sorted(registered_ids)}"
        )

    # ------------------------------------------------------------------
    # Cluster peer gauge
    # ------------------------------------------------------------------

    def test_06_cluster_peers_alive_reflects_topology(self):
        """Every node in the 3-node cluster reports exactly 2 live peers."""
        print("\nTest: cluster_peers_alive reflects 3-node topology")
        # In a 3-node cluster, each node has 2 peers.  Gossip stabilises in
        # the ``time.sleep(5)`` inside ``_bring_up_cluster``, so no extra
        # sleep here — the harness has already waited.
        for name, endpoints in NODES.items():
            value = sum_metric_by_substring(
                endpoints["metrics"], "cluster_peers_alive",
            )
            print(f"  {name}: cluster_peers_alive = {value}")
            self.assertEqual(
                value, 2.0,
                f"{name}: expected 2.0 peers alive, got {value}",
            )

    def test_07_gossip_counters_increment(self):
        """Gossip sync counters advance when new routes are learned.

        ``router_cluster_sync_sent`` / ``router_cluster_sync_received`` count
        per-peer route announcement packets — they only advance when a node
        actually broadcasts a learned prefix route (see
        ``GossipProtocol::broadcast_route`` in src/gossip_protocol.cpp).
        Idle heartbeats use a broadcast path that doesn't touch these
        counters, so the test must drive traffic that forces new routes to
        be learned, then assert the counters moved as a result.
        """
        print("\nTest: gossip sync counters increment when routes broadcast")
        node1_metrics = NODES["node1"]["metrics"]
        node1_api = NODES["node1"]["api"]

        sent_before = sum_metric_by_substring(node1_metrics, "router_cluster_sync_sent")
        # Peers learn the new route from node1's broadcast — read the
        # received counter on a peer to observe the other direction.
        node2_metrics = NODES["node2"]["metrics"]
        recv_before = sum_metric_by_substring(node2_metrics, "router_cluster_sync_received")
        print(f"  before: node1.sent={sent_before}, node2.received={recv_before}")

        # Unique prompts force new prefix routes to be learned on node1,
        # which triggers broadcast_route() to every peer.  Three sends
        # because the tokenizer may coalesce similar prompts into an
        # existing route.
        unique_suffix = str(int(time.time() * 1000))
        for i in range(3):
            status, _body, _headers = send_chat_request(
                node1_api,
                [{"role": "user", "content": f"gossip probe {unique_suffix}-{i}"}],
            )
            self.assertEqual(status, 200, f"request {i}: HTTP {status}")

        # Poll with a 10 s deadline: the broadcast happens asynchronously
        # after the response body closes, and slow CI runners add jitter.
        deadline = time.time() + 10.0
        sent_after = sent_before
        recv_after = recv_before
        while time.time() < deadline:
            sent_after = sum_metric_by_substring(node1_metrics, "router_cluster_sync_sent")
            recv_after = sum_metric_by_substring(node2_metrics, "router_cluster_sync_received")
            if sent_after > sent_before and recv_after > recv_before:
                break
            time.sleep(0.5)
        print(f"  after:  node1.sent={sent_after}, node2.received={recv_after}")

        self.assertGreater(
            sent_after - sent_before, 0,
            f"router_cluster_sync_sent on node1 did not advance within 10 s "
            f"(before={sent_before}, after={sent_after})",
        )
        self.assertGreater(
            recv_after - recv_before, 0,
            f"router_cluster_sync_received on node2 did not advance within 10 s "
            f"(before={recv_before}, after={recv_after})",
        )

    def test_08_per_shard_metrics_available(self):
        """Seastar exports per-shard series labelled ``shard="0"`` — document and lock it."""
        print("\nTest: per-shard metrics labelled shard=\"0\"")
        raw = _fetch_raw_metrics(NODES["node1"]["metrics"])

        shard_zero_lines = [
            line for line in raw.split("\n")
            if 'shard="0"' in line
            and not line.startswith("#")
            and line.strip()
        ]

        self.assertGreaterEqual(
            len(shard_zero_lines), 1,
            'No metric line contains shard="0" — Seastar per-shard export format may have changed.',
        )
        print(f"  found {len(shard_zero_lines)} metric lines with shard=\"0\"")
        print(f"  sample: {shard_zero_lines[0].strip()}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
