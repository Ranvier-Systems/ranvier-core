#!/usr/bin/env bash
# =============================================================================
# Ranvier Core Validation Suite - Disk Latency Injection Test
# =============================================================================
# Validates that the async I/O refactor successfully decouples the hot path
# from disk operations. Uses stress-ng or Python-based I/O stress to saturate
# the disk while measuring proxy response times.
#
# Success Criteria:
#   P99 response time should not increase by more than 10% during disk stress
#   (configurable via P99_LATENCY_DEGRADATION_THRESHOLD)
#
# Exit Codes:
#   0 - Latency within acceptable degradation threshold
#   1 - Latency degradation exceeded threshold
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
DISK_STRESS_DURATION="${DISK_STRESS_DURATION:-60s}"
BASELINE_DURATION="${BASELINE_DURATION:-30s}"
STRESS_DURATION="${STRESS_DURATION:-60s}"
DISK_PATH="${DISK_PATH:-$(dirname "$RANVIER_DB_PATH")}"

# stress-ng configuration
STRESS_NG_HDD_WORKERS="${STRESS_NG_HDD_WORKERS:-4}"
STRESS_NG_HDD_BYTES="${STRESS_NG_HDD_BYTES:-1G}"
STRESS_NG_IO_WORKERS="${STRESS_NG_IO_WORKERS:-2}"

# Python I/O stress configuration (fallback)
PYTHON_IO_FILE_SIZE="${PYTHON_IO_FILE_SIZE:-104857600}"  # 100MB
PYTHON_IO_ITERATIONS="${PYTHON_IO_ITERATIONS:-1000}"

# Output files
BASELINE_OUTPUT="${VALIDATION_LOG_DIR}/disk_stress_baseline.txt"
STRESS_OUTPUT="${VALIDATION_LOG_DIR}/disk_stress_under_load.txt"
STRESS_NG_LOG="${VALIDATION_LOG_DIR}/stress_ng.log"

# -----------------------------------------------------------------------------
# Functions
# -----------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  -b, --binary PATH        Path to Ranvier binary (default: $RANVIER_BINARY)
  -c, --config PATH        Path to config file (default: $RANVIER_CONFIG)
  -p, --disk-path PATH     Path to stress disk (default: same as DB path)
  -t, --threshold PERCENT  Max allowed P99 degradation (default: $P99_LATENCY_DEGRADATION_THRESHOLD%)
  --baseline-duration TIME Baseline measurement duration (default: $BASELINE_DURATION)
  --stress-duration TIME   Stress test duration (default: $STRESS_DURATION)
  --hdd-workers N          Number of stress-ng HDD workers (default: $STRESS_NG_HDD_WORKERS)
  -h, --help               Show this help message

Environment Variables:
  RANVIER_BINARY           Path to Ranvier binary
  RANVIER_CONFIG           Path to configuration file
  RANVIER_DB_PATH          Path to SQLite database file
  P99_LATENCY_DEGRADATION_THRESHOLD  Max P99 degradation percentage

Example:
  $(basename "$0") --threshold 15 --stress-duration 120s

EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -b|--binary)
                RANVIER_BINARY="$2"
                shift 2
                ;;
            -c|--config)
                RANVIER_CONFIG="$2"
                shift 2
                ;;
            -p|--disk-path)
                DISK_PATH="$2"
                shift 2
                ;;
            -t|--threshold)
                P99_LATENCY_DEGRADATION_THRESHOLD="$2"
                shift 2
                ;;
            --baseline-duration)
                BASELINE_DURATION="$2"
                shift 2
                ;;
            --stress-duration)
                STRESS_DURATION="$2"
                shift 2
                ;;
            --hdd-workers)
                STRESS_NG_HDD_WORKERS="$2"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
}

