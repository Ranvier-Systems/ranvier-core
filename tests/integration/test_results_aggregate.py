#!/usr/bin/env python3
"""Unit tests for results_parser repeat-run aggregation (BACKLOG §25 item 7).

Pure-Python: constructs BenchmarkResults directly, no log files or GPU needed.
Run: python3 -m pytest tests/integration/test_results_aggregate.py -v
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from results_parser import (  # noqa: E402
    BenchmarkResults,
    aggregate_runs,
    aggregate_compare,
    _quartiles,
    _pct_change,
)


def _run(p99=None, hit_rate=None):
    r = BenchmarkResults()
    r.p99_ttft_ms = p99
    r.cache_hit_rate_pct = hit_rate
    return r


# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------

def test_quartiles_single_value_is_degenerate():
    assert _quartiles([42.0]) == (42.0, 42.0)


def test_quartiles_empty():
    assert _quartiles([]) == (None, None)


def test_pct_change_basic_and_guards():
    assert _pct_change(200.0, 100.0) == -50.0
    assert _pct_change(100.0, 150.0) == 50.0
    assert _pct_change(0.0, 100.0) is None       # divide-by-zero guarded
    assert _pct_change(None, 100.0) is None
    assert _pct_change(100.0, None) is None


# ---------------------------------------------------------------------------
# Single-arm aggregation
# ---------------------------------------------------------------------------

def test_single_arm_median_and_range():
    runs = [_run(p99=100), _run(p99=110), _run(p99=120)]
    agg = aggregate_runs(runs)
    m = agg["metrics"]["p99_ttft_ms"]
    assert agg["mode"] == "single-arm"
    assert m["n"] == 3
    assert m["median"] == 110
    assert m["min"] == 100
    assert m["max"] == 120
    assert agg["p99_outliers"] == []


def test_single_arm_flags_p99_outlier():
    # Tight cluster + one blow-out repeat -> flagged above Q3 + 1.5*IQR.
    runs = [_run(p99=100), _run(p99=102), _run(p99=104), _run(p99=106), _run(p99=300)]
    agg = aggregate_runs(runs)
    assert len(agg["p99_outliers"]) == 1
    assert agg["p99_outliers"][0]["repeat"] == 5
    assert agg["p99_outliers"][0]["p99_ttft_ms"] == 300


def test_single_arm_skips_missing_metric():
    runs = [_run(p99=None), _run(p99=None)]
    agg = aggregate_runs(runs)
    assert "p99_ttft_ms" not in agg["metrics"]


# ---------------------------------------------------------------------------
# Paired A/B aggregation + verdict
# ---------------------------------------------------------------------------

def test_paired_reliable_improvement():
    baseline = [_run(p99=200), _run(p99=210), _run(p99=205)]
    treatment = [_run(p99=100), _run(p99=105), _run(p99=102)]
    agg = aggregate_compare(baseline, treatment, "p99_ttft_ms")
    assert agg["mode"] == "paired-ab"
    assert agg["reliable"] is True
    assert "IMPROVEMENT" in agg["verdict"]
    # All three ~-50% -> IQR entirely below zero.
    assert agg["delta_pct"]["q3"] < 0


def test_paired_no_reliable_effect_when_iqr_spans_zero():
    # Deltas roughly -37%, +31%, +5% -> middle 50% straddles zero.
    baseline = [_run(p99=100), _run(p99=100), _run(p99=100)]
    treatment = [_run(p99=63), _run(p99=131), _run(p99=105)]
    agg = aggregate_compare(baseline, treatment, "p99_ttft_ms")
    assert agg["reliable"] is False
    assert "NO RELIABLE EFFECT" in agg["verdict"]


def test_paired_insufficient_data_single_repeat():
    agg = aggregate_compare([_run(p99=100)], [_run(p99=50)], "p99_ttft_ms")
    assert agg["reliable"] is False
    assert "INSUFFICIENT DATA" in agg["verdict"]


def test_paired_flags_affinity_thrash_hotspot():
    # Treatment P99 >> baseline at high hit rate -> hot-spot flagged.
    baseline = [_run(p99=100, hit_rate=12), _run(p99=100, hit_rate=12)]
    treatment = [_run(p99=200, hit_rate=95), _run(p99=105, hit_rate=95)]
    agg = aggregate_compare(baseline, treatment, "p99_ttft_ms")
    assert len(agg["hotspots"]) == 1
    assert agg["hotspots"][0]["repeat"] == 1


def test_paired_no_hotspot_when_hit_rate_low():
    # Same P99 blow-up but low hit rate -> not the affinity signature.
    baseline = [_run(p99=100, hit_rate=12)]
    treatment = [_run(p99=200, hit_rate=10)]
    agg = aggregate_compare(baseline, treatment, "p99_ttft_ms")
    assert agg["hotspots"] == []


def test_paired_uneven_counts_truncate_to_min():
    baseline = [_run(p99=200), _run(p99=210)]
    treatment = [_run(p99=100)]
    agg = aggregate_compare(baseline, treatment, "p99_ttft_ms")
    assert agg["n_pairs"] == 1


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-v"]))
