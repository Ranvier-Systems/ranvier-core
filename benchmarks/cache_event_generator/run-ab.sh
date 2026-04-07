#!/usr/bin/env bash
# Phase 4 push-cache-eviction A/B runner.
#
# Wraps run.sh to do the full two-leg A/B in one command:
#
#   1. Create a timestamped run directory under results/
#   2. tee everything we print into $RUN_LOG in that directory
#   3. Start Ranvier with cache_events DISABLED, wait for /health,
#      run MODE=silent ./run.sh, then SIGTERM Ranvier
#   4. Start Ranvier with cache_events ENABLED, wait for /health,
#      run MODE=push ./run.sh, then SIGTERM Ranvier
#   5. Print the two result file paths so a follow-up tool can diff
#      them
#
# Usage:
#
#   ./benchmarks/cache_event_generator/run-ab.sh
#
#   # Smaller smoke run:
#   REQUESTS=1000 ./benchmarks/cache_event_generator/run-ab.sh
#
#   # Custom binary path or args:
#   RANVIER_BIN=./build/release/ranvier_server \
#     RANVIER_ARGS='--config ranvier.yaml' \
#     ./benchmarks/cache_event_generator/run-ab.sh
#
# All environment overrides accepted by run.sh (REQUESTS, CAPACITY,
# POPULATION, ZIPF_S, SEED, NUM_BACKENDS, BACKEND_PORT_BASE,
# PROXY_URL, METRICS_URL) work here too and are forwarded.
#
# Deliberately NOT using `set -e` — explicit `die` so nothing fails
# silently.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

RANVIER_BIN="${RANVIER_BIN:-$REPO/build/ranvier_server}"
RANVIER_ARGS="${RANVIER_ARGS:---local}"
PROXY_URL="${PROXY_URL:-http://127.0.0.1:8080}"
METRICS_URL="${METRICS_URL:-http://127.0.0.1:9180}"
RANVIER_STARTUP_TIMEOUT_S="${RANVIER_STARTUP_TIMEOUT_S:-15}"
RANVIER_SHUTDOWN_TIMEOUT_S="${RANVIER_SHUTDOWN_TIMEOUT_S:-10}"

TS="$(date +%Y%m%d_%H%M%S)"
AB_DIR="$HERE/results/ab-$TS"
mkdir -p "$AB_DIR"
RUN_LOG="$AB_DIR/run.log"

# Capture original command before anything shifts.
ORIGINAL_CMD="$0 $*"

# Mirror all stdout/stderr from this wrapper to the run log.
exec > >(tee -a "$RUN_LOG") 2>&1

die() {
  echo "ERROR: $*" >&2
  exit 1
}

# Header — same vibe as scripts/bench.sh
echo "============================================="
echo "Ranvier Push-Cache-Eviction A/B"
echo "Started: $(date)"
echo "Host: $(hostname 2>/dev/null || uname -n 2>/dev/null || echo unknown)"
echo "User: $(whoami 2>/dev/null || echo unknown)"
echo "PWD: $(pwd)"
echo "Command: $ORIGINAL_CMD"
echo "Git Commit: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "---------------------------------------------"
echo "Ranvier binary: $RANVIER_BIN"
echo "Ranvier args:   $RANVIER_ARGS"
echo "Proxy URL:      $PROXY_URL"
echo "Metrics URL:    $METRICS_URL"
echo "Run dir:        $AB_DIR"
echo "---------------------------------------------"
echo "Harness environment overrides:"
env | grep -E '^(REQUESTS|CAPACITY|POPULATION|ZIPF_S|SEED|NUM_BACKENDS|BACKEND_PORT_BASE|PROXY_URL|METRICS_URL)=' | sort || echo "  (none)"
echo "============================================="
echo

# Sanity: binary exists and is executable.
if [[ ! -x "$RANVIER_BIN" ]]; then
  die "Ranvier binary not found or not executable: $RANVIER_BIN
       Override with RANVIER_BIN=/path/to/ranvier_server."
fi

# State
RANVIER_PID=""

