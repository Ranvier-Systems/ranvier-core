#!/usr/bin/env bash
# =============================================================================
# Ranvier Core - Production Readiness Validation Suite v1.0
# =============================================================================
# Integrated test runner that validates the architectural refactors have
# successfully eliminated bottlenecks and reactor stalls.
#
# Test Categories:
#   1. Reactor Stall Detection   - Catches micro-stalls with aggressive quotas
#   2. Disk I/O Decoupling       - Validates async persistence under disk stress
#   3. SMP Bus Pressure          - Tests gossip handling under high load
#   4. Atomic-Free Execution     - Verifies shared_ptr removal from hot paths
#
# Usage:
#   ./validate_v1.sh                    # Run all tests
#   ./validate_v1.sh --test stall       # Run specific test
#   ./validate_v1.sh --quick            # Quick validation (shorter durations)
#   ./validate_v1.sh --ci               # CI mode (strict thresholds)
#
# Exit Codes:
#   0 - All tests passed
#   1 - One or more tests failed
#   2 - Setup/configuration error
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

# =============================================================================
# Configuration
# =============================================================================
VERSION="1.0.0"
SUITE_NAME="Production Readiness v1.0"

# Test selection
RUN_STALL_TEST="${RUN_STALL_TEST:-true}"
RUN_DISK_TEST="${RUN_DISK_TEST:-true}"
RUN_GOSSIP_TEST="${RUN_GOSSIP_TEST:-true}"
RUN_ATOMIC_TEST="${RUN_ATOMIC_TEST:-true}"

# Mode presets
QUICK_MODE=""
CI_MODE=""
VERBOSE=""

# Test durations (seconds)
STALL_TEST_DURATION="${STALL_TEST_DURATION:-60}"
DISK_TEST_DURATION="${DISK_TEST_DURATION:-60}"
GOSSIP_TEST_DURATION="${GOSSIP_TEST_DURATION:-60}"

# Quick mode durations
QUICK_STALL_DURATION=30
QUICK_DISK_DURATION=30
QUICK_GOSSIP_DURATION=30

# CI mode thresholds (stricter)
CI_STALL_THRESHOLD=0
CI_LATENCY_THRESHOLD=5
CI_SMP_THRESHOLD=100
CI_ATOMIC_THRESHOLD=0

# Report file
REPORT_FILE="${VALIDATION_REPORTS_DIR}/validation_v1_$(date +%Y%m%d_%H%M%S).json"

# Track test results
declare -A TEST_RESULTS
declare -A TEST_DETAILS