create_python_io_stressor() {
    local script_path="${VALIDATION_LOG_DIR}/io_stressor.py"

    cat > "$script_path" <<'PYEOF'
#!/usr/bin/env python3
"""
Disk I/O Stressor - Saturates disk with heavy write/sync operations.
Designed to stress the same disk where SQLite WAL files reside.
"""

import os
import sys
import time
import signal
import fcntl
import tempfile
import threading
from pathlib import Path

# Configuration from environment
FILE_SIZE = int(os.environ.get('PYTHON_IO_FILE_SIZE', 104857600))  # 100MB
ITERATIONS = int(os.environ.get('PYTHON_IO_ITERATIONS', 1000))
DISK_PATH = os.environ.get('DISK_PATH', '/tmp')
WORKER_COUNT = int(os.environ.get('PYTHON_IO_WORKERS', 4))

running = True

def signal_handler(sig, frame):
    global running
    print("\nShutdown signal received, stopping workers...")
    running = False

def io_worker(worker_id: int, path: str):
    """Heavy I/O worker that performs writes with fsync and flock."""
    global running

    data = os.urandom(4096)  # 4KB random data blocks
    temp_file = os.path.join(path, f'stress_io_worker_{worker_id}.tmp')

    stats = {'writes': 0, 'syncs': 0, 'locks': 0, 'bytes': 0}

    try:
        with open(temp_file, 'wb') as f:
            while running:
                # Acquire exclusive lock (simulates SQLite WAL behavior)
                try:
                    fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    stats['locks'] += 1
                except BlockingIOError:
                    pass  # Lock contention, continue anyway

                # Write data in chunks
                for _ in range(min(ITERATIONS, 100)):
                    if not running:
                        break

                    # Random seek to create fragmentation
                    offset = (stats['writes'] * 4096) % FILE_SIZE
                    f.seek(offset)
                    f.write(data)
                    stats['writes'] += 1
                    stats['bytes'] += len(data)

                # Force sync to disk (this creates real I/O wait)
                f.flush()
                os.fsync(f.fileno())
                stats['syncs'] += 1

                # Release lock
                try:
                    fcntl.flock(f.fileno(), fcntl.LOCK_UN)
                except OSError:
                    pass

                # Small delay to prevent CPU saturation
                time.sleep(0.001)

    except Exception as e:
        print(f"Worker {worker_id} error: {e}", file=sys.stderr)
    finally:
        # Cleanup temp file
        try:
            os.unlink(temp_file)
        except OSError:
            pass

    print(f"Worker {worker_id}: writes={stats['writes']}, syncs={stats['syncs']}, "
          f"bytes={stats['bytes']/(1024*1024):.1f}MB")

def main():
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Ensure disk path exists
    Path(DISK_PATH).mkdir(parents=True, exist_ok=True)

    print(f"Starting {WORKER_COUNT} I/O stress workers on {DISK_PATH}")
    print(f"Target file size: {FILE_SIZE/(1024*1024):.1f}MB per worker")

    threads = []
    for i in range(WORKER_COUNT):
        t = threading.Thread(target=io_worker, args=(i, DISK_PATH), daemon=True)
        t.start()
        threads.append(t)

    # Wait for signal or threads
    try:
        while running and any(t.is_alive() for t in threads):
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass

    print("Waiting for workers to finish...")
    for t in threads:
        t.join(timeout=5)

    print("I/O stress test complete")

if __name__ == '__main__':
    main()
PYEOF

    chmod +x "$script_path"
    echo "$script_path"
}

start_stress_ng() {
    log_info "Starting stress-ng disk stressor..."

    local duration_seconds
    duration_seconds=$(echo "$STRESS_DURATION" | sed 's/[^0-9]//g')

    # Create a directory for stress-ng temp files
    mkdir -p "${DISK_PATH}/stress_tmp"

    # Start stress-ng with HDD and I/O stress
    stress-ng \
        --hdd "$STRESS_NG_HDD_WORKERS" \
        --hdd-bytes "$STRESS_NG_HDD_BYTES" \
        --hdd-write-size 4K \
        --io "$STRESS_NG_IO_WORKERS" \
        --temp-path "${DISK_PATH}/stress_tmp" \
        --timeout "${duration_seconds}s" \
        --metrics-brief \
        2>&1 | tee "$STRESS_NG_LOG" &

    local stress_pid=$!
    register_cleanup "$stress_pid"

    log_info "stress-ng started with PID: $stress_pid"
    echo "$stress_pid"
}

