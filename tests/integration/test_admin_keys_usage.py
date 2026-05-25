#!/usr/bin/env python3
"""Integration tests for the GET /admin/keys/usage endpoint (memo §8).

This is the read/query half of per-API-key attribution: the metrics and
persistence *write* paths are covered by test_metrics.py and
test_persistence_recovery.py, but nothing else exercises the admin
endpoint end-to-end (admin handler -> query_attribution_summary ->
SQLite -> JSON response). These tests close that gap.

Environment note
----------------
The integration cluster configures no ``auth.api_keys`` (see
tests/integration/configs/node1.yaml), so:
  * the admin endpoint is open (admin auth is disabled when no keys are
    configured), and
  * every proxied request attributes to api_key_id == "" (the
    "_unauthenticated" branch of resolve_api_key). The query therefore
    returns a single aggregated row keyed by the empty string. Testing
    the named-key path requires a custom server config the shared
    docker-compose file can't express; the cardinality unit tests in
    metrics_service_test.cpp cover the keyed bound logic instead.

The request_attribution table is single-node: node1 persists rows only
for the requests it served, so the tests drive traffic to node1's API
and query node1's admin endpoint.

Usage:
    python tests/integration/test_admin_keys_usage.py
"""

from __future__ import annotations

import sys
import time
import unittest

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    ClusterTestCase,
    NODES,
    free_docker_subnet,
    send_chat_request,
)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _query_usage(api_url: str, *, from_ms=None, to_ms=None, key=None, limit=None,
                 timeout: int = 5):
    """GET /admin/keys/usage with optional params. Returns (status, json_or_none)."""
    params = {}
    if from_ms is not None:
        params["from"] = from_ms
    if to_ms is not None:
        params["to"] = to_ms
    if key is not None:
        params["key"] = key
    if limit is not None:
        params["limit"] = limit
    resp = requests.get(f"{api_url}/admin/keys/usage", params=params, timeout=timeout)
    body = None
    try:
        body = resp.json()
    except ValueError:
        pass
    return resp.status_code, body


