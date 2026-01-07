#!/usr/bin/env bash
# =============================================================================
# Ranvier Core Validation Suite - Common Library
# =============================================================================
# Shared utilities for all validation scripts
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration Defaults
# -----------------------------------------------------------------------------
export RANVIER_BINARY="${RANVIER_BINARY:-./build/ranvier_server}"
export RANVIER_CONFIG="${RANVIER_CONFIG:-./validation/config/test_config.yaml}"
export RANVIER_API_PORT="${RANVIER_API_PORT:-8080}"
export RANVIER_METRICS_PORT="${RANVIER_METRICS_PORT:-9180}"
export RANVIER_GOSSIP_PORT="${RANVIER_GOSSIP_PORT:-9190}"
export RANVIER_DB_PATH="${RANVIER_DB_PATH:-./validation/reports/ranvier_test.db}"

export VALIDATION_REPORTS_DIR="${VALIDATION_REPORTS_DIR:-./validation/reports}"
export VALIDATION_LOG_DIR="${VALIDATION_LOG_DIR:-./validation/reports/logs}"

# Test thresholds
export STALL_THRESHOLD="${STALL_THRESHOLD:-0}"
export P99_LATENCY_DEGRADATION_THRESHOLD="${P99_LATENCY_DEGRADATION_THRESHOLD:-10}"  # percent
export SMP_QUEUE_OVERFLOW_THRESHOLD="${SMP_QUEUE_OVERFLOW_THRESHOLD:-1000}"
export ATOMIC_INSTRUCTION_THRESHOLD="${ATOMIC_INSTRUCTION_THRESHOLD:-0}"

# Load test settings
export WRK_DURATION="${WRK_DURATION:-30s}"
export WRK_CONNECTIONS="${WRK_CONNECTIONS:-100}"
export WRK_THREADS="${WRK_THREADS:-4}"

# -----------------------------------------------------------------------------
# Color Output (exported for subshell access)
# -----------------------------------------------------------------------------
if [[ -t 1 ]]; then
    export RED='\033[0;31m'
    export GREEN='\033[0;32m'
    export YELLOW='\033[0;33m'
    export BLUE='\033[0;34m'
    export CYAN='\033[0;36m'
    export BOLD='\033[1m'
    export NC='\033[0m'
else
    export RED=''
    export GREEN=''
    export YELLOW=''
    export BLUE=''
    export CYAN=''
    export BOLD=''
    export NC=''
fi

# -----------------------------------------------------------------------------
# Logging Functions
# -----------------------------------------------------------------------------
log_info() {
    echo -e "${BLUE}[INFO]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*"
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*"
}

log_section() {
    echo ""
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo -e "${BOLD}${CYAN} $*${NC}"
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo ""
}

# -----------------------------------------------------------------------------
# Process Management
# -----------------------------------------------------------------------------
declare -a PIDS_TO_CLEANUP=()

cleanup_processes() {
    log_info "Cleaning up background processes..."
    for pid in "${PIDS_TO_CLEANUP[@]:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            log_info "Killing process $pid"
            kill -TERM "$pid" 2>/dev/null || true
            sleep 1
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    PIDS_TO_CLEANUP=()
}

register_cleanup() {
    local pid="$1"
    PIDS_TO_CLEANUP+=("$pid")
}

# Generic retry-with-timeout function
# Usage: wait_with_retry "description" timeout_seconds check_command [args...]
wait_with_retry() {
    local description="$1"
    local timeout="$2"
    shift 2
    local check_cmd=("$@")

    log_info "Waiting for $description (timeout: ${timeout}s)..."

    local count=0
    while ! "${check_cmd[@]}" 2>/dev/null; do
        sleep 1
        count=$((count + 1))
        if [[ $count -ge $timeout ]]; then
            log_error "Timeout waiting for $description"
            return 1
        fi
    done

    log_success "$description is available"
    return 0
}

wait_for_port() {
    local port="$1"
    local timeout="${2:-30}"
    local host="${3:-127.0.0.1}"

    wait_with_retry "port $port on $host" "$timeout" nc -z "$host" "$port"
}

wait_for_http() {
    local url="$1"
    local timeout="${2:-30}"

    wait_with_retry "HTTP endpoint $url" "$timeout" curl -sf "$url" -o /dev/null
}

# -----------------------------------------------------------------------------
# Metric Collection
# -----------------------------------------------------------------------------
fetch_prometheus_metric() {
    local metric_name="$1"
    local endpoint="${2:-http://127.0.0.1:${RANVIER_METRICS_PORT}/metrics}"

    curl -sf "$endpoint" 2>/dev/null | grep "^${metric_name}" | head -1 | awk '{print $2}'
}

