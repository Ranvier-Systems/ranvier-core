#!/bin/bash
# =============================================================================
# REMOVED: functionality consolidated into ./scripts/bench.sh
# =============================================================================
#
# This orchestrator was deprecated and has now been reduced to a hard stop so it
# can no longer silently run the old, drift-prone benchmark path (or reproduce the
# deprecated 5-prefix workload). Use bench.sh instead:
#
#   Instead of:
#     ./scripts/run-multi-gpu-benchmark.sh 10.0.0.1 10.0.0.2
#   Use:
#     ./scripts/bench.sh --skip-vllm --vllm-endpoints 10.0.0.1:8000,10.0.0.2:8000
#
#   Sequential ports on one host:
#     ./scripts/bench.sh --skip-vllm --vllm-host 10.0.0.1 --gpus 8
#
# See scripts/bench.sh --help and docs/benchmarks/benchmark-methodology.md.
# =============================================================================

echo "" >&2
echo -e "\033[1;31m[REMOVED]\033[0m scripts/run-multi-gpu-benchmark.sh has been retired." >&2
echo "Use: ./scripts/bench.sh --skip-vllm --vllm-endpoints <ip1>:8000,<ip2>:8000" >&2
echo "     (see ./scripts/bench.sh --help and docs/benchmarks/benchmark-methodology.md)" >&2
echo "" >&2
exit 1
