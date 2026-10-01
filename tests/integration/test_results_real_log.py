#!/usr/bin/env python3
"""Unit tests for parsing a real-backend benchmark log (audit 2026-09-30 fixes 4-5).

Pure-Python, no cluster needed.
Run: python3 -m pytest tests/integration/test_results_real_log.py -v
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pytest  # noqa: E402

from results_parser import (  # noqa: E402
    _resolve_run_input,
    aggregate_compare,
    aggregate_runs,
    format_aggregate,
    compare_results,
    parse_benchmark_log,
)

# Locust's approximated table: the row it would print for a 13B-class run.
LOCUST_TABLE = """
Type     Name                                   50%    66%    75%    80%    90%    95%    98%    99%  99.9% 99.99%   100% # reqs
GET      TTFT (Time To First Token)            1200   1400   1500   1600   1900   2300   2700   3100   4000   4500   4500   1000
GET      TTFT (Route-consistent)               1100   1300   1400   1500   1800   2200   2600   3000   3900   4400   4400    800
GET      TTFT (Route-changed)                  1500   1700   1800   1900   2200   2600   3000   3300   4100   4500   4500    200
         Aggregated                            5000   5500   6000   6200   7000   8000   8500   9000   9500   9800   9800   1000
"""


def _log(stats: dict, table: str = LOCUST_TABLE) -> str:
    return table + "\nBENCHMARK_STATS_JSON:" + json.dumps(stats) + "\n"


NEW_STATS = {
    "total_requests": 1000, "successful_requests": 990, "failed_requests": 0,
    "cache_hits": 800, "cache_misses": 190, "route_consistency_pct": 80.8,
    "ttft_p50_ms": 1187.4, "ttft_p90_ms": 1912.9, "ttft_p95_ms": 2288.1, "ttft_p99_ms": 3063.7,
    "ttft_samples": 990,
    "kv_prefix_cache_hit_rate_pct": 91.2, "kv_prefix_cache_hits": 9120000.0,
    "kv_prefix_cache_queries": 10000000.0, "kv_prefix_cache_backends_scraped": 8,
    "ttft_cache_hit_p50_ms": 1100.2, "ttft_cache_hit_p99_ms": 2990.0,
    "ttft_cache_miss_p50_ms": 1490.0, "ttft_cache_miss_p99_ms": 3310.0,
}

# A log written before 2026-09-30: old key name, no raw percentiles, no KV counters.
OLD_STATS = {
    "total_requests": 1000, "successful_requests": 990, "failed_requests": 0,
    "cache_hits": 800, "cache_misses": 190, "cache_hit_rate_pct": 80.8,
}
OLD_TABLE = LOCUST_TABLE.replace("Route-consistent", "Cache HIT").replace("Route-changed", "Cache MISS")


def _parse(content: str, tmp_path):
    f = tmp_path / "benchmark.log"
    f.write_text(content)
    return parse_benchmark_log(str(f))


def test_raw_percentiles_override_locust_table(tmp_path):
    r = _parse(_log(NEW_STATS), tmp_path)
    assert r.benchmark_type == "real"
    assert r.ttft_source == "raw"
    assert r.p99_ttft_ms == 3063.7       # not the table's 3100
    assert r.p50_ttft_ms == 1187.4       # not the table's 1200
    assert r.p95_ttft_ms == 2288.1


def test_old_log_falls_back_to_locust_table_and_says_so(tmp_path):
    r = _parse(_log(OLD_STATS, OLD_TABLE), tmp_path)
    assert r.benchmark_type == "real"
    assert r.ttft_source == "locust_table"
    assert r.p99_ttft_ms == 3100.0
    assert r.ttft_cache_hit_p50_ms == 1100.0   # old "Cache HIT" row still parsed


def test_route_consistency_reads_new_and_old_keys(tmp_path):
    new = _parse(_log(NEW_STATS), tmp_path)
    old = _parse(_log(OLD_STATS, OLD_TABLE), tmp_path)
    assert new.cache_hit_rate_pct == 80.8
    assert old.cache_hit_rate_pct == 80.8


def test_kv_prefix_cache_hit_rate_parsed_and_absent_on_old_logs(tmp_path):
    new = _parse(_log(NEW_STATS), tmp_path)
    old = _parse(_log(OLD_STATS, OLD_TABLE), tmp_path)
    assert new.kv_prefix_cache_hit_rate_pct == 91.2
    assert new.kv_prefix_cache_backends_scraped == 8
    assert old.kv_prefix_cache_hit_rate_pct is None


def test_renamed_rows_still_yield_route_ttft(tmp_path):
    r = _parse(_log({**NEW_STATS, "ttft_cache_hit_p50_ms": None}), tmp_path)
    # JSON None -> text/table fallback supplies the Route-consistent row's P50.
    assert r.ttft_cache_hit_p50_ms == 1100.0


def test_comparison_labels_route_consistency_not_cache_hits(tmp_path):
    base = _parse(_log({**NEW_STATS, "route_consistency_pct": 12.5, "kv_prefix_cache_hit_rate_pct": 95.0}), tmp_path)
    new = _parse(_log(NEW_STATS), tmp_path)
    text = compare_results(base, new)
    assert "Route Consistency" in text
    assert "KV Prefix-Cache Hit" in text
    assert "Cache Hit Rate" not in text
    assert "TTFT source: baseline=raw, new=raw" in text
    assert "(+68.3 pp)" in text      # signed, percentage points


def test_aggregate_reports_ttft_sources(tmp_path):
    raw = _parse(_log(NEW_STATS), tmp_path)
    approx = _parse(_log(OLD_STATS, OLD_TABLE), tmp_path)
    single = aggregate_runs([raw, raw, raw])
    assert single["ttft_sources"] == ["raw"]
    assert "not all runs" not in format_aggregate(single)
    mixed = aggregate_compare([approx, approx], [raw, raw], "p99_ttft_ms")
    assert mixed["ttft_sources"] == ["locust_table", "raw"]
    assert "approximated" in format_aggregate(mixed)


def test_text_fallback_does_not_confuse_kv_line_with_route_consistency(tmp_path):
    # No JSON at all: the parser must fall back to the text lines, and the KV
    # line (which contains the substring "Cache Hit Rate:") must not win.
    text = LOCUST_TABLE + """
KV Prefix-Cache Hit Rate: 91.2% (9120000/10000000 tokens across 8 backends)
Route Consistency (client-side: same backend as the previous request with this prefix):
  Route-consistent: 800
  Route changed / first seen: 190
  Route Consistency: 80.8%
"""
    r = _parse(text, tmp_path)
    assert r.cache_hit_rate_pct == 80.8
    assert r.cache_hits == 800 and r.cache_misses == 190


def test_failed_marker_refuses_dir(tmp_path):
    (tmp_path / "benchmark.log").write_text(_log(NEW_STATS))
    assert _resolve_run_input(str(tmp_path)).p99_ttft_ms == 3063.7   # no marker: fine
    (tmp_path / "FAILED").write_text("no BENCHMARK_STATS_JSON in benchmark.log (locust exit 1)\n")
    with pytest.raises(SystemExit) as exc:
        _resolve_run_input(str(tmp_path))
    assert "FAILED" in str(exc.value) and "locust exit 1" in str(exc.value)
