#!/bin/bash
# Benchmark regression testing helper script for Ranvier Core
# Parses Locust output and compares against baseline metrics
#
# Usage:
#   ./run-benchmark.sh --results-dir <dir> --baseline <file> [options]
#   ./run-benchmark.sh --results-dir <dir> --generate-baseline <output-file>
#
# Options:
#   --results-dir <dir>        Directory containing Locust CSV output
#   --baseline <file>          Path to baseline JSON file
#   --p99-threshold <pct>      P99 latency regression threshold (default: 10)
#   --throughput-threshold <pct>  Throughput regression threshold (default: 5)
#   --generate-baseline <file> Generate new baseline from results
#
# Exit codes:
#   0 - All metrics within thresholds (PASS)
#   1 - Regression detected or error (FAIL)

set -euo pipefail

# Default thresholds
P99_THRESHOLD=10
THROUGHPUT_THRESHOLD=5
RESULTS_DIR=""
BASELINE_FILE=""
GENERATE_BASELINE=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 --results-dir <dir> --baseline <file> [options]"
    echo ""
    echo "Options:"
    echo "  --results-dir <dir>           Directory containing Locust CSV output"
    echo "  --baseline <file>             Path to baseline JSON file"
    echo "  --p99-threshold <pct>         P99 latency regression threshold (default: 10)"
    echo "  --throughput-threshold <pct>  Throughput regression threshold (default: 5)"
    echo "  --generate-baseline <file>    Generate new baseline from results"
    exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --results-dir)
            RESULTS_DIR="$2"
            shift 2
            ;;
        --baseline)
            BASELINE_FILE="$2"
            shift 2
            ;;
        --p99-threshold)
            P99_THRESHOLD="$2"
            shift 2
            ;;
        --throughput-threshold)
            THROUGHPUT_THRESHOLD="$2"
            shift 2
            ;;
        --generate-baseline)
            GENERATE_BASELINE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Validate required arguments
if [[ -z "$RESULTS_DIR" ]]; then
    echo "Error: --results-dir is required"
    usage
fi

# Check for required files
STATS_FILE="$RESULTS_DIR/benchmark_stats.csv"
if [[ ! -f "$STATS_FILE" ]]; then
    echo "Error: Stats file not found: $STATS_FILE"
    exit 1
fi