class AdminKeysUsageTest(ClusterTestCase):
    """End-to-end coverage of GET /admin/keys/usage."""

    PROJECT_NAME = "ranvier-admin-keys-usage"
    AUTO_REGISTER_BACKENDS = True

    @classmethod
    def setUpClass(cls):
        free_docker_subnet()
        super().setUpClass()

    def _send_unauthenticated(self, n: int, tag: str) -> None:
        for i in range(n):
            status, _body, _headers = send_chat_request(
                NODES["node1"]["api"],
                [{"role": "user", "content": f"admin-usage {tag} {i}"}],
            )
            self.assertEqual(status, 200, f"chat request {i} failed: HTTP {status}")

    def _wait_for_rows(self, api_url, from_ms, to_ms, *, min_count, timeout=15):
        """Poll the endpoint until the aggregated request_count reaches min_count.

        Rows flow through the async persistence worker, so they land a short
        time after the response closes. Returns the final rows list (possibly
        short of min_count if the deadline passes — the caller asserts).
        """
        deadline = time.time() + timeout
        rows = []
        while time.time() < deadline:
            status, body = _query_usage(api_url, from_ms=from_ms, to_ms=to_ms)
            if status == 200 and body and body.get("rows"):
                rows = body["rows"]
                total = sum(r.get("request_count", 0) for r in rows)
                if total >= min_count:
                    return rows
            time.sleep(0.5)
        return rows

    # ------------------------------------------------------------------
    # Happy path
    # ------------------------------------------------------------------

    def test_01_usage_returns_aggregated_rows(self):
        """Traffic produces an aggregated row with the documented field shape."""
        print("\nTest: /admin/keys/usage returns aggregated per-key rows")
        api_url = NODES["node1"]["api"]

        n = 3
        from_ms = _now_ms() - 3600_000  # 1h ago
        self._send_unauthenticated(n, "happy")
        to_ms = _now_ms() + 3600_000    # 1h ahead (clock skew headroom)

        rows = self._wait_for_rows(api_url, from_ms, to_ms, min_count=n)
        self.assertTrue(
            rows,
            "Expected at least one aggregated row after sending traffic; got none. "
            "The LogRequestOp write path or query_attribution_summary may be broken.",
        )

        # The cluster has no configured keys, so all traffic aggregates under
        # api_key_id == "" (unauthenticated).
        row = next((r for r in rows if r.get("api_key_id") == ""), None)
        self.assertIsNotNone(
            row,
            f"No row for the unauthenticated key (api_key_id == ''). rows={rows}",
        )

        # Documented field shape (memo §8.2).
        expected_fields = {
            "api_key_id", "request_count", "success_count", "error_count",
            "latency_ms_p50", "latency_ms_p95", "latency_ms_p99",
            "input_tokens_sum", "output_tokens_sum", "cost_units_sum",
        }
        missing = expected_fields - set(row.keys())
        self.assertFalse(missing, f"row missing fields: {missing}. row={row}")

        self.assertGreaterEqual(
            row["request_count"], n,
            f"request_count {row['request_count']} < {n} sent",
        )
        # All three requests succeeded (mock backend returns 200).
        self.assertGreaterEqual(row["success_count"], n)
        # Percentiles are non-negative ints; p99 >= p50 by construction.
        self.assertGreaterEqual(row["latency_ms_p99"], row["latency_ms_p50"])
        print(f"  aggregated row: request_count={row['request_count']}, "
              f"success={row['success_count']}, "
              f"p50/p95/p99={row['latency_ms_p50']}/{row['latency_ms_p95']}/"
              f"{row['latency_ms_p99']}")

    def test_02_response_envelope_shape(self):
        """The response carries window bounds and a boolean truncated flag."""
        print("\nTest: response envelope has window + truncated")
        api_url = NODES["node1"]["api"]
        from_ms = _now_ms() - 3600_000
        to_ms = _now_ms() + 3600_000

        status, body = _query_usage(api_url, from_ms=from_ms, to_ms=to_ms)
        self.assertEqual(status, 200, f"HTTP {status}")
        self.assertIsNotNone(body, "response was not valid JSON")
        self.assertIn("window", body)
        self.assertEqual(body["window"].get("from_ms"), from_ms)
        self.assertEqual(body["window"].get("to_ms"), to_ms)
        self.assertIn("rows", body)
        self.assertIsInstance(body["rows"], list)
        self.assertIn("truncated", body)
        self.assertIsInstance(body["truncated"], bool)
        print(f"  window echoed, truncated={body['truncated']}, "
              f"rows={len(body['rows'])}")

    # ------------------------------------------------------------------
    # Parameter validation
    # ------------------------------------------------------------------

    def test_03_missing_params_returns_400(self):
        """from/to are required."""
        print("\nTest: missing from/to -> 400")
        api_url = NODES["node1"]["api"]

        status, _ = _query_usage(api_url)  # no params
        self.assertEqual(status, 400, "expected 400 with no params")

        status, _ = _query_usage(api_url, from_ms=_now_ms())  # only from
        self.assertEqual(status, 400, "expected 400 with only 'from'")

        status, _ = _query_usage(api_url, to_ms=_now_ms())  # only to
        self.assertEqual(status, 400, "expected 400 with only 'to'")
        print("  all three missing-param cases returned 400")

    def test_04_inverted_window_returns_400(self):
        """to must be strictly greater than from."""
        print("\nTest: to <= from -> 400")
        api_url = NODES["node1"]["api"]
        now = _now_ms()

        status, _ = _query_usage(api_url, from_ms=now, to_ms=now)  # equal
        self.assertEqual(status, 400, "expected 400 when to == from")

        status, _ = _query_usage(api_url, from_ms=now, to_ms=now - 1000)  # inverted
        self.assertEqual(status, 400, "expected 400 when to < from")
        print("  equal and inverted windows returned 400")

    def test_05_window_too_large_returns_400(self):
        """Window wider than admin_query_max_window_hours (168h default) is rejected."""
        print("\nTest: window > 168h -> 400")
        api_url = NODES["node1"]["api"]
        from_ms = _now_ms()
        # 169 hours — just over the default 7-day (168h) cap.
        to_ms = from_ms + 169 * 3600_000

        status, body = _query_usage(api_url, from_ms=from_ms, to_ms=to_ms)
        self.assertEqual(status, 400, f"expected 400 for over-long window, got {status}")
        # Error message references the configured bound.
        if body and "error" in body:
            self.assertIn("admin_query_max_window_hours", body["error"])
        print("  over-long window returned 400")

    def test_06_non_numeric_params_returns_400(self):
        """Non-numeric from/to are rejected (Rule #10: validated parsing)."""
        print("\nTest: non-numeric from/to -> 400")
        api_url = NODES["node1"]["api"]
        resp = requests.get(
            f"{api_url}/admin/keys/usage",
            params={"from": "notanumber", "to": "alsobad"},
            timeout=5,
        )
        self.assertEqual(resp.status_code, 400)
        print("  non-numeric params returned 400")

    # ------------------------------------------------------------------
    # Filtering and limiting
    # ------------------------------------------------------------------

    def test_07_unknown_key_filter_returns_empty_rows(self):
        """Scoping to a key that never sent traffic returns 200 with no rows."""
        print("\nTest: key=<unknown> -> empty rows")
        api_url = NODES["node1"]["api"]
        from_ms = _now_ms() - 3600_000
        to_ms = _now_ms() + 3600_000

        status, body = _query_usage(
            api_url, from_ms=from_ms, to_ms=to_ms, key="no-such-key-xyz")
        self.assertEqual(status, 200, f"HTTP {status}")
        self.assertIsNotNone(body)
        self.assertEqual(body["rows"], [], f"expected no rows, got {body['rows']}")
        self.assertFalse(body["truncated"])
        print("  unknown-key filter returned an empty row set")

    def test_08_limit_truncates_and_sets_flag(self):
        """A row_limit below the materialised row count sets truncated=true."""
        print("\nTest: limit below row count -> truncated=true")
        api_url = NODES["node1"]["api"]

        # Ensure several raw rows exist within the window.
        n = 4
        from_ms = _now_ms() - 3600_000
        self._send_unauthenticated(n, "limit")
        to_ms = _now_ms() + 3600_000

        # Wait until at least n rows are persisted (unfiltered aggregate count).
        self._wait_for_rows(api_url, from_ms, to_ms, min_count=n)

        # limit=1 caps the raw SELECT at one row -> truncated must be true.
        status, body = _query_usage(api_url, from_ms=from_ms, to_ms=to_ms, limit=1)
        self.assertEqual(status, 200, f"HTTP {status}")
        self.assertTrue(
            body["truncated"],
            "truncated should be true when limit caps the materialised rows "
            f"below the available count. body={body}",
        )
        print(f"  limit=1 set truncated={body['truncated']}")


def main():
    loader = unittest.TestLoader()
    loader.sortTestMethodsUsing = lambda x, y: (x > y) - (x < y)
    suite = loader.loadTestsFromTestCase(AdminKeysUsageTest)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
