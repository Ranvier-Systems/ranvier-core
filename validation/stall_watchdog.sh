#!/usr/bin/env bash
# =============================================================================
# Ranvier Core Validation Suite - Stall Watchdog Runner
# =============================================================================
# Launches Ranvier with aggressive Seastar stall detection to catch any
# reactor stalls that would indicate blocking operations on the hot path.
#
# Seastar Flags Used:
#   --task-quota-ms 0.1       : Task time slice of 100μs (catches micro-stalls)
#
# Exit Codes:
#   0 - No stalls detected
#   1 - Stalls detected or test failed
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
STALL_LOG_FILE="${VALIDATION_LOG_DIR}/stall_watchdog_$(date +%Y%m%d_%H%M%S).log"
LOAD_DURATION="${LOAD_DURATION:-60s}"
WARMUP_SECONDS="${WARMUP_SECONDS:-10}"

# Seastar stall detection configuration
TASK_QUOTA_MS="${TASK_QUOTA_MS:-0.1}"

# Additional Seastar flags for diagnostics
SEASTAR_POLL_MODE="${SEASTAR_POLL_MODE:-}"  # --poll-mode for spinloop (better stall detection)
SEASTAR_IDLE_POLL="${SEASTAR_IDLE_POLL:-}"  # --idle-poll-time-us

# -----------------------------------------------------------------------------
# Functions
# -----------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  -b, --binary PATH        Path to Ranvier binary (default: $RANVIER_BINARY)
  -c, --config PATH        Path to config file (default: $RANVIER_CONFIG)
  -d, --duration TIME      Load test duration (default: $LOAD_DURATION)
  -w, --warmup SECONDS     Warmup period before load (default: $WARMUP_SECONDS)
  -q, --task-quota MS      Seastar task quota in ms (default: $TASK_QUOTA_MS)
  --poll-mode              Enable Seastar poll mode (spinloop)
  -h, --help               Show this help message

Environment Variables:
  RANVIER_BINARY           Path to Ranvier binary
  RANVIER_CONFIG           Path to configuration file
  RANVIER_API_PORT         API port (default: 8080)
  WRK_CONNECTIONS          Number of wrk connections (default: 100)
  WRK_THREADS              Number of wrk threads (default: 4)

Example:
  $(basename "$0") --duration 120s --task-quota 0.05

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
            -d|--duration)
                LOAD_DURATION="$2"
                shift 2
                ;;
            -w|--warmup)
                WARMUP_SECONDS="$2"
                shift 2
                ;;
            -q|--task-quota)
                TASK_QUOTA_MS="$2"
                shift 2
                ;;
            --poll-mode)
                SEASTAR_POLL_MODE="--poll-mode"
                shift
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

build_seastar_args() {
    local args=(
        "--task-quota-ms" "$TASK_QUOTA_MS"
    )

    # Optional poll mode for better stall detection
    if [[ -n "$SEASTAR_POLL_MODE" ]]; then
        args+=("$SEASTAR_POLL_MODE")
    fi

    # Set idle poll time if specified
    if [[ -n "$SEASTAR_IDLE_POLL" ]]; then
        args+=("--idle-poll-time-us" "$SEASTAR_IDLE_POLL")
    fi

    echo "${args[*]}"
}

start_ranvier_with_stall_detection() {
    log_info "Starting Ranvier with stall detection enabled..."

    local seastar_args
    seastar_args=$(build_seastar_args)

    mkdir -p "$(dirname "$STALL_LOG_FILE")"

    log_info "Seastar arguments: $seastar_args"
    log_info "Log file: $STALL_LOG_FILE"

    # Start Ranvier in background with stall detection
    # Use process substitution to correctly capture Ranvier's PID (not tee's)
    # shellcheck disable=SC2086
    "$RANVIER_BINARY" \
        --config "$RANVIER_CONFIG" \
        $seastar_args \
        > >(tee "$STALL_LOG_FILE") 2>&1 &

    local ranvier_pid=$!
    register_cleanup "$ranvier_pid"

    log_info "Ranvier started with PID: $ranvier_pid"

    # Wait for API port to be ready (Ranvier doesn't have a /health endpoint)
    if ! wait_for_port "$RANVIER_API_PORT" 60; then
        log_error "Ranvier failed to start"
        return 1
    fi

    echo "$ranvier_pid"
}

