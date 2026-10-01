#!/usr/bin/env bash
# git bisect helper: runs the benchmark and exits 0 (good) or 1 (bad)
# based on whether P99 latency exceeds a threshold.
#
# Usage:
#   git bisect start BAD GOOD
#   git bisect run tests/integration/bisect-benchmark.sh [P99_THRESHOLD_MS]
#
# Example:
#   git bisect start 91c6083 e0f0dbb
#   git bisect run tests/integration/bisect-benchmark.sh 100
#
# Default threshold: 100ms (midpoint between good ~85ms and bad ~120ms)
# Exit codes:
#   0   = good (P99 below threshold)
#   1   = bad  (P99 above threshold)
#   125 = skip (build failed or benchmark couldn't run)

set -euo pipefail

THRESHOLD_MS="${1:-100}"
COMPOSE_FILE="docker-compose.test.yml"
RESULTS_DIR="$(mktemp -d)/benchmark-results"
BENCHMARK_USERS="${BENCHMARK_USERS:-100}"
BENCHMARK_DURATION="${BENCHMARK_DURATION:-60s}"
BENCHMARK_SPAWN_RATE="${BENCHMARK_SPAWN_RATE:-10}"

echo "=== Bisect: testing commit $(git rev-parse --short HEAD) ==="
echo "=== P99 threshold: ${THRESHOLD_MS}ms ==="

# Step 1: Build the Docker image for this commit
echo "Building Docker image..."
if ! make docker-build 2>&1 | tail -5; then
    echo "Build failed — skipping this commit"
    exit 125
fi

# Step 2: Start the test environment
echo "Starting test environment..."
# ranvier2/ranvier3 are gated behind the 'full' profile and locust depends on
# them, so the profile is required here exactly as in benchmark.yml.
docker compose -f "$COMPOSE_FILE" --profile full down -v --remove-orphans 2>/dev/null || true
if ! docker compose -f "$COMPOSE_FILE" --profile full up -d --wait; then
    echo "Failed to start services — skipping"
    docker compose -f "$COMPOSE_FILE" --profile full down -v --remove-orphans 2>/dev/null || true
    exit 125
fi

# docker compose --wait already ensures all health checks pass

# Step 3: Run the benchmark
echo "Running benchmark (${BENCHMARK_USERS} users, ${BENCHMARK_DURATION})..."
mkdir -p "$RESULTS_DIR"
chmod 777 "$RESULTS_DIR"

# `|| true`: locust exits 1 on any request error or its own P99 check; under
# `set -euo pipefail` that would abort before the CSV is read and mark the
# commit "bad" on a different metric than the threshold below.
docker compose -f "$COMPOSE_FILE" --profile full run --rm \
    -e RANVIER_NODE1=http://172.28.2.1:8080 \
    -e RANVIER_NODE2=http://172.28.2.2:8080 \
    -e RANVIER_NODE3=http://172.28.2.3:8080 \
    -e RANVIER_METRICS1=http://172.28.2.1:9180 \
    -e RANVIER_METRICS2=http://172.28.2.2:9180 \
    -e RANVIER_METRICS3=http://172.28.2.3:9180 \
    -e BACKEND_IP=172.28.1.10 \
    -e BACKEND_PORT=8000 \
    -e LOCUST_WAIT_MIN=0.1 \
    -e LOCUST_WAIT_MAX=0.5 \
    -v "$RESULTS_DIR:/mnt/results" \
    locust \
      -f /mnt/locust/locustfile.py \
      --headless \
      -u "$BENCHMARK_USERS" \
      -r "$BENCHMARK_SPAWN_RATE" \
      -t "$BENCHMARK_DURATION" \
      --stop-timeout 10 \
      --csv=/mnt/results/benchmark \
      2>&1 | tail -20 || true

# Step 4: Extract P99 from CSV
if [ ! -f "$RESULTS_DIR/benchmark_stats.csv" ]; then
    echo "No benchmark results — skipping"
    docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
    exit 125
fi

# P99 is column 19 in the Aggregated row (Locust 2.24 emits a 98% column at 18).
P99=$(awk -F',' '/Aggregated/ { print $19 }' "$RESULTS_DIR/benchmark_stats.csv" | head -1)

# Step 5: Cleanup
docker compose -f "$COMPOSE_FILE" --profile full down -v --remove-orphans 2>/dev/null || true
rm -rf "$(dirname "$RESULTS_DIR")"

if [ -z "$P99" ]; then
    echo "Could not extract P99 — skipping"
    exit 125
fi

echo "=== P99: ${P99}ms (threshold: ${THRESHOLD_MS}ms) ==="

# Compare as integers (P99 from Locust is already in ms, integer)
P99_INT=$(printf "%.0f" "$P99")
if [ "$P99_INT" -le "$THRESHOLD_MS" ]; then
    echo "=== GOOD (P99 ${P99_INT}ms <= ${THRESHOLD_MS}ms) ==="
    exit 0
else
    echo "=== BAD (P99 ${P99_INT}ms > ${THRESHOLD_MS}ms) ==="
    exit 1
fi