# Parse Locust CSV stats
# Format: Type,Name,Request Count,Failure Count,Median Response Time,Average Response Time,
#         Min Response Time,Max Response Time,Average Content Size,Requests/s,Failures/s,
#         50%,66%,75%,80%,90%,95%,99%,99.9%,99.99%,100%
parse_locust_stats() {
    local stats_file="$1"

    # Get the aggregated row (Type="Aggregated")
    local agg_line
    agg_line=$(grep "^Aggregated" "$stats_file" 2>/dev/null || grep "^\"Aggregated\"" "$stats_file" 2>/dev/null || echo "")

    if [[ -z "$agg_line" ]]; then
        # Try to get the last non-header line as aggregate
        agg_line=$(tail -n 1 "$stats_file")
    fi

    # Parse CSV fields (handle both quoted and unquoted formats)
    # Using awk for robust CSV parsing
    echo "$agg_line" | awk -F',' '
    {
        # Remove quotes from fields
        for (i=1; i<=NF; i++) {
            gsub(/^"/, "", $i)
            gsub(/"$/, "", $i)
        }

        # Locust CSV columns (0-indexed in awk is 1-indexed):
        # 1:Type, 2:Name, 3:Request Count, 4:Failure Count, 5:Median, 6:Average,
        # 7:Min, 8:Max, 9:Avg Content Size, 10:Requests/s, 11:Failures/s,
        # 12:50%, 13:66%, 14:75%, 15:80%, 16:90%, 17:95%, 18:99%, 19:99.9%, 20:99.99%, 21:100%

        request_count = $3
        failure_count = $4
        avg_latency = $6
        min_latency = $7
        max_latency = $8
        rps = $10
        p50 = $12
        p90 = $16
        p99 = $18

        # Calculate failure rate
        if (request_count > 0) {
            failure_rate = (failure_count / request_count) * 100
        } else {
            failure_rate = 0
        }

        printf "REQUEST_COUNT=%s\n", request_count
        printf "FAILURE_COUNT=%s\n", failure_count
        printf "AVG_LATENCY=%.2f\n", avg_latency
        printf "MIN_LATENCY=%.2f\n", min_latency
        printf "MAX_LATENCY=%.2f\n", max_latency
        printf "P50_LATENCY=%.2f\n", p50
        printf "P90_LATENCY=%.2f\n", p90
        printf "P99_LATENCY=%.2f\n", p99
        printf "THROUGHPUT=%.2f\n", rps
        printf "FAILURE_RATE=%.2f\n", failure_rate
    }'
}

# Generate baseline JSON from current results
generate_baseline() {
    local output_file="$1"

    echo "Parsing Locust stats from: $STATS_FILE"

    # Parse current results
    eval "$(parse_locust_stats "$STATS_FILE")"

    # Calculate success rate
    local success_rate
    success_rate=$(echo "100 - $FAILURE_RATE" | bc -l 2>/dev/null || echo "99.9")

    # Generate JSON
    cat > "$output_file" << EOF
{
  "version": "1.0.0",
  "description": "Benchmark baseline for Ranvier Core CI regression testing",
  "updated_at": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "updated_by": "run-benchmark.sh",
  "test_config": {
    "users": ${BENCHMARK_USERS:-100},
    "duration_seconds": 60,
    "spawn_rate": ${BENCHMARK_SPAWN_RATE:-10},
    "environment": "docker-compose.test.yml (mock backends)"
  },
  "metrics": {
    "p50_latency_ms": ${P50_LATENCY:-15.0},
    "p90_latency_ms": ${P90_LATENCY:-35.0},
    "p99_latency_ms": ${P99_LATENCY:-75.0},
    "avg_latency_ms": ${AVG_LATENCY:-20.0},
    "min_latency_ms": ${MIN_LATENCY:-5.0},
    "max_latency_ms": ${MAX_LATENCY:-150.0},
    "throughput_rps": ${THROUGHPUT:-450.0},
    "total_requests": ${REQUEST_COUNT:-27000},
    "failure_rate_percent": ${FAILURE_RATE:-0.1},
    "success_rate_percent": ${success_rate}
  },
  "thresholds": {
    "p99_regression_percent": ${P99_THRESHOLD},
    "throughput_regression_percent": ${THROUGHPUT_THRESHOLD},
    "max_failure_rate_percent": 1.0
  },
  "notes": [
    "Baseline established with mock vLLM backends (docker-compose.test.yml)",
    "Mock backends have minimal latency compared to real vLLM inference",
    "P99 latency target: <100ms for mock backend tests",
    "Update baseline via: gh workflow run benchmark.yml -f update_baseline=true"
  ]
}
EOF

    echo "Generated baseline: $output_file"
    cat "$output_file"
}

# Compare current results against baseline
compare_results() {
    local baseline_file="$1"

    echo "=================================================="
    echo "Benchmark Regression Test"
    echo "=================================================="
    echo ""

    # Check baseline exists
    if [[ ! -f "$baseline_file" ]]; then
        echo "Warning: Baseline file not found: $baseline_file"
        echo "Running without baseline comparison (first run)"

        # Generate comparison.md for summary
        cat > "$RESULTS_DIR/comparison.md" << 'EOF'
### First Run - No Baseline

This appears to be the first benchmark run. No baseline comparison available.

To establish a baseline, run:
```
gh workflow run benchmark.yml -f update_baseline=true
```
EOF
        return 0
    fi

    # Parse current results
    echo "Parsing current results..."
    eval "$(parse_locust_stats "$STATS_FILE")"

    echo "Current Results:"
    echo "  P99 Latency:  ${P99_LATENCY}ms"
    echo "  P90 Latency:  ${P90_LATENCY}ms"
    echo "  P50 Latency:  ${P50_LATENCY}ms"
    echo "  Avg Latency:  ${AVG_LATENCY}ms"
    echo "  Throughput:   ${THROUGHPUT} req/s"
    echo "  Requests:     ${REQUEST_COUNT}"
    echo "  Failure Rate: ${FAILURE_RATE}%"
    echo ""

    # Parse baseline
    echo "Loading baseline from: $baseline_file"

    # Use jq if available, otherwise use grep/sed
    if command -v jq &> /dev/null; then
        BASELINE_P99=$(jq -r '.metrics.p99_latency_ms' "$baseline_file")
        BASELINE_P90=$(jq -r '.metrics.p90_latency_ms' "$baseline_file")
        BASELINE_P50=$(jq -r '.metrics.p50_latency_ms' "$baseline_file")
        BASELINE_THROUGHPUT=$(jq -r '.metrics.throughput_rps' "$baseline_file")
        BASELINE_FAILURE_RATE=$(jq -r '.metrics.failure_rate_percent' "$baseline_file")
        MAX_FAILURE_RATE=$(jq -r '.thresholds.max_failure_rate_percent // 1.0' "$baseline_file")
    else
        # Fallback to grep/sed parsing
        BASELINE_P99=$(grep -o '"p99_latency_ms":[^,}]*' "$baseline_file" | sed 's/.*://')
        BASELINE_P90=$(grep -o '"p90_latency_ms":[^,}]*' "$baseline_file" | sed 's/.*://')
        BASELINE_P50=$(grep -o '"p50_latency_ms":[^,}]*' "$baseline_file" | sed 's/.*://')
        BASELINE_THROUGHPUT=$(grep -o '"throughput_rps":[^,}]*' "$baseline_file" | sed 's/.*://')
        BASELINE_FAILURE_RATE=$(grep -o '"failure_rate_percent":[^,}]*' "$baseline_file" | sed 's/.*://')
        MAX_FAILURE_RATE=1.0
    fi

    echo "Baseline:"
    echo "  P99 Latency:  ${BASELINE_P99}ms"
    echo "  P90 Latency:  ${BASELINE_P90}ms"
    echo "  P50 Latency:  ${BASELINE_P50}ms"
    echo "  Throughput:   ${BASELINE_THROUGHPUT} req/s"
    echo "  Failure Rate: ${BASELINE_FAILURE_RATE}%"
    echo ""

    # Calculate deltas
    # P99 regression: positive delta is bad (latency increased)
    P99_DELTA=$(echo "scale=2; (($P99_LATENCY - $BASELINE_P99) / $BASELINE_P99) * 100" | bc -l)

    # Throughput regression: negative delta is bad (throughput decreased)
    THROUGHPUT_DELTA=$(echo "scale=2; (($THROUGHPUT - $BASELINE_THROUGHPUT) / $BASELINE_THROUGHPUT) * 100" | bc -l)

    # P90 and P50 deltas (informational)
    P90_DELTA=$(echo "scale=2; (($P90_LATENCY - $BASELINE_P90) / $BASELINE_P90) * 100" | bc -l)
    P50_DELTA=$(echo "scale=2; (($P50_LATENCY - $BASELINE_P50) / $BASELINE_P50) * 100" | bc -l)

    echo "=================================================="
    echo "Comparison Results"
    echo "=================================================="
    echo ""

    # Track if any regression detected
    REGRESSION_DETECTED=0

    # Check P99 latency regression
    P99_THRESHOLD_EXCEEDED=$(echo "$P99_DELTA > $P99_THRESHOLD" | bc -l)
    if [[ "$P99_THRESHOLD_EXCEEDED" -eq 1 ]]; then
        echo -e "${RED}FAIL${NC}: P99 latency regressed by ${P99_DELTA}% (threshold: ${P99_THRESHOLD}%)"
        echo "       ${BASELINE_P99}ms -> ${P99_LATENCY}ms"
        REGRESSION_DETECTED=1
    else
        echo -e "${GREEN}PASS${NC}: P99 latency delta: ${P99_DELTA}% (threshold: ${P99_THRESHOLD}%)"
        echo "       ${BASELINE_P99}ms -> ${P99_LATENCY}ms"
    fi

    # Check throughput regression (negative delta is regression)
    THROUGHPUT_REGRESSION=$(echo "$THROUGHPUT_DELTA < -$THROUGHPUT_THRESHOLD" | bc -l)
    if [[ "$THROUGHPUT_REGRESSION" -eq 1 ]]; then
        echo -e "${RED}FAIL${NC}: Throughput regressed by ${THROUGHPUT_DELTA}% (threshold: -${THROUGHPUT_THRESHOLD}%)"
        echo "       ${BASELINE_THROUGHPUT} req/s -> ${THROUGHPUT} req/s"
        REGRESSION_DETECTED=1
    else
        echo -e "${GREEN}PASS${NC}: Throughput delta: ${THROUGHPUT_DELTA}% (threshold: -${THROUGHPUT_THRESHOLD}%)"
        echo "       ${BASELINE_THROUGHPUT} req/s -> ${THROUGHPUT} req/s"
    fi

    # Check failure rate
    FAILURE_EXCEEDED=$(echo "$FAILURE_RATE > $MAX_FAILURE_RATE" | bc -l)
    if [[ "$FAILURE_EXCEEDED" -eq 1 ]]; then
        echo -e "${RED}FAIL${NC}: Failure rate too high: ${FAILURE_RATE}% (max: ${MAX_FAILURE_RATE}%)"
        REGRESSION_DETECTED=1
    else
        echo -e "${GREEN}PASS${NC}: Failure rate: ${FAILURE_RATE}% (max: ${MAX_FAILURE_RATE}%)"
    fi

    echo ""
    echo "Additional Metrics (informational):"
    echo "  P90 latency delta: ${P90_DELTA}%"
    echo "  P50 latency delta: ${P50_DELTA}%"
    echo ""

    # Generate comparison.md for GitHub summary
    if [[ "$REGRESSION_DETECTED" -eq 1 ]]; then
        STATUS_ICON=":x:"
        STATUS_TEXT="FAILED - Regression Detected"
    else
        STATUS_ICON=":white_check_mark:"
        STATUS_TEXT="PASSED"
    fi

    cat > "$RESULTS_DIR/comparison.md" << EOF
### Benchmark Comparison: ${STATUS_TEXT}

| Metric | Baseline | Current | Delta | Status |
|--------|----------|---------|-------|--------|
| P99 Latency | ${BASELINE_P99}ms | ${P99_LATENCY}ms | ${P99_DELTA}% | $(if [[ "$P99_THRESHOLD_EXCEEDED" -eq 1 ]]; then echo ":x:"; else echo ":white_check_mark:"; fi) |
| P90 Latency | ${BASELINE_P90}ms | ${P90_LATENCY}ms | ${P90_DELTA}% | - |
| P50 Latency | ${BASELINE_P50}ms | ${P50_LATENCY}ms | ${P50_DELTA}% | - |
| Throughput | ${BASELINE_THROUGHPUT} req/s | ${THROUGHPUT} req/s | ${THROUGHPUT_DELTA}% | $(if [[ "$THROUGHPUT_REGRESSION" -eq 1 ]]; then echo ":x:"; else echo ":white_check_mark:"; fi) |
| Failure Rate | ${BASELINE_FAILURE_RATE}% | ${FAILURE_RATE}% | - | $(if [[ "$FAILURE_EXCEEDED" -eq 1 ]]; then echo ":x:"; else echo ":white_check_mark:"; fi) |
| Total Requests | - | ${REQUEST_COUNT} | - | - |

**Thresholds:**
- P99 latency regression: ≤${P99_THRESHOLD}%
- Throughput regression: ≤${THROUGHPUT_THRESHOLD}%
- Max failure rate: ≤${MAX_FAILURE_RATE}%
EOF

    echo "Comparison report written to: $RESULTS_DIR/comparison.md"

    # Return exit code based on regression
    if [[ "$REGRESSION_DETECTED" -eq 1 ]]; then
        echo ""
        echo -e "${RED}=================================================="
        echo "BENCHMARK FAILED - REGRESSION DETECTED"
        echo "==================================================${NC}"
        return 1
    else
        echo ""
        echo -e "${GREEN}=================================================="
        echo "BENCHMARK PASSED"
        echo "==================================================${NC}"
        return 0
    fi
}

# Main execution
main() {
    # Generate baseline mode
    if [[ -n "$GENERATE_BASELINE" ]]; then
        generate_baseline "$GENERATE_BASELINE"
        exit 0
    fi

    # Comparison mode (requires baseline)
    if [[ -z "$BASELINE_FILE" ]]; then
        echo "Error: --baseline is required for comparison mode"
        usage
    fi

    compare_results "$BASELINE_FILE"
}

main
