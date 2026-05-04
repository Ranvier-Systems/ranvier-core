#!/usr/bin/env python3
"""Per-API-key attribution cardinality bound tests for Ranvier Core.

Memo §6.2 / §9: verify the bounded label table behaves correctly:
  * Pre-registered sentinels (_unauthenticated, _invalid, _overflow) are
    always present, regardless of traffic.
  * The unlabelled overflow counter (ranvier_api_key_label_overflow_total)
    is always registered.
  * Traffic with malformed Authorization headers populates the _invalid
    sentinel; traffic without the header populates _unauthenticated.

The integration test cluster does not configure ``auth.api_keys`` (see
``tests/integration/configs/node1.yaml``), so the alice/bob/named-key
flow described in memo §9 is exercised via the env-var override path
when Ranvier is run with ``RANVIER_ATTRIBUTION_MAX_LABEL_CARDINALITY``
configured. That scenario requires a custom compose config and is
covered manually for now — flagged as a TODO at the bottom of this
suite.

Usage:
    python tests/integration/test_attribution_cardinality.py
"""

from __future__ import annotations

import re
import sys
import time
import unittest

try:
    import requests  # noqa: F401  (used transitively via send_chat_request)
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    ClusterTestCase,
    NODES,
    free_docker_subnet,
    metric_is_registered,
    send_chat_request,
    sum_metric_by_labels,
    sum_metric_by_substring,
)


def _fetch_raw_metrics(metrics_url: str) -> str:
    resp = requests.get(f"{metrics_url}/metrics", timeout=5)
    resp.raise_for_status()
    return resp.text