start_python_stressor() {
    log_info "Starting Python I/O stressor (stress-ng not available)..."

    local script_path
    script_path=$(create_python_io_stressor)

    export DISK_PATH
    export PYTHON_IO_FILE_SIZE
    export PYTHON_IO_ITERATIONS
    export PYTHON_IO_WORKERS=4

    python3 "$script_path" &

    local stress_pid=$!
    register_cleanup "$stress_pid"

    log_info "Python I/O stressor started with PID: $stress_pid"
    echo "$stress_pid"
}

start_disk_stress() {
    if command -v stress-ng &>/dev/null; then
        start_stress_ng
    else
        log_warn "stress-ng not found, falling back to Python I/O stressor"
        start_python_stressor
    fi
}

run_wrk_benchmark() {
    local output_file="$1"
    local duration="$2"
    local label="$3"

    log_info "Running $label benchmark for $duration..."

    mkdir -p "$(dirname "$output_file")"

    # Create Lua script for consistent benchmarking
    local lua_script="${VALIDATION_LOG_DIR}/wrk_disk_test.lua"
    cat > "$lua_script" <<'LUAEOF'
wrk.method = "POST"
wrk.body   = '{"model": "test", "messages": [{"role": "user", "content": "test request for latency measurement"}]}'
wrk.headers["Content-Type"] = "application/json"
LUAEOF

    if command -v wrk &>/dev/null; then
        wrk \
            -t"$WRK_THREADS" \
            -c"$WRK_CONNECTIONS" \
            -d"$duration" \
            -s "$lua_script" \
            --latency \
            "http://127.0.0.1:${RANVIER_API_PORT}/v1/chat/completions" 2>&1 | tee "$output_file"
    else
        log_error "wrk is required for this test"
        return 1
    fi
}

extract_p99_latency() {
    local output_file="$1"

    # Extract P99 from wrk output (format: "99%   XXX.XXms")
    local p99
    p99=$(grep -E "^\s*99%" "$output_file" | awk '{print $2}' | head -1)

    if [[ -z "$p99" ]]; then
        log_error "Could not extract P99 latency from $output_file"
        return 1
    fi

    # Convert to milliseconds (handle us, ms, s suffixes)
    local value unit
    value=$(echo "$p99" | sed 's/[^0-9.]//g')
    unit=$(echo "$p99" | sed 's/[0-9.]//g')

    case "$unit" in
        us)
            echo "scale=3; $value / 1000" | bc
            ;;
        ms)
            echo "$value"
            ;;
        s)
            echo "scale=3; $value * 1000" | bc
            ;;
        *)
            echo "$value"
            ;;
    esac
}

analyze_latency_degradation() {
    local baseline_p99="$1"
    local stress_p99="$2"

    log_info "Baseline P99: ${baseline_p99}ms"
    log_info "Under Stress P99: ${stress_p99}ms"

    local degradation
    degradation=$(calculate_percentage_change "$baseline_p99" "$stress_p99")

    log_info "Latency degradation: ${degradation}%"

    if (( $(echo "$degradation > $P99_LATENCY_DEGRADATION_THRESHOLD" | bc -l) )); then
        log_error "LATENCY DEGRADATION EXCEEDED: ${degradation}% > ${P99_LATENCY_DEGRADATION_THRESHOLD}%"
        return 1
    fi

    log_success "Latency degradation within threshold: ${degradation}% <= ${P99_LATENCY_DEGRADATION_THRESHOLD}%"
    return 0
}