# =============================================================================
# Functions
# =============================================================================
usage() {
    cat <<EOF
Ranvier Core - Production Readiness Validation Suite v${VERSION}

Usage: $(basename "$0") [OPTIONS]

Options:
  -b, --binary PATH        Path to Ranvier binary (default: $RANVIER_BINARY)
  -c, --config PATH        Path to config file (default: $RANVIER_CONFIG)
  -t, --test NAME          Run specific test (stall|disk|gossip|atomic)
  -q, --quick              Quick mode (shorter test durations)
  --ci                     CI mode (strict thresholds, machine-readable output)
  -v, --verbose            Verbose output
  -o, --output FILE        Output report file (default: auto-generated)
  --skip-stall             Skip reactor stall test
  --skip-disk              Skip disk I/O test
  --skip-gossip            Skip gossip storm test
  --skip-atomic            Skip atomic audit
  -h, --help               Show this help message

Environment Variables:
  RANVIER_BINARY           Path to Ranvier binary
  RANVIER_CONFIG           Path to configuration file
  RANVIER_API_PORT         API port (default: 8080)
  RANVIER_METRICS_PORT     Metrics port (default: 9180)
  RANVIER_GOSSIP_PORT      Gossip port (default: 9190)

Test Thresholds (environment):
  STALL_THRESHOLD          Max allowed reactor stalls (default: 0)
  P99_LATENCY_DEGRADATION_THRESHOLD  Max P99 increase % (default: 10)
  SMP_QUEUE_OVERFLOW_THRESHOLD       Max SMP queue overflows (default: 1000)
  ATOMIC_INSTRUCTION_THRESHOLD       Max atomic instructions (default: 0)

Examples:
  # Full validation suite
  $(basename "$0")

  # Quick validation for development
  $(basename "$0") --quick

  # CI pipeline with strict thresholds
  $(basename "$0") --ci --output results.json

  # Run only the stall test
  $(basename "$0") --test stall --verbose

  # Skip atomic audit (requires perf)
  $(basename "$0") --skip-atomic

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
            -t|--test)
                # Disable all, then enable specified
                RUN_STALL_TEST="false"
                RUN_DISK_TEST="false"
                RUN_GOSSIP_TEST="false"
                RUN_ATOMIC_TEST="false"
                case "$2" in
                    stall) RUN_STALL_TEST="true" ;;
                    disk) RUN_DISK_TEST="true" ;;
                    gossip) RUN_GOSSIP_TEST="true" ;;
                    atomic) RUN_ATOMIC_TEST="true" ;;
                    all)
                        RUN_STALL_TEST="true"
                        RUN_DISK_TEST="true"
                        RUN_GOSSIP_TEST="true"
                        RUN_ATOMIC_TEST="true"
                        ;;
                    *)
                        log_error "Unknown test: $2"
                        exit 2
                        ;;
                esac
                shift 2
                ;;
            -q|--quick)
                QUICK_MODE="1"
                STALL_TEST_DURATION=$QUICK_STALL_DURATION
                DISK_TEST_DURATION=$QUICK_DISK_DURATION
                GOSSIP_TEST_DURATION=$QUICK_GOSSIP_DURATION
                shift
                ;;
            --ci)
                CI_MODE="1"
                STALL_THRESHOLD=$CI_STALL_THRESHOLD
                P99_LATENCY_DEGRADATION_THRESHOLD=$CI_LATENCY_THRESHOLD
                SMP_QUEUE_OVERFLOW_THRESHOLD=$CI_SMP_THRESHOLD
                ATOMIC_INSTRUCTION_THRESHOLD=$CI_ATOMIC_THRESHOLD
                shift
                ;;
            -v|--verbose)
                VERBOSE="1"
                shift
                ;;
            -o|--output)
                REPORT_FILE="$2"
                shift 2
                ;;
            --skip-stall)
                RUN_STALL_TEST="false"
                shift
                ;;
            --skip-disk)
                RUN_DISK_TEST="false"
                shift
                ;;
            --skip-gossip)
                RUN_GOSSIP_TEST="false"
                shift
                ;;
            --skip-atomic)
                RUN_ATOMIC_TEST="false"
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 2
                ;;
        esac
    done
}

