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

# macOS ships without `timeout`; use `gtimeout` from coreutils if available
if ! command -v timeout &>/dev/null; then
    if command -v gtimeout &>/dev/null; then
        timeout() { gtimeout "$@"; }
    else
        echo "ERROR: 'timeout' not found. On macOS run: brew install coreutils"
        exit 125
    fi
fi

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
docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
if ! docker compose -f "$COMPOSE_FILE" up -d --wait; then
    echo "Failed to start services — skipping"
    docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
    exit 125
fi

# Wait for health checks
echo "Waiting for services..."
sleep 10
for i in 1 2 3; do
    if ! timeout 30 bash -c "until curl -sf http://172.28.2.$i:8080/health > /dev/null 2>&1; do sleep 1; done"; then
        echo "ranvier$i failed health check — skipping"
        docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
        exit 125
    fi
done

# Step 3: Run the benchmark
echo "Running benchmark (${BENCHMARK_USERS} users, ${BENCHMARK_DURATION})..."
mkdir -p "$RESULTS_DIR"
chmod 777 "$RESULTS_DIR"

docker compose -f "$COMPOSE_FILE" run --rm \
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
      2>&1 | tail -20

# Step 4: Extract P99 from CSV
if [ ! -f "$RESULTS_DIR/benchmark_stats.csv" ]; then
    echo "No benchmark results — skipping"
    docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
    exit 125
fi

# P99 is column 18 (0-indexed: 17) in the Aggregated row
P99=$(awk -F',' '/Aggregated/ { print $18 }' "$RESULTS_DIR/benchmark_stats.csv" | head -1)

# Step 5: Cleanup
docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
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