generate_load() {
    local duration="$1"

    log_info "Generating load for $duration..."

    # Use wrk if available, otherwise fall back to curl-based load
    if command -v wrk &>/dev/null; then
        log_info "Using wrk for load generation"

        # Create a simple Lua script for POST requests
        local lua_script="${VALIDATION_LOG_DIR}/wrk_stall_test.lua"
        cat > "$lua_script" <<'LUAEOF'
-- wrk Lua script for Ranvier load testing
wrk.method = "POST"
wrk.body   = '{"model": "test", "messages": [{"role": "user", "content": "test"}]}'
wrk.headers["Content-Type"] = "application/json"

-- Track request stats
local counter = 0
local errors = 0

function response(status, headers, body)
    counter = counter + 1
    if status >= 400 then
        errors = errors + 1
    end
end

function done(summary, latency, requests)
    io.write("------------------------------\n")
    io.write(string.format("Requests/sec: %.2f\n", summary.requests / summary.duration * 1000000))
    io.write(string.format("Transfer/sec: %.2fKB\n", summary.bytes / summary.duration * 1000000 / 1024))
    io.write(string.format("Errors: %d (%.2f%%)\n", errors, errors / counter * 100))
end
LUAEOF

        wrk \
            -t"$WRK_THREADS" \
            -c"$WRK_CONNECTIONS" \
            -d"$duration" \
            -s "$lua_script" \
            --latency \
            "http://127.0.0.1:${RANVIER_API_PORT}/v1/chat/completions" 2>&1 | \
            tee "${VALIDATION_LOG_DIR}/wrk_stall_output.txt"

    else
        log_warn "wrk not found, using curl-based load generation"

        # Simple curl-based load generation with timeout
        local duration_seconds
        duration_seconds=$(echo "$duration" | sed 's/[^0-9]//g')
        local end_time=$((SECONDS + duration_seconds))

        local count=0
        while [[ $SECONDS -lt $end_time ]]; do
            for _ in $(seq 1 10); do
                # Use short timeout since we're just generating load, not waiting for responses
                curl -sf --max-time 2 -X POST \
                    -H "Content-Type: application/json" \
                    -d '{"model": "test", "messages": [{"role": "user", "content": "test"}]}' \
                    "http://127.0.0.1:${RANVIER_API_PORT}/v1/chat/completions" \
                    >/dev/null 2>&1 &
            done
            # Don't wait for all - just throttle slightly to avoid fork bomb
            sleep 0.1
            count=$((count + 10))
        done

        # Wait for remaining curl processes
        wait 2>/dev/null || true

        log_info "Completed $count requests"
    fi
}

analyze_stalls() {
    local log_file="$1"

    log_info "Analyzing stall log..."

    # Count stall occurrences
    local stall_count
    stall_count=$(grep -c "Reactor stall" "$log_file" 2>/dev/null || echo "0")

    log_info "Found $stall_count reactor stall(s)"

    if [[ "$stall_count" -gt "$STALL_THRESHOLD" ]]; then
        log_error "STALL THRESHOLD EXCEEDED: $stall_count > $STALL_THRESHOLD"

        # Extract stall details
        log_info "Stall details:"
        grep -A 20 "Reactor stall" "$log_file" | head -100 || true

        return 1
    fi

    # Check for other concerning patterns
    local blocked_count
    blocked_count=$(grep -c "blocked reactor" "$log_file" 2>/dev/null || echo "0")

    if [[ "$blocked_count" -gt 0 ]]; then
        log_warn "Found $blocked_count 'blocked reactor' messages"
        grep -B 2 -A 10 "blocked reactor" "$log_file" | head -50 || true
    fi

    # Look for sleep/blocking syscall warnings
    local sleep_count
    sleep_count=$(grep -c -E "(sleep|nanosleep|usleep|blocking)" "$log_file" 2>/dev/null || echo "0")

    if [[ "$sleep_count" -gt 0 ]]; then
        log_warn "Found $sleep_count potential blocking calls"
    fi

    log_success "No reactor stalls detected within threshold"
    return 0
}

generate_report() {
    local stall_count="$1"
    local status="$2"

    cat <<EOF
{
  "stall_count": $stall_count,
  "threshold": $STALL_THRESHOLD,
  "task_quota_ms": $TASK_QUOTA_MS,
  "load_duration": "$LOAD_DURATION",
  "log_file": "$STALL_LOG_FILE"
}
EOF
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
    parse_args "$@"

    log_section "Stall Watchdog Test"

    # Verify dependencies
    check_dependencies curl nc

    # Verify binary exists
    if [[ ! -x "$RANVIER_BINARY" ]]; then
        log_error "Ranvier binary not found or not executable: $RANVIER_BINARY"
        exit 1
    fi

    # Start Ranvier with stall detection
    local ranvier_pid
    ranvier_pid=$(start_ranvier_with_stall_detection)

    if [[ -z "$ranvier_pid" ]]; then
        log_error "Failed to start Ranvier"
        exit 1
    fi

    # Warmup period
    log_info "Warmup period: ${WARMUP_SECONDS}s"
    sleep "$WARMUP_SECONDS"

    # Generate load
    generate_load "$LOAD_DURATION"

    # Stop Ranvier gracefully
    log_info "Stopping Ranvier..."
    kill -TERM "$ranvier_pid" 2>/dev/null || true
    sleep 3

    # Analyze results
    local stall_count
    stall_count=$(grep -c "Reactor stall" "$STALL_LOG_FILE" 2>/dev/null || echo "0")

    if analyze_stalls "$STALL_LOG_FILE"; then
        log_success "STALL WATCHDOG TEST: PASSED"
        generate_report "$stall_count" "PASS"
        exit 0
    else
        log_error "STALL WATCHDOG TEST: FAILED"
        generate_report "$stall_count" "FAIL"
        exit 1
    fi
}

# Run if executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
