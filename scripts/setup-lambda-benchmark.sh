#!/bin/bash
# =============================================================================
# REMOVED: functionality consolidated into ./scripts/bench.sh --setup
# =============================================================================
#
# This setup script was deprecated and has now been reduced to a hard stop. It
# previously heredoc-generated a repo-root run-benchmark.sh that name-collided
# with tests/integration/run-benchmark.sh (the CI baseline comparator); that
# generation is gone. Use bench.sh instead:
#
#   Instead of:
#     ./scripts/setup-lambda-benchmark.sh
#   Use:
#     ./scripts/bench.sh --setup
#
# See scripts/bench.sh --help and docs/benchmarks/benchmark-methodology.md.
# =============================================================================

echo "" >&2
echo -e "\033[1;31m[REMOVED]\033[0m scripts/setup-lambda-benchmark.sh has been retired." >&2
echo "Use: ./scripts/bench.sh --setup" >&2
echo "     (see ./scripts/bench.sh --help and docs/benchmarks/benchmark-methodology.md)" >&2
echo "" >&2
exit 1
