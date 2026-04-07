#!/usr/bin/env bash
# Phase 4 push-cache-eviction benchmark runner.
#
# Handles everything on the BENCHMARK side:
#   1. Verifies Ranvier is reachable at $PROXY_URL
#   2. Starts $NUM_BACKENDS mock backends on consecutive ports
#      starting at $BACKEND_PORT_BASE
#   3. Waits for each to be ready on /health
#   4. Registers each with Ranvier via POST /admin/backends
#   5. Runs harness.py in the given $MODE with the full backend-id list
#
# It does NOT start Ranvier itself. IMPORTANT: this script MUST be run
# in the same network namespace as Ranvier (same host, or inside the
# same Docker container) because Ranvier's TCP health check resolves
# 127.0.0.1:<port> in ITS namespace — if you run Ranvier in a
# container and run.sh on the host, the mock backends are unreachable
# from Ranvier and get quarantined.
#
# Start Ranvier first, then run this script in two legs:
#
#   # Leg 1 — baseline (cache_events disabled, the default):
#   ./ranvier --local
#   MODE=silent ./benchmarks/cache_event_generator/run.sh
#
#   # Leg 2 — push (restart Ranvier with cache_events enabled):
#   RANVIER_CACHE_EVENTS_ENABLED=1 ./ranvier --local
#   MODE=push   ./benchmarks/cache_event_generator/run.sh
#
# See docs/benchmarks/push-cache-eviction.md for details.

# Deliberately NOT using `set -e`: we handle errors explicitly so the
# script can print actionable diagnostics instead of silently exiting
# on a failed curl assignment.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RESULTS_DIR="${RESULTS_DIR:-$HERE/results}"
mkdir -p "$RESULTS_DIR"

MODE="${MODE:-silent}"                               # silent | push
REQUESTS="${REQUESTS:-10000}"
CAPACITY="${CAPACITY:-512}"
POPULATION="${POPULATION:-4096}"
ZIPF_S="${ZIPF_S:-1.1}"
SEED="${SEED:-12648430}"
NUM_BACKENDS="${NUM_BACKENDS:-2}"                    # >=1; 2+ needed for real A/B
BATCH_SIZE="${BATCH_SIZE:-1}"                        # 1 for clean 1:1 metric mapping; >1 for batched-sidecar regime
BACKEND_PORT_BASE="${BACKEND_PORT_BASE:-9001}"
PROXY_URL="${PROXY_URL:-http://127.0.0.1:8080}"
METRICS_URL="${METRICS_URL:-http://127.0.0.1:9180}"
EVENTS_URL="${EVENTS_URL:-$PROXY_URL/v1/cache/events}"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

[[ "$MODE" == "silent" || "$MODE" == "push" ]] || \
  die "MODE must be 'silent' or 'push' (got: $MODE)"
[[ "$NUM_BACKENDS" =~ ^[0-9]+$ ]] && (( NUM_BACKENDS >= 1 )) || \
  die "NUM_BACKENDS must be a positive integer (got: $NUM_BACKENDS)"

echo "==> Config:"
echo "    MODE=$MODE  REQUESTS=$REQUESTS  CAPACITY=$CAPACITY"
echo "    NUM_BACKENDS=$NUM_BACKENDS  BACKEND_PORT_BASE=$BACKEND_PORT_BASE"
echo "    PROXY_URL=$PROXY_URL  METRICS_URL=$METRICS_URL"
echo "    RESULTS_DIR=$RESULTS_DIR"
echo

# 1. Sanity: is Ranvier reachable?
echo "==> Checking Ranvier at $PROXY_URL"
if ! curl -sS -o /dev/null "$PROXY_URL/health" 2>/dev/null; then
  die "Cannot reach Ranvier at $PROXY_URL/health. Is it running?
       If Ranvier is inside a Docker container and you're running this
       script on the host, run the script INSIDE the container instead."
fi
echo "    Ranvier OK"

# 2. Start N mock backends in the background.
BACKEND_PIDS=()
BACKEND_IDS=()
BACKEND_LOG="$RESULTS_DIR/mock-backend.log"
: >"$BACKEND_LOG"  # truncate
echo "==> Starting $NUM_BACKENDS mock backend(s) (log: $BACKEND_LOG)"

cleanup() {
  for pid in "${BACKEND_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  for bid in "${BACKEND_IDS[@]:-}"; do
    curl -sS -X DELETE "$PROXY_URL/admin/backends?id=$bid" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

for i in $(seq 1 "$NUM_BACKENDS"); do
  port=$(( BACKEND_PORT_BASE + i - 1 ))
  bid=$i
  # BACKEND_ID is echoed in X-Backend-ID — keep it as the bare integer
  # so harness.py can parse it back to a Ranvier backend id.
  PORT="$port" BACKEND_ID="$bid" \
    python3 "$REPO/tests/integration/mock_backend.py" >>"$BACKEND_LOG" 2>&1 &
  BACKEND_PIDS+=( "$!" )
  BACKEND_IDS+=( "$bid" )
done

# 3. Wait for each backend to accept connections (up to 5s each).
for i in $(seq 1 "$NUM_BACKENDS"); do
  port=$(( BACKEND_PORT_BASE + i - 1 ))
  ready=0
  for _ in $(seq 1 50); do
    if curl -sS -o /dev/null "http://127.0.0.1:$port/health" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 0.1
  done
  [[ $ready -eq 1 ]] || die "Mock backend on port $port did not become ready"
done
echo "    mock backends OK"

# 4. Register each backend with Ranvier. Deregister stale entries first.
echo "==> Registering $NUM_BACKENDS backend(s) with Ranvier"
for i in $(seq 1 "$NUM_BACKENDS"); do
  port=$(( BACKEND_PORT_BASE + i - 1 ))
  bid=$i
  curl -sS -X DELETE "$PROXY_URL/admin/backends?id=$bid" >/dev/null 2>&1 || true
  reg_resp="$(curl -sS -X POST \
    "$PROXY_URL/admin/backends?id=$bid&ip=127.0.0.1&port=$port" 2>&1)"
  reg_rc=$?
  echo "    id=$bid port=$port -> $reg_resp"
  if [[ $reg_rc -ne 0 ]]; then
    die "curl failed for id=$bid (rc=$reg_rc)"
  fi
  if [[ "$reg_resp" != *'"status": "ok"'* ]]; then
    die "Ranvier rejected registration for id=$bid: $reg_resp"
  fi
done

# 5. Run the harness.
backend_ids_csv=$(IFS=,; echo "${BACKEND_IDS[*]}")
out="$RESULTS_DIR/result-$MODE.json"
echo
echo "==> Running harness (mode=$MODE, requests=$REQUESTS, backend-ids=$backend_ids_csv)"
python3 "$HERE/harness.py" \
  --mode "$MODE" \
  --ranvier-proxy-url "$PROXY_URL" \
  --ranvier-metrics-url "$METRICS_URL" \
  --cache-events-url "$EVENTS_URL" \
  --capacity "$CAPACITY" \
  --population "$POPULATION" \
  --requests "$REQUESTS" \
  --zipf-s "$ZIPF_S" \
  --seed "$SEED" \
  --batch-size "$BATCH_SIZE" \
  --backend-ids "$backend_ids_csv" \
  --output "$out"
rc=$?
if [[ $rc -ne 0 ]]; then
  die "harness.py exited with rc=$rc"
fi

echo
echo "Result written: $out"
