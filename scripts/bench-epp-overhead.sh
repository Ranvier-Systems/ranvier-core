#!/bin/bash
# =============================================================================
# GIE Endpoint-Picker (EPP) overhead microbenchmark
# =============================================================================
#
# Measures the absolute per-request overhead of one ext_proc routing decision
# against a running Ranvier EPP: the gRPC Process round-trip + Ranvier's
# routing-decision + alien::submit_to reactor bridge. The "number not currently
# published anywhere" proof point (BACKLOG §20.2 P1.4 follow-up). The full
# inline-vs-sidecar A/B against a real GIE gateway is a separate, heavier
# benchmark.
#
# Methodology: docs/benchmarks/epp-overhead-microbenchmark.md
# Requires:    Docker (+ Compose), a WITH_GIE_EPP=ON build, and Python with
#              grpcio + grpcio-tools (pip install -r tests/integration/requirements.txt).
#
# Usage:
#   ./scripts/bench-epp-overhead.sh                 # 2000 timed reqs (prefix-aware)
#   ./scripts/bench-epp-overhead.sh --requests 5000
#   ./scripts/bench-epp-overhead.sh --no-body       # headers-only (load/hash) path
#
# Build constraint: not runnable in the sandbox — run in your Docker env.
# =============================================================================

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/docker-compose.epp-test.yml"
PROJECT="ranvier-epp-bench"

EPP_METRICS="http://localhost:9191/metrics"
EPP_API="http://localhost:8091"
EPP_TARGET="localhost:9002"
BACKEND_IP="172.29.1.10"
BACKEND_PORT="8000"

# Forward extra flags (e.g. --requests, --warmup, --no-body) to the driver.
DRIVER_ARGS=("$@")

# Detect docker compose invocation.
if docker compose version >/dev/null 2>&1; then
    COMPOSE=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE=(docker-compose)
else
    echo "error: neither 'docker compose' nor 'docker-compose' found" >&2
    exit 1
fi

compose() { "${COMPOSE[@]}" -f "${COMPOSE_FILE}" -p "${PROJECT}" "$@"; }

cleanup() { compose down -v --remove-orphans >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "Bringing up EPP node + mock backend..."
compose down -v --remove-orphans >/dev/null 2>&1 || true
compose up -d --build

echo "Waiting for the EPP node to become healthy..."
for _ in $(seq 1 60); do
    if curl -fsS "${EPP_METRICS}" >/dev/null 2>&1; then
        break
    fi
    sleep 2
done
if ! curl -fsS "${EPP_METRICS}" >/dev/null 2>&1; then
    echo "error: EPP node did not become healthy" >&2
    compose logs --tail=80 || true
    exit 1
fi

echo "Registering backend 1 (${BACKEND_IP}:${BACKEND_PORT})..."
curl -fsS -X POST \
    "${EPP_API}/admin/backends?id=1&ip=${BACKEND_IP}&port=${BACKEND_PORT}" >/dev/null
# Give registration a moment to propagate across shards.
sleep 3

echo "Running microbenchmark against ${EPP_TARGET}..."
python3 "${REPO_ROOT}/tests/integration/epp_microbench.py" \
    --target "${EPP_TARGET}" "${DRIVER_ARGS[@]}"
