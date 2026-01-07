#!/usr/bin/env bash
# =============================================================================
# Unit tests for validation/lib/common.sh
# =============================================================================
# Tests the common library functions used by the validation suite.
# Run with: bash tests/unit/test_validation_common.sh
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Source the common library
source "${PROJECT_ROOT}/validation/lib/common.sh"

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test output
test_output() {
    local status="$1"
    local test_name="$2"
    local message="${3:-}"

    TESTS_RUN=$((TESTS_RUN + 1))
    if [[ "$status" == "PASS" ]]; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
        echo "[PASS] $test_name"
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        echo "[FAIL] $test_name"
        if [[ -n "$message" ]]; then
            echo "       $message"
        fi
    fi
}

# =============================================================================
# Tests for calculate_percentage_change
# =============================================================================
test_percentage_change_positive() {
    local result
    result=$(calculate_percentage_change 100 110)
    if [[ "$result" == "10.0" ]] || [[ "$result" == "10" ]]; then
        test_output "PASS" "calculate_percentage_change: 100 -> 110 = 10%"
    else
        test_output "FAIL" "calculate_percentage_change: 100 -> 110" "Expected 10, got $result"
    fi
}

test_percentage_change_negative() {
    local result
    result=$(calculate_percentage_change 100 90)
    if [[ "$result" == "-10.0" ]] || [[ "$result" == "-10" ]]; then
        test_output "PASS" "calculate_percentage_change: 100 -> 90 = -10%"
    else
        test_output "FAIL" "calculate_percentage_change: 100 -> 90" "Expected -10, got $result"
    fi
}

test_percentage_change_zero_baseline() {
    local result
    result=$(calculate_percentage_change 0 100)
    if [[ "$result" == "0" ]]; then
        test_output "PASS" "calculate_percentage_change: handles zero baseline"
    else
        test_output "FAIL" "calculate_percentage_change: zero baseline" "Expected 0, got $result"
    fi
}

test_percentage_change_no_change() {
    local result
    result=$(calculate_percentage_change 100 100)
    if [[ "$result" == "0.0" ]] || [[ "$result" == "0" ]]; then
        test_output "PASS" "calculate_percentage_change: no change = 0%"
    else
        test_output "FAIL" "calculate_percentage_change: no change" "Expected 0, got $result"
    fi
}

# =============================================================================
# Tests for get_cpu_count
# =============================================================================
test_get_cpu_count() {
    local result
    result=$(get_cpu_count)
    if [[ "$result" =~ ^[0-9]+$ ]] && [[ "$result" -gt 0 ]]; then
        test_output "PASS" "get_cpu_count: returns positive integer ($result)"
    else
        test_output "FAIL" "get_cpu_count" "Expected positive integer, got $result"
    fi
}

# =============================================================================
# Tests for check_dependencies
# =============================================================================
test_check_dependencies_found() {
    if check_dependencies bash; then
        test_output "PASS" "check_dependencies: finds bash"
    else
        test_output "FAIL" "check_dependencies: finds bash" "bash should be found"
    fi
}

test_check_dependencies_not_found() {
    if ! check_dependencies nonexistent_command_xyz123; then
        test_output "PASS" "check_dependencies: handles missing command"
    else
        test_output "FAIL" "check_dependencies: handles missing command" "Should have failed"
    fi
}

# =============================================================================
# Tests for color variables
# =============================================================================
test_color_variables_exported() {
    local colors_defined=true

    for var in RED GREEN YELLOW BLUE CYAN BOLD NC; do
        if [[ -z "${!var+x}" ]]; then
            colors_defined=false
            break
        fi
    done

    if $colors_defined; then
        test_output "PASS" "Color variables: all defined"
    else
        test_output "FAIL" "Color variables: all defined" "Some color vars missing"
    fi
}

# =============================================================================
# Tests for logging functions
# =============================================================================
test_log_info_output() {
    local output
    output=$(log_info "test message" 2>&1)
    if [[ "$output" == *"INFO"* ]] && [[ "$output" == *"test message"* ]]; then
        test_output "PASS" "log_info: outputs INFO and message"
    else
        test_output "FAIL" "log_info: outputs INFO and message" "Output: $output"
    fi
}

test_log_error_output() {
    local output
    output=$(log_error "error message" 2>&1)
    if [[ "$output" == *"FAIL"* ]] && [[ "$output" == *"error message"* ]]; then
        test_output "PASS" "log_error: outputs FAIL and message"
    else
        test_output "FAIL" "log_error: outputs FAIL and message" "Output: $output"
    fi
}

# =============================================================================
# Tests for process management
# =============================================================================
test_register_cleanup() {
    # Reset the array
    PIDS_TO_CLEANUP=()

    register_cleanup 12345
    register_cleanup 67890

    if [[ ${#PIDS_TO_CLEANUP[@]} -eq 2 ]]; then
        test_output "PASS" "register_cleanup: adds PIDs to array"
    else
        test_output "FAIL" "register_cleanup: adds PIDs to array" "Expected 2, got ${#PIDS_TO_CLEANUP[@]}"
    fi

    # Cleanup
    PIDS_TO_CLEANUP=()
}

# =============================================================================
# Tests for environment variable defaults
# =============================================================================
test_default_ports() {
    if [[ "$RANVIER_API_PORT" == "8080" ]] && \
       [[ "$RANVIER_METRICS_PORT" == "9180" ]] && \
       [[ "$RANVIER_GOSSIP_PORT" == "9190" ]]; then
        test_output "PASS" "Default ports: correct values"
    else
        test_output "FAIL" "Default ports: correct values" \
            "API=$RANVIER_API_PORT, Metrics=$RANVIER_METRICS_PORT, Gossip=$RANVIER_GOSSIP_PORT"
    fi
}

test_default_thresholds() {
    if [[ "$STALL_THRESHOLD" == "0" ]] && \
       [[ "$P99_LATENCY_DEGRADATION_THRESHOLD" == "10" ]] && \
       [[ "$ATOMIC_INSTRUCTION_THRESHOLD" == "0" ]]; then
        test_output "PASS" "Default thresholds: correct values"
    else
        test_output "FAIL" "Default thresholds: correct values"
    fi
}

# =============================================================================
# Main test runner
# =============================================================================
main() {
    echo "========================================"
    echo "Validation Common Library Unit Tests"
    echo "========================================"
    echo ""

    # Run all tests
    test_percentage_change_positive
    test_percentage_change_negative
    test_percentage_change_zero_baseline
    test_percentage_change_no_change
    test_get_cpu_count
    test_check_dependencies_found
    test_check_dependencies_not_found
    test_color_variables_exported
    test_log_info_output
    test_log_error_output
    test_register_cleanup
    test_default_ports
    test_default_thresholds

    echo ""
    echo "========================================"
    echo "Results: $TESTS_PASSED/$TESTS_RUN passed, $TESTS_FAILED failed"
    echo "========================================"

    if [[ $TESTS_FAILED -gt 0 ]]; then
        exit 1
    fi
    exit 0
}

# Disable cleanup trap for tests
trap - EXIT INT TERM

main