fetch_prometheus_histogram_p99() {
    local metric_name="$1"
    local endpoint="${2:-http://127.0.0.1:${RANVIER_METRICS_PORT}/metrics}"

    # Fetch histogram and calculate P99 (approximation from bucket data)
    # P99 is the bucket where cumulative count first exceeds 99% of total
    curl -sf "$endpoint" 2>/dev/null | grep "${metric_name}_bucket" | \
        awk '
        BEGIN { n = 0 }
        {
            # Parse le="X" and count from prometheus format
            match($0, /le="([^"]+)"/, arr)
            le = arr[1]
            count = $NF
            if (le != "+Inf") {
                buckets[n] = le
                counts[n] = count
                n++
            } else {
                total = count
            }
        }
        END {
            if (total == 0) { print 0; exit }
            target = total * 0.99
            for (i = 0; i < n; i++) {
                if (counts[i] >= target) {
                    print buckets[i]
                    exit
                }
            }
            # Fallback to last bucket
            if (n > 0) print buckets[n-1]
        }
        '
}

# -----------------------------------------------------------------------------
# Report Generation
# -----------------------------------------------------------------------------
init_report() {
    local report_name="$1"
    local report_file="${VALIDATION_REPORTS_DIR}/${report_name}_$(date +%Y%m%d_%H%M%S).json"

    mkdir -p "$VALIDATION_REPORTS_DIR" "$VALIDATION_LOG_DIR"

    cat > "$report_file" <<EOF
{
  "report_name": "$report_name",
  "timestamp": "$(date -Iseconds)",
  "ranvier_version": "$(${RANVIER_BINARY} --version 2>/dev/null || echo 'unknown')",
  "tests": []
}
EOF
    echo "$report_file"
}

add_test_result() {
    local report_file="$1"
    local test_name="$2"
    local status="$3"  # "PASS" or "FAIL"
    local details="$4"

    # Use jq if available, otherwise Python
    if command -v jq &>/dev/null; then
        local tmp_file="${report_file}.tmp"
        jq ".tests += [{\"name\": \"$test_name\", \"status\": \"$status\", \"details\": $details}]" "$report_file" > "$tmp_file"
        mv "$tmp_file" "$report_file"
    else
        python3 -c "
import json
import sys
with open('$report_file', 'r') as f:
    data = json.load(f)
data['tests'].append({
    'name': '$test_name',
    'status': '$status',
    'details': $details
})
with open('$report_file', 'w') as f:
    json.dump(data, f, indent=2)
"
    fi
}

finalize_report() {
    local report_file="$1"

    # Calculate overall status
    local pass_count fail_count
    if command -v jq &>/dev/null; then
        pass_count=$(jq '[.tests[] | select(.status == "PASS")] | length' "$report_file")
        fail_count=$(jq '[.tests[] | select(.status == "FAIL")] | length' "$report_file")
    else
        pass_count=$(python3 -c "import json; data=json.load(open('$report_file')); print(len([t for t in data['tests'] if t['status']=='PASS']))")
        fail_count=$(python3 -c "import json; data=json.load(open('$report_file')); print(len([t for t in data['tests'] if t['status']=='FAIL']))")
    fi

    local overall_status="PASS"
    if [[ "$fail_count" -gt 0 ]]; then
        overall_status="FAIL"
    fi

    # Add summary
    if command -v jq &>/dev/null; then
        local tmp_file="${report_file}.tmp"
        jq ". + {\"summary\": {\"total\": $((pass_count + fail_count)), \"passed\": $pass_count, \"failed\": $fail_count, \"overall_status\": \"$overall_status\"}}" "$report_file" > "$tmp_file"
        mv "$tmp_file" "$report_file"
    else
        python3 -c "
import json
with open('$report_file', 'r') as f:
    data = json.load(f)
data['summary'] = {
    'total': $((pass_count + fail_count)),
    'passed': $pass_count,
    'failed': $fail_count,
    'overall_status': '$overall_status'
}
with open('$report_file', 'w') as f:
    json.dump(data, f, indent=2)
"
    fi

    echo "$overall_status"
}

# -----------------------------------------------------------------------------
# Utility Functions
# -----------------------------------------------------------------------------
check_dependencies() {
    local deps=("$@")
    local missing=()

    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &>/dev/null; then
            missing+=("$dep")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing dependencies: ${missing[*]}"
        log_info "Install with: apt-get install ${missing[*]}"
        return 1
    fi

    return 0
}

get_cpu_count() {
    nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4
}

calculate_percentage_change() {
    local baseline="$1"
    local current="$2"

    if [[ -z "$baseline" || "$baseline" == "0" ]]; then
        echo "0"
        return
    fi

    python3 -c "print(round((($current - $baseline) / $baseline) * 100, 2))"
}

# Trap for cleanup on exit
trap cleanup_processes EXIT INT TERM
