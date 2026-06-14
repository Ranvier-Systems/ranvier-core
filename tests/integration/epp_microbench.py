"""GIE EPP overhead microbenchmark.

Measures the per-request latency of one ext_proc routing decision against a
running Ranvier EPP — i.e. the cost of the gRPC `Process` round-trip plus
Ranvier's routing-decision + `alien::submit_to` reactor bridge. This is the
"absolute per-request EPP routing overhead" proof point (BACKLOG §20.2 P1.4
follow-up); the inline-vs-sidecar A/B against a real GIE gateway is separate.

Reuses tests/integration/epp_client.py with a single persistent channel so the
timed quantity is the decision, not channel setup. Run it via
scripts/bench-epp-overhead.sh (which brings up the EPP node + registers a
backend), or point it at any running EPP:

    python tests/integration/epp_microbench.py --target localhost:9002 \
        --requests 2000 --warmup 200

Build constraint: needs a running WITH_GIE_EPP=ON Ranvier + grpcio. Not
runnable in the sandbox.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time

import grpc  # type: ignore

import epp_client


def _percentile(sorted_vals, q: float) -> float:
    """Nearest-rank percentile (q in [0,100]) over a pre-sorted list."""
    if not sorted_vals:
        return float("nan")
    if q <= 0:
        return sorted_vals[0]
    if q >= 100:
        return sorted_vals[-1]
    # nearest-rank
    rank = max(1, int(round(q / 100.0 * len(sorted_vals))))
    return sorted_vals[min(rank, len(sorted_vals)) - 1]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="GIE EPP overhead microbenchmark")
    ap.add_argument("--target", default="localhost:9002", help="EPP host:port")
    ap.add_argument("--requests", type=int, default=2000, help="timed requests")
    ap.add_argument("--warmup", type=int, default=200, help="untimed warmup requests")
    ap.add_argument("--prompt", default="the quick brown fox jumps over the lazy dog " * 8)
    ap.add_argument(
        "--no-body",
        action="store_true",
        help="send headers-only (load/hash path) instead of a tokenized body",
    )
    args = ap.parse_args(argv)

    send_body = not args.no_body
    latencies_ms = []
    ok = 0
    errors = 0

    with grpc.insecure_channel(args.target) as channel:
        # Warmup: prime the connection, tokenizer cache, and JIT-ish paths.
        for _ in range(args.warmup):
            try:
                epp_client.pick_endpoint_on_channel(channel, args.prompt, send_body=send_body)
            except grpc.RpcError:
                pass

        for _ in range(args.requests):
            t0 = time.perf_counter()
            try:
                res = epp_client.pick_endpoint_on_channel(
                    channel, args.prompt, send_body=send_body
                )
            except grpc.RpcError as e:
                errors += 1
                print(f"  RPC error: {e.code()}", file=sys.stderr)
                continue
            dt_ms = (time.perf_counter() - t0) * 1000.0
            latencies_ms.append(dt_ms)
            if res.has_endpoint:
                ok += 1

    if not latencies_ms:
        print("No successful timed requests — is the EPP up and a backend registered?")
        return 1

    latencies_ms.sort()
    print("=" * 60)
    print("GIE EPP overhead microbenchmark")
    print("=" * 60)
    print(f"  target          : {args.target}")
    print(f"  mode            : {'prefix-aware (body)' if send_body else 'headers-only (load/hash)'}")
    print(f"  timed requests  : {len(latencies_ms)}  (endpoint set: {ok}, errors: {errors})")
    print(f"  mean            : {statistics.fmean(latencies_ms):.3f} ms")
    print(f"  p50             : {_percentile(latencies_ms, 50):.3f} ms")
    print(f"  p90             : {_percentile(latencies_ms, 90):.3f} ms")
    print(f"  p99             : {_percentile(latencies_ms, 99):.3f} ms")
    print(f"  min / max       : {latencies_ms[0]:.3f} / {latencies_ms[-1]:.3f} ms")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
