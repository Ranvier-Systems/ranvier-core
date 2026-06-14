"""HTTP latency driver for the inline-vs-sidecar EPP overhead A/B.

Sends /v1/chat/completions requests to one arm's ingress and reports per-request
latency percentiles. Single-stream by default (--concurrency 1) because the A/B
measures a per-request OVERHEAD delta, and sequential latency is the cleanest,
most reproducible signal (concurrency adds queueing noise). The shared mock
backend's response time is identical across arms, so it cancels in the deltas.

Run via scripts/bench-inline-vs-sidecar.sh (which drives all three arms), or
point it at any ingress:

    python tests/integration/http_ab_load.py --target http://localhost:8101 \
        --requests 2000 --warmup 200

Only needs `requests` (already a suite dep). Not runnable in the sandbox.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import requests


PROMPT = "the quick brown fox jumps over the lazy dog " * 8


def _percentile(sorted_vals, q: float) -> float:
    if not sorted_vals:
        return float("nan")
    if q <= 0:
        return sorted_vals[0]
    if q >= 100:
        return sorted_vals[-1]
    rank = max(1, int(round(q / 100.0 * len(sorted_vals))))
    return sorted_vals[min(rank, len(sorted_vals)) - 1]


def _one_request(session: requests.Session, url: str, body: dict, timeout: float):
    """Return (latency_ms, ok) for a single non-streaming chat completion."""
    t0 = time.perf_counter()
    try:
        resp = session.post(url, json=body, timeout=timeout)
        dt_ms = (time.perf_counter() - t0) * 1000.0
        return dt_ms, resp.status_code == 200
    except requests.exceptions.RequestException:
        return None, False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="inline-vs-sidecar A/B HTTP latency driver")
    ap.add_argument("--target", required=True, help="ingress base URL, e.g. http://localhost:8101")
    ap.add_argument("--requests", type=int, default=2000, help="timed requests")
    ap.add_argument("--warmup", type=int, default=200, help="untimed warmup requests")
    ap.add_argument("--concurrency", type=int, default=1, help="concurrent workers (1 = clean latency)")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--label", default="", help="arm label for the output header")
    args = ap.parse_args(argv)

    url = args.target.rstrip("/") + "/v1/chat/completions"
    # Non-streaming so the timed quantity is a complete request, not just TTFB.
    body = {
        "model": "test-model",
        "messages": [{"role": "user", "content": PROMPT}],
        "stream": False,
    }

    session = requests.Session()
    # Warmup (prime connections, tokenizer cache, ext_proc channel).
    for _ in range(args.warmup):
        _one_request(session, url, body, args.timeout)

    latencies = []
    ok = 0
    errors = 0

    if args.concurrency <= 1:
        for _ in range(args.requests):
            dt, good = _one_request(session, url, body, args.timeout)
            if dt is None:
                errors += 1
                continue
            latencies.append(dt)
            ok += int(good)
    else:
        # Each worker uses its own session (requests.Session isn't thread-safe).
        def worker(_):
            s = requests.Session()
            return _one_request(s, url, body, args.timeout)

        with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
            for dt, good in ex.map(worker, range(args.requests)):
                if dt is None:
                    errors += 1
                    continue
                latencies.append(dt)
                ok += int(good)

    if not latencies:
        print(f"No successful requests against {url} — is the arm up and a backend registered?")
        return 1

    latencies.sort()
    header = f"arm: {args.label}" if args.label else f"target: {args.target}"
    print("-" * 60)
    print(header)
    print(
        f"  reqs={len(latencies)} ok={ok} errors={errors} conc={args.concurrency}  "
        f"mean={statistics.fmean(latencies):.3f}ms "
        f"p50={_percentile(latencies,50):.3f} "
        f"p90={_percentile(latencies,90):.3f} "
        f"p99={_percentile(latencies,99):.3f} "
        f"max={latencies[-1]:.3f}"
    )
    # Machine-readable line for the runner to parse/compare across arms.
    print(
        "JSON " + json.dumps({
            "label": args.label,
            "target": args.target,
            "reqs": len(latencies),
            "ok": ok,
            "errors": errors,
            "concurrency": args.concurrency,
            "mean_ms": statistics.fmean(latencies),
            "p50_ms": _percentile(latencies, 50),
            "p90_ms": _percentile(latencies, 90),
            "p99_ms": _percentile(latencies, 99),
            "max_ms": latencies[-1],
        })
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