start_ranvier() {
    log_info "Starting Ranvier for disk stress test..."

    "$RANVIER_BINARY" --config "$RANVIER_CONFIG" &

    local ranvier_pid=$!
    register_cleanup "$ranvier_pid"

    # Wait for API port (Ranvier doesn't have a /health endpoint)
    if ! wait_for_port "$RANVIER_API_PORT" 60; then
        log_error "Ranvier failed to start"
        return 1
    fi

    log_success "Ranvier started with PID: $ranvier_pid"
    echo "$ranvier_pid"
}

generate_report() {
    local baseline_p99="$1"
    local stress_p99="$2"
    local degradation="$3"
    local status="$4"

    cat <<EOF
{
  "baseline_p99_ms": $baseline_p99,
  "stress_p99_ms": $stress_p99,
  "degradation_percent": $degradation,
  "threshold_percent": $P99_LATENCY_DEGRADATION_THRESHOLD,
  "disk_path": "$DISK_PATH",
  "stress_duration": "$STRESS_DURATION",
  "baseline_output": "$BASELINE_OUTPUT",
  "stress_output": "$STRESS_OUTPUT"
}
EOF
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
    parse_args "$@"

    log_section "Disk I/O Decoupling Test"

    # Verify dependencies
    check_dependencies wrk curl nc bc

    # Verify binary exists
    if [[ ! -x "$RANVIER_BINARY" ]]; then
        log_error "Ranvier binary not found or not executable: $RANVIER_BINARY"
        exit 1
    fi

    mkdir -p "$VALIDATION_LOG_DIR"

    # Start Ranvier
    local ranvier_pid
    ranvier_pid=$(start_ranvier)

    if [[ -z "$ranvier_pid" ]]; then
        log_error "Failed to start Ranvier"
        exit 1
    fi

    # Warmup
    log_info "Warmup period: 10s"
    sleep 10

    # Phase 1: Baseline measurement (no disk stress)
    log_section "Phase 1: Baseline Measurement"
    run_wrk_benchmark "$BASELINE_OUTPUT" "$BASELINE_DURATION" "Baseline"

    local baseline_p99
    baseline_p99=$(extract_p99_latency "$BASELINE_OUTPUT")

    if [[ -z "$baseline_p99" ]]; then
        log_error "Failed to extract baseline P99"
        exit 1
    fi

    log_info "Baseline P99 latency: ${baseline_p99}ms"

    # Phase 2: Measurement under disk stress
    log_section "Phase 2: Measurement Under Disk Stress"

    # Start disk stress
    local stress_pid
    stress_pid=$(start_disk_stress)

    # Wait for stress to ramp up
    log_info "Waiting for disk stress to ramp up..."
    sleep 5

    # Run benchmark under stress
    run_wrk_benchmark "$STRESS_OUTPUT" "$STRESS_DURATION" "Under Stress"

    local stress_p99
    stress_p99=$(extract_p99_latency "$STRESS_OUTPUT")

    if [[ -z "$stress_p99" ]]; then
        log_error "Failed to extract stress P99"
        exit 1
    fi

    # Stop stress
    log_info "Stopping disk stress..."
    kill -TERM "$stress_pid" 2>/dev/null || true

    # Analyze results
    log_section "Results Analysis"

    local degradation
    degradation=$(calculate_percentage_change "$baseline_p99" "$stress_p99")

    if analyze_latency_degradation "$baseline_p99" "$stress_p99"; then
        log_success "DISK I/O DECOUPLING TEST: PASSED"
        generate_report "$baseline_p99" "$stress_p99" "$degradation" "PASS"
        exit 0
    else
        log_error "DISK I/O DECOUPLING TEST: FAILED"
        generate_report "$baseline_p99" "$stress_p99" "$degradation" "FAIL"
        exit 1
    fi
}

# Run if executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
