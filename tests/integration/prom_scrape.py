"""Pure parsers for Seastar's Prometheus text exposition.

Seastar is shard-per-core and emits one series PER SHARD, with a ``shard="N"``
label on every line and labels sorted by name, e.g.::

    seastar_ranvier_router_routing_latency_seconds_bucket{le="0.001000",shard="0"} 12
    seastar_ranvier_router_routing_latency_seconds_bucket{le="0.001000",shard="1"} 9
    seastar_ranvier_prefix_boundary_used{shard="3"} 41

so a reader must (a) tolerate a label block after the metric name and (b)
combine across shards: sum for counters and histogram buckets, max for gauges.
Reading the first matching line, or requiring ``{le="x"}`` to be the whole
label block, silently yields one shard's numbers (or nothing at all).

Shared by both locustfiles. ``results_parser.py`` has its own multi-node
variant for the dumped ``prometheus_metrics_node*.txt`` files.
"""

import re
from typing import Dict, Iterator, List, Optional, Tuple

_NUM = r"([-+]?(?:\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|Inf)|NaN)"
_LE = re.compile(r'(?:^|[{,])le="([^"]+)"')


def _series_re(name: str) -> "re.Pattern[str]":
    # Prefix-tolerant on purpose: callers pass either the full exported name
    # ("seastar_ranvier_router_routing_latency_seconds") or a suffix of it
    # ("router_cluster_sync_invalid" for the seastar_ranvier_-prefixed export).
    # The name must be followed by a label block or whitespace so that
    # "foo" never matches "foo_total" or "foo_bucket".
    return re.compile(rf"^(?:[A-Za-z0-9_:]*_)?{re.escape(name)}(\{{[^}}]*\}})?\s+{_NUM}\s*$")


def iter_series(text: str, name: str) -> Iterator[Tuple[str, float]]:
    """Yield (label_block, value) for every sample line of metric ``name``."""
    pattern = _series_re(name)
    for line in text.split("\n"):
        if not line or line.startswith("#"):
            continue
        match = pattern.match(line.strip())
        if match:
            yield (match.group(1) or ""), float(match.group(2))


def metric_value(text: str, name: str, agg: str = "sum") -> Optional[float]:
    """Combine one metric across shards: ``agg`` is "sum" (counters) or "max" (gauges)."""
    values = [v for _, v in iter_series(text, name)]
    if not values:
        return None
    if agg == "max":
        return max(values)
    if agg == "sum":
        return sum(values)
    raise ValueError(f"unknown agg {agg!r}; expected 'sum' or 'max'")


def histogram_avg(text: str, name: str) -> Optional[float]:
    """Mean of a histogram across all shards: sum(_sum) / sum(_count)."""
    total = metric_value(text, f"{name}_sum", "sum")
    count = metric_value(text, f"{name}_count", "sum")
    if total is None or count is None or count <= 0:
        return None
    return total / count


def histogram_buckets(text: str, name: str) -> List[Tuple[float, float]]:
    """Cumulative buckets of a histogram, summed per ``le`` across shards, sorted by bound."""
    per_le: Dict[float, float] = {}
    for labels, value in iter_series(text, f"{name}_bucket"):
        le_match = _LE.search(labels)
        if not le_match:
            continue
        bound = float(le_match.group(1))  # float("+Inf") == inf
        per_le[bound] = per_le.get(bound, 0.0) + value
    return sorted(per_le.items())


def histogram_percentile(text: str, name: str, percentile: float) -> Optional[float]:
    """Percentile from cumulative buckets by linear interpolation within the crossing bucket.

    Reference: https://prometheus.io/docs/practices/histograms/#quantiles
    Returns the previous finite bound when the crossing bucket is +Inf.
    """
    buckets = histogram_buckets(text, name)
    if not buckets:
        return None
    total_count = buckets[-1][1]
    if total_count <= 0:
        return None

    target_count = percentile * total_count
    prev_bound = 0.0
    prev_count = 0.0
    for upper_bound, cumulative_count in buckets:
        if cumulative_count >= target_count:
            if upper_bound == float("inf"):
                return prev_bound
            bucket_count = cumulative_count - prev_count
            if bucket_count <= 0:
                return prev_bound
            fraction = (target_count - prev_count) / bucket_count
            return prev_bound + (upper_bound - prev_bound) * fraction
        prev_bound = upper_bound
        prev_count = cumulative_count

    for upper_bound, _ in reversed(buckets):
        if upper_bound != float("inf"):
            return upper_bound
    return None
