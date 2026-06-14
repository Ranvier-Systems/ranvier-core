#!/bin/bash
# =============================================================================
# Inline vs. Sidecar (GIE EPP) routing-overhead A/B
# =============================================================================
#
# Drives identical single-stream load through three arms over ONE backend and
# reports the per-request latency deltas (BACKLOG §20.2 P1.4):
#   A  inline       client -> Ranvier :8080 -> backend
#   B  plain Envoy  client -> Envoy   :10000 -> backend
#   C  Envoy + EPP  client -> Envoy   :10001 -> ext_proc(Ranvier EPP) -> backend
#   ext_proc overhead   = C - B   (same proxy, with vs without the EPP call)
#   inline-vs-sidecar   = C - A
#
# Methodology + caveats: docs/benchmarks/inline-vs-sidecar-ab-benchmark.md
# Scope/design:          docs/benchmarks/inline-vs-sidecar-ab-scope.md
# Requires: Docker (+ Compose), a WITH_GIE_EPP=ON build, Python `requests`.
#
# Usage:
#   ./scripts/bench-inline-vs-sidecar.sh                  # 2000 reqs/arm
#   ./scripts/bench-inline-vs-sidecar.sh --requests 5000 --warmup 500
#
# Build constraint: not runnable in the sandbox — run on your Docker host.
# =============================================================================

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/docker-compose.epp-ab.yml"
PROJECT="ranvier-epp-ab"
DRIVER="${REPO_ROOT}/tests/integration/http_ab_load.py"

RANVIER_METRICS="http://localhost:9192/metrics"
RANVIER_API="http://localhost:8101"
BACKEND_IP="172.30.1.10"
BACKEND_PORT="8000"

ARM_A="http://localhost:8101"   # inline Ranvier
ARM_B="http://localhost:8102"   # plain Envoy
ARM_C="http://localhost:8103"   # Envoy + ext_proc EPP

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

echo "Bringing up A/B stack (Ranvier + Envoy + mock backend)..."
compose down -v --remove-orphans >/dev/null 2>&1 || true
compose up -d --build

echo "Waiting for Ranvier to become healthy..."
for _ in $(seq 1 60); do
    curl -fsS "${RANVIER_METRICS}" >/dev/null 2>&1 && break
    sleep 2
done
if ! curl -fsS "${RANVIER_METRICS}" >/dev/null 2>&1; then
    echo "error: Ranvier did not become healthy" >&2
    compose logs --tail=80 || true
    exit 1
fi

echo "Registering backend 1 (${BACKEND_IP}:${BACKEND_PORT})..."
curl -fsS -X POST \
    "${RANVIER_API}/admin/backends?id=1&ip=${BACKEND_IP}&port=${BACKEND_PORT}" >/dev/null
# Let registration propagate + Envoy settle on its upstreams.
sleep 4

OUT="$(mktemp -d)"
# Forward extra flags (e.g. --requests/--warmup) to every arm via "$@".
python_arm() {
    local label="$1" target="$2" file="$3"; shift 3
    echo "Running arm ${label} against ${target}..."
    python3 "${DRIVER}" --target "${target}" --label "${label}" "$@" | tee "${file}"
}

python_arm "A-inline"     "${ARM_A}" "${OUT}/a.txt" "$@"
python_arm "B-plain-envoy" "${ARM_B}" "${OUT}/b.txt" "$@"
python_arm "C-envoy-epp"  "${ARM_C}" "${OUT}/c.txt" "$@"

echo
echo "Computing deltas..."
python3 - "${OUT}/a.txt" "${OUT}/b.txt" "${OUT}/c.txt" <<'PY'
import json, sys

def load(path):
    for line in open(path):
        if line.startswith("JSON "):
            return json.loads(line[5:])
    return None

a, b, c = (load(p) for p in sys.argv[1:4])
if not all((a, b, c)):
    print("  (could not parse one or more arms — see output above)")
    sys.exit(0)

def row(name, d):
    print(f"  {name:14s} p50={d['p50_ms']:.3f}ms  p99={d['p99_ms']:.3f}ms  mean={d['mean_ms']:.3f}ms")

print("=" * 64)
print("Inline vs. sidecar (GIE EPP) — single-stream latency")
print("=" * 64)
row("A inline", a)
row("B plain envoy", b)
row("C envoy+epp", c)
print("-" * 64)
print(f"  ext_proc overhead (C - B):  p50 {c['p50_ms']-b['p50_ms']:+.3f}ms  "
      f"p99 {c['p99_ms']-b['p99_ms']:+.3f}ms  mean {c['mean_ms']-b['mean_ms']:+.3f}ms")
print(f"  inline-vs-sidecar (C - A):  p50 {c['p50_ms']-a['p50_ms']:+.3f}ms  "
      f"p99 {c['p99_ms']-a['p99_ms']:+.3f}ms  mean {c['mean_ms']-a['mean_ms']:+.3f}ms")
print(f"  envoy vs ranvier proxy (B - A):  p50 {b['p50_ms']-a['p50_ms']:+.3f}ms  "
      f"mean {b['mean_ms']-a['mean_ms']:+.3f}ms")
print("=" * 64)
PY