stop_ranvier() {
  local pid="$RANVIER_PID"
  [[ -z "$pid" ]] && return 0
  if kill -0 "$pid" 2>/dev/null; then
    echo "==> Stopping Ranvier (pid=$pid, graceful SIGTERM)"
    kill -TERM "$pid" 2>/dev/null || true
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
      sleep 0.5
      waited=$((waited + 1))
      if (( waited >= RANVIER_SHUTDOWN_TIMEOUT_S * 2 )); then
        echo "    still alive after ${RANVIER_SHUTDOWN_TIMEOUT_S}s, SIGKILL"
        kill -KILL "$pid" 2>/dev/null || true
        break
      fi
    done
    wait "$pid" 2>/dev/null || true
  fi
  RANVIER_PID=""
}

cleanup_on_exit() {
  stop_ranvier
}
trap cleanup_on_exit EXIT INT TERM

start_ranvier() {
  local leg="$1"
  local extra_env="$2"
  local log_file="$AB_DIR/ranvier-$leg.log"
  echo "==> Starting Ranvier for leg=$leg"
  echo "    log: $log_file"
  if [[ -n "$extra_env" ]]; then
    echo "    env: $extra_env"
  fi
  echo "    cmd: $RANVIER_BIN $RANVIER_ARGS"

  # Launch with the leg-specific env prepended. `env` runs the binary
  # with the extra env var set; empty extra_env passes unchanged.
  # shellcheck disable=SC2086
  env $extra_env "$RANVIER_BIN" $RANVIER_ARGS >"$log_file" 2>&1 &
  RANVIER_PID=$!
  echo "    pid: $RANVIER_PID"

  # Wait for /health (poll up to $RANVIER_STARTUP_TIMEOUT_S).
  local deadline=$(( SECONDS + RANVIER_STARTUP_TIMEOUT_S ))
  while (( SECONDS < deadline )); do
    if ! kill -0 "$RANVIER_PID" 2>/dev/null; then
      echo "    Ranvier exited before becoming ready — see $log_file"
      die "Ranvier for leg=$leg died during startup"
    fi
    if curl -sS -o /dev/null "$PROXY_URL/health" 2>/dev/null; then
      echo "    Ranvier ready"
      return 0
    fi
    sleep 0.2
  done
  die "Ranvier for leg=$leg did not become ready within ${RANVIER_STARTUP_TIMEOUT_S}s (log: $log_file)"
}

run_leg() {
  local leg="$1"
  echo
  echo "============================================="
  echo "LEG: $leg"
  echo "============================================="
  # accept_client_tokens is required in BOTH legs because the harness
  # sends `prompt_token_ids` in every request so it can compute the
  # exact same prefix hash Ranvier stores in its reverse index.
  local extra_env="RANVIER_ACCEPT_CLIENT_TOKENS=1"
  if [[ "$leg" == "push" ]]; then
    extra_env="$extra_env RANVIER_CACHE_EVENTS_ENABLED=1"
  fi

  start_ranvier "$leg" "$extra_env"

  echo "==> Running run.sh with MODE=$leg RESULTS_DIR=$AB_DIR"
  MODE="$leg" RESULTS_DIR="$AB_DIR" "$HERE/run.sh"
  local rc=$?
  if [[ $rc -ne 0 ]]; then
    stop_ranvier
    die "run.sh leg=$leg failed (rc=$rc)"
  fi

  stop_ranvier
}

run_leg silent
run_leg push

echo
echo "============================================="
echo "A/B complete"
echo "Finished: $(date)"
echo "---------------------------------------------"
echo "Result files:"
echo "  $AB_DIR/result-silent.json"
echo "  $AB_DIR/result-push.json"
echo
echo "Ranvier logs:"
echo "  $AB_DIR/ranvier-silent.log"
echo "  $AB_DIR/ranvier-push.log"
echo
echo "Full run log:"
echo "  $RUN_LOG"
echo
echo "Diff the two result files to see the prefill-savings delta:"
echo "  diff <(jq -S . $AB_DIR/result-silent.json) \\"
echo "       <(jq -S . $AB_DIR/result-push.json)"
echo "============================================="
