#!/usr/bin/env python3
"""Unit tests for multi-node Prometheus aggregation + Gini (BACKLOG §25 item 9).

Pure-Python: writes tiny per-node /metrics dumps to a tmp dir, no GPU needed.
Run: python3 -m pytest tests/integration/test_results_prometheus.py -v
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from results_parser import (  # noqa: E402
    _gini_coefficient,
    parse_prometheus_report,
    parse_benchmark_log,
)


def _node_dump(fallbacks, residency, backend_counts):
    """One ranvier node's /metrics text: fallbacks, downgrades, per-backend hist counts."""
    lines = [
        "# HELP ranvier_routing_load_aware_fallbacks_total ...",
        f'ranvier_routing_load_aware_fallbacks_total{{shard="0"}} {fallbacks}',
        f'ranvier_router_residency_route_downgrades_total{{shard="0"}} {residency}',
    ]
    for bid, count in backend_counts.items():
        lines.append(
            f'ranvier_backend_latency_seconds_count{{shard="0",backend_id="{bid}"}} {count}'
        )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# _gini_coefficient
# ---------------------------------------------------------------------------

def test_gini_even_distribution_is_zero():
    assert abs(_gini_coefficient([10, 10, 10, 10])) < 1e-9


def test_gini_fully_concentrated():
    # [0,0,0,40] -> 0.75 for n=4.
    assert math.isclose(_gini_coefficient([0, 0, 0, 40]), 0.75, rel_tol=1e-9)


def test_gini_empty_is_none():
    assert _gini_coefficient([]) is None


def test_gini_all_zero_is_zero():
    assert _gini_coefficient([0, 0, 0]) == 0.0


def test_gini_monotonic_with_concentration():
    even = _gini_coefficient([25, 25, 25, 25])
    skewed = _gini_coefficient([70, 10, 10, 10])
    assert skewed > even


# ---------------------------------------------------------------------------
# parse_prometheus_report — the 3-node aggregation (the F5 fix)
# ---------------------------------------------------------------------------

def test_sums_counters_across_three_nodes(tmp_path):
    (tmp_path / "prometheus_metrics_node1.txt").write_text(_node_dump(10, 1, {"1": 100, "2": 100}))
    (tmp_path / "prometheus_metrics_node2.txt").write_text(_node_dump(10, 2, {"1": 100, "2": 100}))
    (tmp_path / "prometheus_metrics_node3.txt").write_text(_node_dump(10, 3, {"1": 100, "2": 100}))
    agg = parse_prometheus_report(str(tmp_path))
    assert agg["nodes_scraped"] == 3
    assert agg["load_aware_fallbacks_total"] == 30           # 10+10+10, NOT one node
    assert agg["residency_route_downgrades_total"] == 6      # 1+2+3
    assert agg["backend_routed_total"] == {"1": 300.0, "2": 300.0}


def test_falls_back_to_combined_single_file(tmp_path):
    (tmp_path / "prometheus_metrics.txt").write_text(_node_dump(7, 0, {"1": 50}))
    agg = parse_prometheus_report(str(tmp_path))
    assert agg["nodes_scraped"] == 1
    assert agg["load_aware_fallbacks_total"] == 7


def test_no_dump_returns_empty(tmp_path):
    assert parse_prometheus_report(str(tmp_path)) == {}


def test_per_node_preferred_over_combined(tmp_path):
    # Both present: per-node files win (avoids double counting the combined file).
    (tmp_path / "prometheus_metrics_node1.txt").write_text(_node_dump(5, 0, {"1": 10}))
    (tmp_path / "prometheus_metrics_node2.txt").write_text(_node_dump(5, 0, {"1": 10}))
    (tmp_path / "prometheus_metrics.txt").write_text(_node_dump(10, 0, {"1": 20}))  # concat
    agg = parse_prometheus_report(str(tmp_path))
    assert agg["nodes_scraped"] == 2
    assert agg["load_aware_fallbacks_total"] == 10  # 5+5 from per-node, not 10+... from combined


# ---------------------------------------------------------------------------
# End-to-end via parse_benchmark_log — Gini + nodes_scraped become fields
# ---------------------------------------------------------------------------

def test_parse_benchmark_log_sets_gini_and_node_count(tmp_path):
    d = tmp_path / "20260706_8gpu_prefix"
    d.mkdir()
    (d / "benchmark.log").write_text("stub benchmark log\n")
    # Concentrated distribution across 3 nodes -> nonzero Gini.
    (d / "prometheus_metrics_node1.txt").write_text(_node_dump(10, 0, {"1": 300, "2": 5, "3": 5}))
    (d / "prometheus_metrics_node2.txt").write_text(_node_dump(10, 0, {"1": 300, "2": 5, "3": 5}))
    (d / "prometheus_metrics_node3.txt").write_text(_node_dump(10, 0, {"1": 300, "2": 5, "3": 5}))
    r = parse_benchmark_log(str(d / "benchmark.log"))
    assert r.prometheus_nodes_scraped == 3
    assert r.load_aware_fallbacks_total == 30
    assert r.backend_active_requests == {"1": 900.0, "2": 15.0, "3": 15.0}
    assert r.backend_request_gini is not None and r.backend_request_gini > 0.3


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-v"]))