print_banner() {
    cat <<'EOF'
    ____                  _
   / __ \____ _____  _  _(_)__  _____
  / /_/ / __ `/ __ \| | / / _ \/ ___/
 / _, _/ /_/ / / / /| |/ /  __/ /
/_/ |_|\__,_/_/ /_/ |___/\___/_/

Production Readiness Validation Suite v1.0
EOF
    echo ""
}

verify_prerequisites() {
    log_section "Prerequisites Check"

    local errors=0

    # Check binary
    if [[ ! -x "$RANVIER_BINARY" ]]; then
        log_error "Ranvier binary not found or not executable: $RANVIER_BINARY"
        log_info "Build with: make build"
        errors=$((errors + 1))
    else
        log_success "Binary found: $RANVIER_BINARY"
    fi

    # Check config
    if [[ ! -f "$RANVIER_CONFIG" ]]; then
        log_warn "Config file not found: $RANVIER_CONFIG"
        log_info "Using default configuration"
    else
        log_success "Config found: $RANVIER_CONFIG"
    fi

    # Check required tools
    local required_tools=(curl nc awk grep)
    for tool in "${required_tools[@]}"; do
        if command -v "$tool" &>/dev/null; then
            log_success "Tool found: $tool"
        else
            log_error "Required tool missing: $tool"
            errors=$((errors + 1))
        fi
    done

    # Check optional tools
    local optional_tools=(wrk stress-ng perf objdump python3)
    for tool in "${optional_tools[@]}"; do
        if command -v "$tool" &>/dev/null; then
            log_success "Optional tool found: $tool"
        else
            log_warn "Optional tool missing: $tool (some tests may be limited)"
        fi
    done

    # Check port availability
    for port in "$RANVIER_API_PORT" "$RANVIER_METRICS_PORT" "$RANVIER_GOSSIP_PORT"; do
        if nc -z 127.0.0.1 "$port" 2>/dev/null; then
            log_warn "Port $port is already in use"
        fi
    done

    if [[ $errors -gt 0 ]]; then
        log_error "Prerequisites check failed with $errors error(s)"
        return 1
    fi

    log_success "All prerequisites satisfied"
    return 0
}

# -----------------------------------------------------------------------------
# Test Runner Helper
# -----------------------------------------------------------------------------
# Runs a test script and records results
# Usage: run_test "test_name" "display_name" test_command [args...]
run_test_wrapper() {
    local test_name="$1"
    local display_name="$2"
    shift 2

    local start_time
    start_time=$(date +%s)

    local output
    local exit_code=0

    output=$("$@" 2>&1) || exit_code=$?

    local end_time
    end_time=$(date +%s)
    local duration=$((end_time - start_time))

    if [[ $exit_code -eq 0 ]]; then
        TEST_RESULTS[$test_name]="PASS"
        log_success "$display_name: PASSED (${duration}s)"
    else
        TEST_RESULTS[$test_name]="FAIL"
        log_error "$display_name: FAILED (${duration}s)"
    fi

    TEST_DETAILS[$test_name]="$output"
    return $exit_code
}

# -----------------------------------------------------------------------------
# Test Runners
# -----------------------------------------------------------------------------
run_stall_test() {
    log_section "Test 1: Reactor Stall Detection"

    local test_name="reactor_stall_detection"

    log_info "Duration: ${STALL_TEST_DURATION}s"
    log_info "Task quota: ${TASK_QUOTA_MS:-0.1}ms"
    log_info "Stall threshold: $STALL_THRESHOLD"

    run_test_wrapper "$test_name" "Reactor Stall Test" \
        "${SCRIPT_DIR}/stall_watchdog.sh" \
        --binary "$RANVIER_BINARY" \
        --config "$RANVIER_CONFIG" \
        --duration "${STALL_TEST_DURATION}s" \
        --task-quota "${TASK_QUOTA_MS:-0.1}"
}

run_disk_test() {
    log_section "Test 2: Disk I/O Decoupling"

    local test_name="disk_io_decoupling"

    log_info "Duration: ${DISK_TEST_DURATION}s"
    log_info "Latency threshold: ${P99_LATENCY_DEGRADATION_THRESHOLD}%"

    # Check if wrk is available
    if ! command -v wrk &>/dev/null; then
        log_warn "wrk not available, skipping disk stress test"
        TEST_RESULTS[$test_name]="SKIP"
        TEST_DETAILS[$test_name]='{"reason": "wrk not installed"}'
        return 0
    fi

    run_test_wrapper "$test_name" "Disk I/O Decoupling Test" \
        "${SCRIPT_DIR}/disk_stress.sh" \
        --binary "$RANVIER_BINARY" \
        --config "$RANVIER_CONFIG" \
        --threshold "$P99_LATENCY_DEGRADATION_THRESHOLD" \
        --stress-duration "${DISK_TEST_DURATION}s"
}

run_gossip_test() {
    log_section "Test 3: SMP Gossip Storm"

    local test_name="smp_gossip_storm"
    local start_time
    start_time=$(date +%s)

    log_info "Duration: ${GOSSIP_TEST_DURATION}s"
    log_info "Target PPS: 5000"
    log_info "SMP threshold: $SMP_QUEUE_OVERFLOW_THRESHOLD"

    # Check Python3 is available
    if ! command -v python3 &>/dev/null; then
        log_warn "python3 not available, skipping gossip test"
        TEST_RESULTS[$test_name]="SKIP"
        TEST_DETAILS[$test_name]='{"reason": "python3 not installed"}'
        return 0
    fi

    # Check if cluster peers are configured
    # Ranvier only binds gossip port when peers are configured
    local has_peers="false"
    if [[ -f "$RANVIER_CONFIG" ]]; then
        # Check for non-empty peers list in YAML config
        if grep -E '^\s+peers:\s*$' "$RANVIER_CONFIG" &>/dev/null; then
            # Found peers: key, check if next lines have list items
            if grep -A5 '^\s+peers:' "$RANVIER_CONFIG" | grep -E '^\s+-\s+' &>/dev/null; then
                has_peers="true"
            fi
        fi
    fi

    if [[ "$has_peers" != "true" ]]; then
        log_warn "No cluster peers configured - gossip test requires multi-node setup"
        log_info "This test validates SMP queue handling under peer-to-peer gossip load"
        log_info "For single-node validation, this test is not applicable"
        TEST_RESULTS[$test_name]="SKIP"
        TEST_DETAILS[$test_name]='{"reason": "no cluster peers configured (single-node mode)"}'
        return 0
    fi

    # Start Ranvier for gossip test
    log_info "Starting Ranvier for gossip test..."
    "$RANVIER_BINARY" --config "$RANVIER_CONFIG" &
    local ranvier_pid=$!
    register_cleanup "$ranvier_pid"

    if ! wait_for_port "$RANVIER_GOSSIP_PORT" 30; then
        log_error "Ranvier gossip port not available"
        TEST_RESULTS[$test_name]="FAIL"
        TEST_DETAILS[$test_name]='{"error": "gossip port not available"}'
        return 1
    fi

    local output
    local exit_code=0

    output=$(python3 "${SCRIPT_DIR}/gossip_storm.py" \
        --target 127.0.0.1 \
        --port "$RANVIER_GOSSIP_PORT" \
        --pps 5000 \
        --duration "$GOSSIP_TEST_DURATION" \
        --threshold "$SMP_QUEUE_OVERFLOW_THRESHOLD" \
        --prometheus "http://127.0.0.1:${RANVIER_METRICS_PORT}/metrics" \
        --output "${VALIDATION_LOG_DIR}/gossip_storm_report.json" \
        2>&1) || exit_code=$?

    # Stop Ranvier and wait for cleanup
    kill -TERM "$ranvier_pid" 2>/dev/null || true
    sleep 2  # Allow graceful shutdown

    local end_time
    end_time=$(date +%s)
    local duration=$((end_time - start_time))

    if [[ $exit_code -eq 0 ]]; then
        TEST_RESULTS[$test_name]="PASS"
        log_success "SMP Gossip Storm Test: PASSED (${duration}s)"
    else
        TEST_RESULTS[$test_name]="FAIL"
        log_error "SMP Gossip Storm Test: FAILED (${duration}s)"
    fi

    TEST_DETAILS[$test_name]="$output"
    return $exit_code
}

run_atomic_test() {
    log_section "Test 4: Atomic-Free Execution Audit"

    local test_name="atomic_free_execution"

    log_info "Binary: $RANVIER_BINARY"
    log_info "Threshold: $ATOMIC_INSTRUCTION_THRESHOLD atomic instructions"

    # Check objdump is available
    if ! command -v objdump &>/dev/null; then
        log_warn "objdump not available, skipping atomic audit"
        TEST_RESULTS[$test_name]="SKIP"
        TEST_DETAILS[$test_name]='{"reason": "objdump not installed"}'
        return 0
    fi

    # Build command with optional verbose flag
    local cmd=("${SCRIPT_DIR}/atomic_audit.sh"
        --binary "$RANVIER_BINARY"
        --threshold "$ATOMIC_INSTRUCTION_THRESHOLD")
    [[ -n "${VERBOSE:-}" ]] && cmd+=(--verbose)

    run_test_wrapper "$test_name" "Atomic-Free Execution Test" "${cmd[@]}"
}

# -----------------------------------------------------------------------------
# Report Generation
# -----------------------------------------------------------------------------
generate_final_report() {
    log_section "Generating Final Report"

    mkdir -p "$(dirname "$REPORT_FILE")"

    local total=0
    local passed=0
    local failed=0
    local skipped=0

    for test in "${!TEST_RESULTS[@]}"; do
        total=$((total + 1))
        case "${TEST_RESULTS[$test]}" in
            PASS) passed=$((passed + 1)) ;;
            FAIL) failed=$((failed + 1)) ;;
            SKIP) skipped=$((skipped + 1)) ;;
        esac
    done

    local overall_status="PASS"
    if [[ $failed -gt 0 ]]; then
        overall_status="FAIL"
    fi

    # Build JSON report
    cat > "$REPORT_FILE" <<EOF
{
  "suite_name": "$SUITE_NAME",
  "version": "$VERSION",
  "timestamp": "$(date -Iseconds)",
  "mode": "$([ -n "$QUICK_MODE" ] && echo 'quick' || ([ -n "$CI_MODE" ] && echo 'ci' || echo 'standard'))",
  "overall_status": "$overall_status",
  "summary": {
    "total": $total,
    "passed": $passed,
    "failed": $failed,
    "skipped": $skipped
  },
  "configuration": {
    "binary": "$RANVIER_BINARY",
    "config": "$RANVIER_CONFIG",
    "api_port": $RANVIER_API_PORT,
    "metrics_port": $RANVIER_METRICS_PORT,
    "gossip_port": $RANVIER_GOSSIP_PORT
  },
  "thresholds": {
    "stall_threshold": $STALL_THRESHOLD,
    "latency_degradation_percent": $P99_LATENCY_DEGRADATION_THRESHOLD,
    "smp_queue_overflow": $SMP_QUEUE_OVERFLOW_THRESHOLD,
    "atomic_instructions": $ATOMIC_INSTRUCTION_THRESHOLD
  },
  "tests": {
EOF

    # Add individual test results
    local first=true
    for test in "${!TEST_RESULTS[@]}"; do
        if [[ "$first" != "true" ]]; then
            echo "," >> "$REPORT_FILE"
        fi
        first=false

        # Escape the details for JSON
        local details="${TEST_DETAILS[$test]:-{}}"
        # If details look like JSON, use as-is, otherwise wrap as string
        if [[ "$details" == "{"* ]]; then
            cat >> "$REPORT_FILE" <<EOF
    "$test": {
      "status": "${TEST_RESULTS[$test]}",
      "details": $details
    }
EOF
        else
            # Escape for JSON string
            details=$(echo "$details" | head -20 | sed 's/\\/\\\\/g; s/"/\\"/g; s/$/\\n/' | tr -d '\n')
            cat >> "$REPORT_FILE" <<EOF
    "$test": {
      "status": "${TEST_RESULTS[$test]}",
      "output": "$details"
    }
EOF
        fi
    done

    cat >> "$REPORT_FILE" <<EOF
  },
  "log_directory": "$VALIDATION_LOG_DIR"
}
EOF

    log_success "Report written to: $REPORT_FILE"
}

print_summary() {
    echo ""
    echo "=============================================================================="
    echo "                    VALIDATION SUITE SUMMARY"
    echo "=============================================================================="
    echo ""

    local total=0
    local passed=0
    local failed=0

    printf "%-40s %s\n" "TEST" "STATUS"
    printf "%-40s %s\n" "----" "------"

    for test in "${!TEST_RESULTS[@]}"; do
        local status="${TEST_RESULTS[$test]}"
        local color=""

        case "$status" in
            PASS)
                color="$GREEN"
                passed=$((passed + 1))
                ;;
            FAIL)
                color="$RED"
                failed=$((failed + 1))
                ;;
            SKIP)
                color="$YELLOW"
                ;;
        esac
        total=$((total + 1))

        printf "%-40s ${color}%s${NC}\n" "$test" "$status"
    done

    echo ""
    echo "------------------------------------------------------------------------------"
    printf "Total: %d | Passed: ${GREEN}%d${NC} | Failed: ${RED}%d${NC}\n" "$total" "$passed" "$failed"
    echo "------------------------------------------------------------------------------"

    if [[ $failed -eq 0 ]]; then
        echo ""
        echo -e "${GREEN}${BOLD}   ALL TESTS PASSED - Production Readiness v1.0 VERIFIED${NC}"
        echo ""
    else
        echo ""
        echo -e "${RED}${BOLD}   VALIDATION FAILED - $failed test(s) did not pass${NC}"
        echo ""
    fi

    echo "Report: $REPORT_FILE"
    echo "Logs:   $VALIDATION_LOG_DIR"
    echo "=============================================================================="
}

# =============================================================================
# Main
# =============================================================================
main() {
    parse_args "$@"

    # Print banner (unless CI mode)
    if [[ -z "$CI_MODE" ]]; then
        print_banner
    fi

    log_section "Ranvier Core Validation Suite v${VERSION}"

    log_info "Mode: $([ -n "$QUICK_MODE" ] && echo 'Quick' || ([ -n "$CI_MODE" ] && echo 'CI' || echo 'Standard'))"
    log_info "Binary: $RANVIER_BINARY"
    log_info "Config: $RANVIER_CONFIG"

    # Verify prerequisites
    if ! verify_prerequisites; then
        log_error "Prerequisites check failed"
        exit 2
    fi

    # Create output directories
    mkdir -p "$VALIDATION_REPORTS_DIR" "$VALIDATION_LOG_DIR"

    # Run tests
    local exit_code=0

    if [[ "$RUN_STALL_TEST" == "true" ]]; then
        run_stall_test || exit_code=1
    else
        log_info "Skipping reactor stall test"
    fi

    if [[ "$RUN_DISK_TEST" == "true" ]]; then
        run_disk_test || exit_code=1
    else
        log_info "Skipping disk I/O test"
    fi

    if [[ "$RUN_GOSSIP_TEST" == "true" ]]; then
        run_gossip_test || exit_code=1
    else
        log_info "Skipping gossip storm test"
    fi

    if [[ "$RUN_ATOMIC_TEST" == "true" ]]; then
        run_atomic_test || exit_code=1
    else
        log_info "Skipping atomic audit"
    fi

    # Generate report
    generate_final_report

    # Print summary (unless CI mode with quiet)
    print_summary

    # Return appropriate exit code
    if [[ $exit_code -ne 0 ]]; then
        exit 1
    fi

    exit 0
}

# Run if executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