class AttributionCardinalityTest(ClusterTestCase):
    """Bounded api_key label table behaviour."""

    PROJECT_NAME = "ranvier-attribution-cardinality"
    AUTO_REGISTER_BACKENDS = True

    @classmethod
    def setUpClass(cls):
        free_docker_subnet()
        super().setUpClass()

    # ------------------------------------------------------------------
    # Sentinels and overflow counter
    # ------------------------------------------------------------------

    def test_01_sentinels_pre_registered(self):
        """All three sentinels must appear at least once in the metric set.

        ``_unauthenticated`` and ``_invalid`` only appear after a request
        with the corresponding shape, so this test sends one of each
        before scraping. ``_overflow`` is pre-registered at boot — it
        must be present even with zero traffic, but we don't assert on
        it being non-zero (no overflow has occurred under default
        cardinality of 256).
        """
        print("\nTest: sentinels (_unauthenticated, _invalid, _overflow) present")
        metrics_url = NODES["node1"]["metrics"]
        api_url = NODES["node1"]["api"]

        # Drive one of each shape so the lazy sentinels populate.
        send_chat_request(
            api_url,
            [{"role": "user", "content": "cardinality probe unauthenticated"}],
        )
        send_chat_request(
            api_url,
            [{"role": "user", "content": "cardinality probe invalid"}],
            extra_headers={"Authorization": "NotBearer token"},
        )
        time.sleep(0.3)

        raw = _fetch_raw_metrics(metrics_url)
        for sentinel in ("_unauthenticated", "_invalid", "_overflow"):
            present = any(
                f'api_key="{sentinel}"' in line
                for line in raw.split("\n")
                if "by_key" in line and not line.startswith("#")
            )
            self.assertTrue(
                present,
                f"sentinel '{sentinel}' missing from per-key metric series. "
                "Pre-registration in init_api_key_attribution() may have failed.",
            )
            print(f"  sentinel api_key=\"{sentinel}\" present")

    def test_02_overflow_counter_registered(self):
        """ranvier_api_key_label_overflow_total must always be registered."""
        print("\nTest: api_key_label_overflow_total registered")
        for name, endpoints in NODES.items():
            self.assertTrue(
                metric_is_registered(
                    endpoints["metrics"], "api_key_label_overflow_total"
                ),
                f"{name}: api_key_label_overflow_total missing. "
                "init_api_key_attribution() may not have been called on this shard.",
            )
        print(f"  registered on all {len(NODES)} nodes")

    def test_03_overflow_counter_zero_under_default_cardinality(self):
        """With the default cardinality (256) and zero configured keys, no overflow."""
        print("\nTest: overflow counter stays at 0 under default cardinality")
        # The integration cluster has no configured api_keys, so the only
        # labels observed are sentinels (3 < 256). Overflow must not fire.
        for name, endpoints in NODES.items():
            value = sum_metric_by_substring(
                endpoints["metrics"], "api_key_label_overflow_total"
            )
            self.assertEqual(
                value, 0.0,
                f"{name}: api_key_label_overflow_total = {value}, "
                "but no overflow should occur under the default cardinality.",
            )
        print("  overflow counter is zero on every node (as expected)")

    def test_04_unauthenticated_traffic_populates_sentinel(self):
        """A chat request with no Authorization header bumps the _unauthenticated counter."""
        print("\nTest: unauthenticated traffic increments _unauthenticated")
        metrics_url = NODES["node1"]["metrics"]
        api_url = NODES["node1"]["api"]

        before = sum_metric_by_labels(
            metrics_url, "http_requests_total_by_key",
            {"api_key": "_unauthenticated"},
        )
        for _ in range(2):
            status, _b, _h = send_chat_request(
                api_url,
                [{"role": "user", "content": "unauthenticated cardinality probe"}],
            )
            self.assertEqual(status, 200)
        time.sleep(0.2)
        after = sum_metric_by_labels(
            metrics_url, "http_requests_total_by_key",
            {"api_key": "_unauthenticated"},
        )
        self.assertGreaterEqual(
            after - before, 2,
            f"_unauthenticated counter advanced by {after - before}, "
            "expected >= 2",
        )
        print(f"  _unauthenticated: {before} -> {after}")

    def test_05_malformed_authorization_populates_invalid(self):
        """A malformed Authorization header bumps the _invalid sentinel counter."""
        print("\nTest: malformed Authorization header increments _invalid")
        metrics_url = NODES["node1"]["metrics"]
        api_url = NODES["node1"]["api"]

        before = sum_metric_by_labels(
            metrics_url, "http_requests_total_by_key",
            {"api_key": "_invalid"},
        )
        # No "Bearer " prefix -> resolver returns "_invalid".
        status, _b, _h = send_chat_request(
            api_url,
            [{"role": "user", "content": "invalid cardinality probe"}],
            extra_headers={"Authorization": "Token nottherighttype"},
        )
        # Auth not enforced on data plane — request still serves.
        self.assertEqual(status, 200)
        time.sleep(0.2)
        after = sum_metric_by_labels(
            metrics_url, "http_requests_total_by_key",
            {"api_key": "_invalid"},
        )
        self.assertGreaterEqual(
            after - before, 1,
            f"_invalid counter advanced by {after - before}, expected >= 1",
        )
        print(f"  _invalid: {before} -> {after}")

    def test_06_per_key_label_format(self):
        """Per-key series must emit Prometheus-valid {api_key="..."} labels."""
        print("\nTest: api_key labels are Prometheus-compliant")
        # Generate at least one of each sentinel by driving traffic.
        api_url = NODES["node1"]["api"]
        send_chat_request(api_url, [{"role": "user", "content": "fmt probe a"}])
        send_chat_request(
            api_url,
            [{"role": "user", "content": "fmt probe b"}],
            extra_headers={"Authorization": "Bearer not-a-real-key"},
        )
        time.sleep(0.2)

        raw = _fetch_raw_metrics(NODES["node1"]["metrics"])
        # Match every api_key="..." occurrence on per-key lines.
        pattern = re.compile(r'api_key="([^"]+)"')
        seen = set()
        for line in raw.split("\n"):
            if "by_key" not in line or line.startswith("#"):
                continue
            for m in pattern.finditer(line):
                seen.add(m.group(1))

        self.assertGreaterEqual(
            len(seen), 2,
            f"Expected at least 2 distinct api_key labels, got {seen}. "
            "Per-key registration may have collapsed labels.",
        )
        # Sentinels are the only labels valid here (no api_keys configured).
        # Sanitiser guarantee: every label is [a-z0-9_]+ (sentinels prefixed
        # with underscore).
        sanitiser = re.compile(r"^[a-z0-9_]+$")
        for label in seen:
            self.assertRegex(
                label, sanitiser,
                f"label {label!r} contains characters that should have been "
                "sanitised. Check sanitise_api_key_label().",
            )
        print(f"  observed api_key labels: {sorted(seen)}")

    # NOTE: Memo §9 also calls for tests that:
    #   * Configure max_label_cardinality=4 with 6 distinct keys, drive
    #     traffic for all 6, and assert exactly 4 + 3 sentinels are
    #     registered with the rest collapsing to _overflow.
    #   * Assert exactly one warn log line per shard on first overflow.
    #
    # Both require a custom server config (api_keys + the env-var
    # cardinality override) which is not feasible to express against
    # the shared docker-compose.test.yml without invasive changes to
    # the test harness. The bound and warn-once logic are unit-testable
    # in C++ via the MetricsService API — see the follow-up
    # `tests/unit/metrics_service_attribution_test.cpp` once a unit-test
    # entry point is added.


def main():
    loader = unittest.TestLoader()
    loader.sortTestMethodsUsing = lambda x, y: (x > y) - (x < y)
    suite = loader.loadTestsFromTestCase(AttributionCardinalityTest)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
