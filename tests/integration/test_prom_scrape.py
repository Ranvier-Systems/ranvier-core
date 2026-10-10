#!/usr/bin/env python3
"""Unit tests for prom_scrape: Seastar per-shard Prometheus parsing.

Pure-Python, no cluster needed.
Run: python3 -m pytest tests/integration/test_prom_scrape.py -v
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from prom_scrape import (  # noqa: E402
    histogram_avg,
    histogram_buckets,
    histogram_percentile,
    metric_value,
)

HIST = "seastar_ranvier_router_routing_latency_seconds"

# Two shards. Shard 0: 10 samples all in (0.001, 0.010]; shard 1: 10 samples all
# in (0.010, 0.100]. Combined: 20 samples, P50 sits exactly at the 0.010 bound.
# Seastar sorts labels, so `le` comes before `shard` and the block never ends
# right after `le`.
EXPOSITION = "\n".join([
    "# HELP seastar_ranvier_router_routing_latency_seconds Routing latency",
    "# TYPE seastar_ranvier_router_routing_latency_seconds histogram",
    f'{HIST}_bucket{{le="0.001000",shard="0"}} 0',
    f'{HIST}_bucket{{le="0.010000",shard="0"}} 10',
    f'{HIST}_bucket{{le="0.100000",shard="0"}} 10',
    f'{HIST}_bucket{{le="+Inf",shard="0"}} 10',
    f'{HIST}_sum{{shard="0"}} 0.05',
    f'{HIST}_count{{shard="0"}} 10',
    f'{HIST}_bucket{{le="0.001000",shard="1"}} 0',
    f'{HIST}_bucket{{le="0.010000",shard="1"}} 0',
    f'{HIST}_bucket{{le="0.100000",shard="1"}} 10',
    f'{HIST}_bucket{{le="+Inf",shard="1"}} 10',
    f'{HIST}_sum{{shard="1"}} 0.5',
    f'{HIST}_count{{shard="1"}} 10',
    # A sibling histogram whose name embeds the one above as a suffix.
    f'seastar_ranvier_router_primary_routing_latency_seconds_bucket{{le="+Inf",shard="0"}} 999',
    f'seastar_ranvier_router_primary_routing_latency_seconds_count{{shard="0"}} 999',
    'seastar_ranvier_prefix_boundary_used{shard="0"} 5',
    'seastar_ranvier_prefix_boundary_used{shard="1"} 7',
    'seastar_ranvier_prefix_boundary_used_total{shard="0"} 1000',
    'seastar_ranvier_scheduler_enabled{shard="0"} 1',
    'seastar_ranvier_scheduler_enabled{shard="1"} 1',
    'seastar_ranvier_router_cluster_sync_invalid{shard="0"} 3',
    "",
])


def test_counter_sums_across_shards():
    assert metric_value(EXPOSITION, "seastar_ranvier_prefix_boundary_used") == 12.0


def test_gauge_max_does_not_sum():
    assert metric_value(EXPOSITION, "ranvier_scheduler_enabled", agg="max") == 1.0
    assert metric_value(EXPOSITION, "ranvier_scheduler_enabled") == 2.0  # sum, for contrast


def test_name_is_not_a_substring_match():
    # "..._used" must not pick up "..._used_total".
    assert metric_value(EXPOSITION, "seastar_ranvier_prefix_boundary_used") == 12.0
    assert metric_value(EXPOSITION, "seastar_ranvier_prefix_boundary_used_total") == 1000.0


def test_suffix_name_matches_prefixed_export():
    # The sync map documents callers passing the unprefixed name.
    assert metric_value(EXPOSITION, "router_cluster_sync_invalid") == 3.0


def test_missing_metric_is_none():
    assert metric_value(EXPOSITION, "does_not_exist") is None
    assert histogram_avg(EXPOSITION, "does_not_exist") is None
    assert histogram_percentile(EXPOSITION, "does_not_exist", 0.5) is None


def test_histogram_avg_sums_sum_and_count_across_shards():
    # (0.05 + 0.5) / (10 + 10), not the last shard's 0.5 / 10.
    assert abs(histogram_avg(EXPOSITION, HIST) - 0.0275) < 1e-12


def test_histogram_buckets_sum_per_le_across_shards():
    buckets = histogram_buckets(EXPOSITION, HIST)
    assert buckets == [(0.001, 0.0), (0.01, 10.0), (0.1, 20.0), (float("inf"), 20.0)]


def test_histogram_buckets_ignore_sibling_with_suffix_name():
    # "router_primary_routing_latency_seconds" must not contribute to
    # "router_routing_latency_seconds": the total would be 1019, not 20.
    assert histogram_buckets(EXPOSITION, HIST)[-1][1] == 20.0


def test_histogram_percentile_interpolates_over_combined_shards():
    # P50 of 20 samples: target 10 = cumulative at the 0.010 bound exactly.
    assert abs(histogram_percentile(EXPOSITION, HIST, 0.50) - 0.010) < 1e-12
    # P75: target 15, in (0.010, 0.100] which holds 10 samples -> halfway.
    assert abs(histogram_percentile(EXPOSITION, HIST, 0.75) - 0.055) < 1e-12
    # P99 lands in the same bucket, near its top, never in +Inf.
    p99 = histogram_percentile(EXPOSITION, HIST, 0.99)
    assert 0.09 < p99 <= 0.1
    # P50 != P99: the old code fell back to one mean for both columns.
    assert histogram_percentile(EXPOSITION, HIST, 0.50) != p99


def test_single_shard_export_still_works():
    single = "\n".join([
        f'{HIST}_bucket{{le="0.010000",shard="0"}} 4',
        f'{HIST}_bucket{{le="+Inf",shard="0"}} 4',
    ])
    assert abs(histogram_percentile(single, HIST, 0.5) - 0.005) < 1e-12


def test_unlabelled_export_still_works():
    plain = "\n".join([
        f'{HIST}_bucket{{le="0.010000"}} 4',
        f'{HIST}_bucket{{le="+Inf"}} 4',
        "seastar_ranvier_prefix_boundary_used 9",
    ])
    assert histogram_buckets(plain, HIST) == [(0.01, 4.0), (float("inf"), 4.0)]
    assert metric_value(plain, "seastar_ranvier_prefix_boundary_used") == 9.0
