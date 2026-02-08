#!/bin/bash
# =============================================================================
# Ranvier Benchmark Runner - Multi-Run Orchestrator
# =============================================================================
#
# Runs a series of benchmark configurations sequentially via bench.sh.
# Tracks progress, captures results, and produces a summary report.
#
# Usage:
#   ./scripts/bench-runner.sh                          # Run default suite
#   ./scripts/bench-runner.sh --suite high             # High-priority runs only
#   ./scripts/bench-runner.sh --suite medium            # High + medium priority
#   ./scripts/bench-runner.sh --suite all               # All runs
#   ./scripts/bench-runner.sh --suite custom --file runs.txt  # Custom run file
#   ./scripts/bench-runner.sh --dry-run                # Preview what would run
#   ./scripts/bench-runner.sh --resume 3               # Resume from run #3
#
# Custom run file format (one bench.sh invocation per line):
#   # Comments start with #
#   --compare --model meta-llama/Llama-3.1-8B-Instruct --warmup --duration 10m --users 20
#   --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 30 --max-model-len 8192
#
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_SH="${SCRIPT_DIR}/bench.sh"
RUNNER_OUTPUT_DIR="benchmark-reports"
PAUSE_BETWEEN_RUNS=60  # seconds between runs for GPU cooldown

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Capture original command for logging
ORIGINAL_CMD="$0 $*"

# State
RESUME_FROM=0
DRY_RUN=false
SUITE="high"
CUSTOM_FILE=""
STOP_ON_FAILURE=false
SKIP_RUNS_RAW=""   # comma-separated list from --skip

# -----------------------------------------------------------------------------
# Logging
# -----------------------------------------------------------------------------

log_header()  { echo -e "\n${BOLD}${CYAN}$1${NC}"; echo -e "${CYAN}$(printf '═%.0s' {1..60})${NC}"; }
log_info()    { echo -e "${BLUE}▸${NC} $1"; }
log_ok()      { echo -e "${GREEN}✓${NC} $1"; }
log_warn()    { echo -e "${YELLOW}⚠${NC} $1"; }
log_error()   { echo -e "${RED}✗${NC} $1"; }
log_run()     { echo -e "${BOLD}${CYAN}[$1/$2]${NC} $3"; }

# Format seconds as Xh Ym Zs
fmt_duration() {
    local secs=$1
    local h=$((secs / 3600))
    local m=$(( (secs % 3600) / 60 ))
    local s=$((secs % 60))
    if [[ $h -gt 0 ]]; then
        echo "${h}h ${m}m ${s}s"
    elif [[ $m -gt 0 ]]; then
        echo "${m}m ${s}s"
    else
        echo "${s}s"
    fi
}

# Parse a duration string (e.g., "10m", "1h", "30s") to seconds
parse_duration() {
    local duration="$1"
    local num="${duration%[smhSMH]}"
    local unit="${duration##*[0-9]}"
    case "$unit" in
        s|S) echo "$num" ;;
        m|M) echo $((num * 60)) ;;
        h|H) echo $((num * 3600)) ;;
        *)   echo "$num" ;;
    esac
}

# Estimate total benchmark seconds for a set of bench.sh args.
# Accounts for --duration, --compare (2x), --warmup (+70s), and vLLM startup (~300s).
estimate_run_seconds() {
    local args="$1"
    local duration="5m"  # bench.sh default
    local has_compare=false
    local has_warmup=false

    # Parse relevant flags from the arg string
    set -- $args
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --duration)  duration="$2"; shift 2 ;;
            --compare)   has_compare=true; shift ;;
            --warmup)    has_warmup=true; shift ;;
            *)           shift ;;
        esac
    done

    local secs
    secs=$(parse_duration "$duration")

    if [[ "$has_compare" == true ]]; then
        secs=$((secs * 2 + 30))  # two benchmarks + 30s pause between
    fi
    if [[ "$has_warmup" == true ]]; then
        secs=$((secs + 60 + 10))  # 1m warmup + 10s pause
    fi

    # Add estimated overhead: vLLM startup (~5min) + Ranvier startup (~30s)
    secs=$((secs + 330))

    echo "$secs"
}

# Check if a run number should be skipped (via --resume or --skip).
# Returns 0 (true) if the run should be skipped, 1 (false) if it should execute.
should_skip() {
    local run_num=$1
    # --resume: skip everything before the resume point
    if [[ $RESUME_FROM -gt 0 && $run_num -lt $RESUME_FROM ]]; then
        return 0
    fi
    # --skip: skip specific run numbers
    if [[ -n "$SKIP_RUNS_RAW" ]]; then
        local IFS=','
        for skip_n in $SKIP_RUNS_RAW; do
            if [[ "$skip_n" -eq "$run_num" ]] 2>/dev/null; then
                return 0
            fi
        done
    fi
    return 1
}

# Extract and display key metrics from a benchmark.log file.
# Greps for the BENCHMARK_STATS_JSON line emitted by locustfile_real.py.
extract_metrics() {
    local log_file="$1"
    local label="${2:-}"

    if [[ ! -f "$log_file" ]]; then
        return
    fi

    local json_line
    json_line=$(grep "BENCHMARK_STATS_JSON:" "$log_file" 2>/dev/null | tail -1 | sed 's/.*BENCHMARK_STATS_JSON://')
    if [[ -z "$json_line" ]]; then
        # Fall back to grepping individual fields
        local hit_rate cache_hits ttft_improv
        hit_rate=$(grep "Cache Hit Rate:" "$log_file" 2>/dev/null | tail -1 | grep -oP '[0-9.]+(?=%)' || echo "")
        cache_hits=$(grep "Cache Hits:" "$log_file" 2>/dev/null | tail -1 | grep -oP '\d+' || echo "")
        ttft_improv=$(grep "TTFT Improvement:" "$log_file" 2>/dev/null | tail -1 | grep -oP '\-?[0-9.]+(?=%)' || echo "")
        if [[ -n "$hit_rate" || -n "$ttft_improv" ]]; then
            echo -ne "    ${label:+${BOLD}${label}:${NC} }"
            [[ -n "$hit_rate" ]] && echo -ne "Cache: ${hit_rate}%"
            [[ -n "$hit_rate" && -n "$cache_hits" ]] && echo -ne " (${cache_hits} hits)"
            [[ -n "$ttft_improv" ]] && echo -ne " | TTFT Improv: ${ttft_improv}%"
            echo ""
        fi
        return
    fi

    # Parse JSON with lightweight field extraction (no jq dependency)
    local hit_rate cache_hits ttft_improv total_reqs failed_reqs tokens_sec
    hit_rate=$(echo "$json_line" | grep -oP '"cache_hit_rate_pct":\s*[0-9.]+' | grep -oP '[0-9.]+$' || echo "")
    cache_hits=$(echo "$json_line" | grep -oP '"cache_hits":\s*[0-9]+' | grep -oP '[0-9]+$' || echo "")
    ttft_improv=$(echo "$json_line" | grep -oP '"ttft_improvement_pct":\s*-?[0-9.]+' | grep -oP '\-?[0-9.]+$' || echo "")
    total_reqs=$(echo "$json_line" | grep -oP '"total_requests":\s*[0-9]+' | grep -oP '[0-9]+$' || echo "")
    failed_reqs=$(echo "$json_line" | grep -oP '"failed_requests":\s*[0-9]+' | grep -oP '[0-9]+$' || echo "")
    tokens_sec=$(echo "$json_line" | grep -oP '"tokens_per_second":\s*[0-9.]+' | grep -oP '[0-9.]+$' || echo "")

    echo -ne "    ${label:+${BOLD}${label}:${NC} }"
    [[ -n "$hit_rate" ]] && echo -ne "Cache: ${hit_rate}%"
    [[ -n "$cache_hits" ]] && echo -ne " (${cache_hits} hits)"
    [[ -n "$ttft_improv" ]] && echo -ne " | TTFT Improv: ${ttft_improv}%"
    [[ -n "$tokens_sec" ]] && echo -ne " | Tok/s: ${tokens_sec}"
    [[ -n "$total_reqs" ]] && echo -ne " | Reqs: ${total_reqs}"
    if [[ -n "$failed_reqs" && "$failed_reqs" != "0" ]]; then
        echo -ne " | ${RED}Errors: ${failed_reqs}${NC}"
    fi
    echo ""
}

# Extract per-bucket TTFT improvements from a benchmark.log (xlarge is the headline metric)
extract_bucket_improvement() {
    local log_file="$1"
    local label="${2:-}"

    if [[ ! -f "$log_file" ]]; then
        return
    fi

    # Look for xlarge bucket line in the per-bucket table
    local xlarge_line
    xlarge_line=$(grep -P "^\s*xlarge\s+" "$log_file" 2>/dev/null | tail -1 || echo "")
    if [[ -n "$xlarge_line" ]]; then
        local xlarge_improv
        xlarge_improv=$(echo "$xlarge_line" | grep -oP '\-?[0-9.]+(?=%)' | tail -1 || echo "")
        if [[ -n "$xlarge_improv" ]]; then
            echo -e "    ${label:+${BOLD}${label}:${NC} }XLarge TTFT Improvement: ${xlarge_improv}%"
        fi
    fi
}

# -----------------------------------------------------------------------------
# Help
# -----------------------------------------------------------------------------

print_help() {
    cat << 'EOF'
Ranvier Benchmark Runner - Multi-Run Orchestrator

Runs a series of benchmark configurations sequentially via bench.sh,
tracking progress and producing a summary report.

USAGE:
    ./scripts/bench-runner.sh [OPTIONS]

OPTIONS:
    --suite LEVEL       Which benchmark suite to run (default: high)
                          high   - 3 high-priority runs (~45 min)
                          medium - high + 3 medium-priority runs (~2.5 hours)
                          all    - all 9 runs (~4+ hours)
                          custom - use a custom run file (requires --file)
    --file FILE         Path to custom run file (one bench.sh arg set per line)
    --dry-run           Preview runs without executing
    --resume N          Resume from run number N (1-indexed, skips runs before N)
    --skip LIST         Skip specific runs by number (comma-separated, e.g., --skip 9,10)
    --pause SECONDS     Pause between runs for GPU cooldown (default: 60)
    --stop-on-failure   Stop the suite if any run fails (default: continue)
    --output-dir DIR    Output directory (default: benchmark-reports)
    -h, --help          Show this help

BUILT-IN SUITES:
    high (3 runs, ~1.5h):
      1. 13B at 20 users (compare load-aware vs Jan baseline 38.9%)
      2. 8B at 20 users  (compare load-aware vs Jan baseline 43.7%)
      3. 13B at 10 users (compare load-aware vs Jan baseline 48.2%)

    medium (adds 4 runs, ~4h total):
      4. 13B 30-minute validated run at 30 users
      5. 8B 30-minute validated run at 30 users
      6. 13B prefix ratio 0.7
      7. 13B prefix ratio 0.5

    all (adds 4 more, ~6.5h total):
      8.  13B client tokenization comparison
      9.  8B high concurrency stress test (64 users)
      10. 70B model test
      11. 8B with 16K max prefix (tests larger-than-default prefixes)

ADDING NEW RUNS:
    Edit define_runs() in this script. Each run is one line:
      add_run <priority> "<label>" <bench.sh args...>
    Use --dry-run to verify numbering after changes.

CUSTOM RUN FILE FORMAT:
    # One set of bench.sh arguments per line
    # Lines starting with # are comments, blank lines are skipped
    --compare --model meta-llama/Llama-3.1-8B-Instruct --warmup --duration 10m --users 20
    --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 30 --max-model-len 8192

EXAMPLES:
    # Preview the default high-priority suite
    ./scripts/bench-runner.sh --dry-run

    # Run high-priority benchmarks
    export HF_TOKEN=hf_xxx
    ./scripts/bench-runner.sh --suite high

    # Run all benchmarks, stop if one fails
    ./scripts/bench-runner.sh --suite all --stop-on-failure

    # Resume from run #4 after a failure
    ./scripts/bench-runner.sh --suite all --resume 4

    # Skip specific runs (e.g., 70B model not supported yet)
    ./scripts/bench-runner.sh --suite all --skip 10

    # Skip multiple runs
    ./scripts/bench-runner.sh --suite all --skip 9,10

    # Use a custom list of runs
    ./scripts/bench-runner.sh --suite custom --file my-runs.txt

EOF
}

# -----------------------------------------------------------------------------
# Argument parsing
# -----------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case $1 in
        --suite)            SUITE="$2"; shift 2 ;;
        --file)             CUSTOM_FILE="$2"; shift 2 ;;
        --dry-run)          DRY_RUN=true; shift ;;
        --resume)           RESUME_FROM="$2"; shift 2 ;;
        --skip)             SKIP_RUNS_RAW="$2"; shift 2 ;;
        --pause)            PAUSE_BETWEEN_RUNS="$2"; shift 2 ;;
        --stop-on-failure)  STOP_ON_FAILURE=true; shift ;;
        --output-dir)       RUNNER_OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help)          print_help; exit 0 ;;
        *)                  log_error "Unknown option: $1"; echo "Run with --help for usage."; exit 1 ;;
    esac
done

# -----------------------------------------------------------------------------
# Define benchmark suites
# -----------------------------------------------------------------------------
# Each run is defined as:  add_run <priority> <label> <bench.sh args...>
#
# Priority levels (cumulative):
#   high   = runs 1-3       (included in --suite high, medium, all)
#   medium = runs 4-7       (included in --suite medium, all)
#   low    = runs 8-10      (included in --suite all only)
#
# To add a new benchmark, append an add_run line at the end of the
# appropriate priority section. Run numbers are assigned in order.
# Use --dry-run to verify numbering after changes.

add_run() {
    local priority="$1"
    local label="$2"
    shift 2
    local args="$*"

    case "$SUITE" in
        high)   [[ "$priority" != "high" ]] && return ;;
        medium) [[ "$priority" == "low" ]] && return ;;
        all)    ;;  # include everything
        *)      return ;;  # custom suite doesn't use add_run
    esac

    LABELS+=("$label")
    RUNS+=("$args")
}

define_runs() {
    RUNS=()
    LABELS=()

    # --- High priority: re-run Jan baselines with load-aware routing ----------
    add_run high "13B moderate load (20 users)" \
        --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
        --warmup --duration 10m --users 20 --max-model-len 8192

    add_run high "8B moderate load (20 users)" \
        --compare --model meta-llama/Llama-3.1-8B-Instruct \
        --warmup --duration 10m --users 20

    add_run high "13B low load (10 users)" \
        --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
        --warmup --duration 10m --users 10 --max-model-len 8192

    # --- Medium priority: long runs and prefix ratio sweep --------------------
    add_run medium "13B 30min validated (30 users)" \
        --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
        --warmup --duration 30m --users 30 --max-model-len 8192

    add_run medium "8B 30min validated (30 users)" \
        --compare --model meta-llama/Llama-3.1-8B-Instruct \
        --warmup --duration 30m --users 30

    add_run medium "13B prefix ratio 0.7 (20 users)" \
        --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
        --warmup --duration 10m --users 20 --prefix-ratio 0.7 --max-model-len 8192

    add_run medium "13B prefix ratio 0.5 (20 users)" \
        --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
        --warmup --duration 10m --users 20 --prefix-ratio 0.5 --max-model-len 8192

    # --- Lower priority: client tokenization, stress, large models ------------
    add_run low "13B client tokenization (30 users)" \
        --compare --client-tokenize --model meta-llama/CodeLlama-13b-Instruct-hf \
        --warmup --duration 10m --users 30 --max-model-len 8192

    add_run low "8B high concurrency stress (64 users)" \
        --warmup --duration 15m --users 64 --spawn-rate 4 \
        --model meta-llama/Llama-3.1-8B-Instruct

    add_run low "70B model test (16 users)" \
        --compare --model meta-llama/Llama-3.1-70B-Instruct \
        --warmup --duration 15m --users 16

    add_run low "8B 16K prefix test (20 users)" \
        --compare --model meta-llama/Llama-3.1-8B-Instruct \
        --warmup --duration 10m --users 20 --prefix-max-tokens 16000

    # --- Custom file ----------------------------------------------------------
    if [[ "$SUITE" == "custom" ]]; then
        if [[ -z "$CUSTOM_FILE" ]]; then
            log_error "--suite custom requires --file <path>"
            exit 1
        fi
        if [[ ! -f "$CUSTOM_FILE" ]]; then
            log_error "Custom run file not found: $CUSTOM_FILE"
            exit 1
        fi
        local line_num=0
        while IFS= read -r line || [[ -n "$line" ]]; do
            line_num=$((line_num + 1))
            # Skip comments and blank lines
            line="$(echo "$line" | sed 's/#.*//' | xargs)"
            [[ -z "$line" ]] && continue
            LABELS+=("custom run #${line_num}")
            RUNS+=("$line")
        done < "$CUSTOM_FILE"
    fi
}

define_runs

TOTAL_RUNS=${#RUNS[@]}

if [[ $TOTAL_RUNS -eq 0 ]]; then
    log_error "No runs defined for suite '$SUITE'"
    exit 1
fi

# -----------------------------------------------------------------------------
# Validate
# -----------------------------------------------------------------------------

if [[ ! -x "$BENCH_SH" ]]; then
    log_error "bench.sh not found or not executable at: $BENCH_SH"
    exit 1
fi

if [[ $RESUME_FROM -gt $TOTAL_RUNS ]]; then
    log_error "--resume $RESUME_FROM exceeds total runs ($TOTAL_RUNS)"
    exit 1
fi

# -----------------------------------------------------------------------------
# Dry run
# -----------------------------------------------------------------------------

if [[ "$DRY_RUN" == true ]]; then
    log_header "Dry Run - Suite: $SUITE ($TOTAL_RUNS runs)"
    echo ""
    TOTAL_EST=0
    DRY_ACTIVE=0
    for ((i=0; i<TOTAL_RUNS; i++)); do
        local_num=$((i + 1))
        skip=""
        if should_skip "$local_num"; then
            skip=" ${YELLOW}(skip)${NC}"
        fi
        EST_SECS=$(estimate_run_seconds "${RUNS[$i]}")
        echo -e "  ${BOLD}[$local_num/$TOTAL_RUNS]${NC} ${LABELS[$i]}  ${BLUE}~$(fmt_duration $EST_SECS)${NC}${skip}"
        echo -e "           ${CYAN}./scripts/bench.sh ${RUNS[$i]}${NC}"
        echo ""
        if ! should_skip "$local_num"; then
            TOTAL_EST=$((TOTAL_EST + EST_SECS + PAUSE_BETWEEN_RUNS))
            DRY_ACTIVE=$((DRY_ACTIVE + 1))
        fi
    done
    # Remove trailing pause (none after last run)
    ACTIVE_RUNS=$DRY_ACTIVE
    if [[ $ACTIVE_RUNS -gt 0 ]]; then
        TOTAL_EST=$((TOTAL_EST - PAUSE_BETWEEN_RUNS))
    fi
    echo -e "  ${BOLD}Estimated total time: $(fmt_duration $TOTAL_EST)${NC}"
    echo ""
    log_info "Pause between runs: ${PAUSE_BETWEEN_RUNS}s"
    if [[ "$STOP_ON_FAILURE" == true ]]; then
        log_info "Will stop on first failure"
    else
        log_info "Will continue past failures"
    fi
    exit 0
fi

# -----------------------------------------------------------------------------
# Pre-flight
# -----------------------------------------------------------------------------

log_header "Ranvier Benchmark Runner"
log_info "Suite: $SUITE ($TOTAL_RUNS runs)"
log_info "Output: $RUNNER_OUTPUT_DIR"
if [[ $RESUME_FROM -gt 0 ]]; then
    log_info "Resuming from run #$RESUME_FROM"
fi
if [[ -n "$SKIP_RUNS_RAW" ]]; then
    log_info "Skipping runs: $SKIP_RUNS_RAW"
fi
echo ""

# Check HF_TOKEN
if [[ -z "${HF_TOKEN:-}" ]]; then
    log_error "HF_TOKEN not set. Export it before running:"
    log_info "  export HF_TOKEN=hf_your_token_here"
    exit 1
fi

# Create output directory and runner log
mkdir -p "$RUNNER_OUTPUT_DIR"
RUNNER_LOG="${RUNNER_OUTPUT_DIR}/runner_$(date +%Y%m%d_%H%M%S).log"
RUNNER_SUMMARY="${RUNNER_OUTPUT_DIR}/runner_summary_$(date +%Y%m%d_%H%M%S).md"

# Log to file and terminal
exec > >(tee -a "$RUNNER_LOG") 2>&1

# Compute total estimated time
TOTAL_EST=0
PRE_ACTIVE=0
for ((idx=0; idx<TOTAL_RUNS; idx++)); do
    run_n=$((idx + 1))
    if should_skip "$run_n"; then
        continue
    fi
    est=$(estimate_run_seconds "${RUNS[$idx]}")
    TOTAL_EST=$((TOTAL_EST + est + PAUSE_BETWEEN_RUNS))
    PRE_ACTIVE=$((PRE_ACTIVE + 1))
done
# Remove trailing pause (none after last active run)
if [[ $PRE_ACTIVE -gt 0 ]]; then
    TOTAL_EST=$((TOTAL_EST - PAUSE_BETWEEN_RUNS))
fi

echo "============================================="
echo "Benchmark Runner Log"
echo "Started: $(date)"
echo "Host: $(hostname)"
echo "Command: $ORIGINAL_CMD"
echo "Suite: $SUITE ($TOTAL_RUNS runs)"
echo "Estimated total time: $(fmt_duration $TOTAL_EST)"
echo "============================================="
echo ""

# -----------------------------------------------------------------------------
# Execute runs
# -----------------------------------------------------------------------------

RESULTS=()       # "pass" or "fail" per run
DURATIONS=()     # elapsed seconds per run
REPORT_DIRS=()   # output directory per run (may have multiple for --compare)
RUN_METRICS=()   # short metric summary per run
RUNNER_START_TS=$(date +%s)
PASSED=0
FAILED=0
SKIPPED=0
INTERRUPTED=false

# Adaptive ETA: track cumulative actual vs estimated seconds
ACTUAL_SUM=0
ESTIMATED_SUM=0

# SIGINT trap: mark interrupted so we produce a summary with whatever completed
handle_sigint() {
    echo ""
    log_warn "Interrupted (Ctrl+C) — producing summary for completed runs..."
    INTERRUPTED=true

    # If we're mid-run, mark it as failed
    if [[ ${#RESULTS[@]} -lt $TOTAL_RUNS ]]; then
        local remaining=$((TOTAL_RUNS - ${#RESULTS[@]}))
        for ((j=0; j<remaining; j++)); do
            if [[ $j -eq 0 ]]; then
                RESULTS+=("fail")
                NOW_TS=$(date +%s)
                DURATIONS+=($((NOW_TS - RUN_START_TS)))
                REPORT_DIRS+=("-")
                RUN_METRICS+=("")
                FAILED=$((FAILED + 1))
            else
                RESULTS+=("skipped")
                DURATIONS+=(0)
                REPORT_DIRS+=("-")
                RUN_METRICS+=("")
                SKIPPED=$((SKIPPED + 1))
            fi
        done
    fi
}
trap handle_sigint INT

for ((i=0; i<TOTAL_RUNS; i++)); do
    [[ "$INTERRUPTED" == true ]] && break
    RUN_NUM=$((i + 1))

    # Handle --resume / --skip
    if should_skip "$RUN_NUM"; then
        RESULTS+=("skipped")
        DURATIONS+=(0)
        REPORT_DIRS+=("-")
        RUN_METRICS+=("")
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    echo ""

    # Progress and ETA
    NOW_TS=$(date +%s)
    SUITE_ELAPSED=$((NOW_TS - RUNNER_START_TS))
    RUN_EST=$(estimate_run_seconds "${RUNS[$i]}")

    # Apply adaptive scaling if we have prior data
    if [[ $ESTIMATED_SUM -gt 0 ]]; then
        # Scale factor: how much longer runs actually take vs estimates
        # Use integer math: (actual * 100) / estimated, then apply
        SCALE_100=$((ACTUAL_SUM * 100 / ESTIMATED_SUM))
        RUN_EST=$((RUN_EST * SCALE_100 / 100))
    fi

    # Calculate remaining estimate: this run + future runs + pauses
    REMAINING_EST=$RUN_EST
    for ((k=i+1; k<TOTAL_RUNS; k++)); do
        FUTURE_EST=$(estimate_run_seconds "${RUNS[$k]}")
        if [[ $ESTIMATED_SUM -gt 0 ]]; then
            FUTURE_EST=$((FUTURE_EST * SCALE_100 / 100))
        fi
        REMAINING_EST=$((REMAINING_EST + FUTURE_EST + PAUSE_BETWEEN_RUNS))
    done
    ETA_TS=$((NOW_TS + REMAINING_EST))
    ETA_TIME=$(date -d "@$ETA_TS" +%H:%M 2>/dev/null || date -r "$ETA_TS" +%H:%M 2>/dev/null || echo "")

    log_header "Run $RUN_NUM of $TOTAL_RUNS: ${LABELS[$i]}"
    echo -e "  ${CYAN}./scripts/bench.sh ${RUNS[$i]}${NC}"
    echo ""
    log_info "Elapsed: $(fmt_duration $SUITE_ELAPSED) | This run: ~$(fmt_duration $RUN_EST) | Remaining: ~$(fmt_duration $REMAINING_EST)${ETA_TIME:+ | ETA: $ETA_TIME}"

    # Snapshot existing report directories before the run
    DIRS_BEFORE=$(ls -d "${RUNNER_OUTPUT_DIR}"/*/ 2>/dev/null | sort)

    RUN_START_TS=$(date +%s)

    # Run bench.sh, capturing exit code
    set +e
    bash "$BENCH_SH" ${RUNS[$i]} --output-dir "$RUNNER_OUTPUT_DIR"
    EXIT_CODE=$?
    set -e

    RUN_END_TS=$(date +%s)
    RUN_ELAPSED=$((RUN_END_TS - RUN_START_TS))
    DURATIONS+=($RUN_ELAPSED)

    # Update adaptive ETA data
    RAW_EST=$(estimate_run_seconds "${RUNS[$i]}")
    ACTUAL_SUM=$((ACTUAL_SUM + RUN_ELAPSED))
    ESTIMATED_SUM=$((ESTIMATED_SUM + RAW_EST))

    # Find new report directories by diffing before/after snapshots
    DIRS_AFTER=$(ls -d "${RUNNER_OUTPUT_DIR}"/*/ 2>/dev/null | sort)
    NEW_DIRS=$(comm -13 <(echo "$DIRS_BEFORE") <(echo "$DIRS_AFTER") 2>/dev/null || echo "")
    if [[ -n "$NEW_DIRS" ]]; then
        REPORT_DIRS+=("$(echo "$NEW_DIRS" | tr '\n' ',' | sed 's/,$//')")
    else
        REPORT_DIRS+=("-")
    fi

    if [[ $EXIT_CODE -eq 0 ]]; then
        log_ok "Run $RUN_NUM completed in $(fmt_duration $RUN_ELAPSED)"
        RESULTS+=("pass")
        PASSED=$((PASSED + 1))

        # Extract and display key metrics from new benchmark.log(s)
        METRIC_SUMMARY=""
        if [[ -n "$NEW_DIRS" ]]; then
            echo ""
            while IFS= read -r dir; do
                [[ -z "$dir" ]] && continue
                local_log="${dir}benchmark.log"
                if [[ -f "$local_log" ]]; then
                    # Derive a label from the directory name (e.g., "round_robin" or "prefix")
                    dir_base=$(basename "$dir")
                    mode_label=$(echo "$dir_base" | grep -oP '(round_robin|prefix|random)' || echo "$dir_base")
                    extract_metrics "$local_log" "$mode_label"
                    extract_bucket_improvement "$local_log" "$mode_label"

                    # Capture one-line summary for the final table
                    json_line=$(grep "BENCHMARK_STATS_JSON:" "$local_log" 2>/dev/null | tail -1 | sed 's/.*BENCHMARK_STATS_JSON://' || echo "")
                    if [[ -n "$json_line" ]]; then
                        hr=$(echo "$json_line" | grep -oP '"cache_hit_rate_pct":\s*[0-9.]+' | grep -oP '[0-9.]+$' || echo "")
                        ti=$(echo "$json_line" | grep -oP '"ttft_improvement_pct":\s*-?[0-9.]+' | grep -oP '\-?[0-9.]+$' || echo "")
                        if [[ -n "$hr" ]]; then
                            METRIC_SUMMARY+="${mode_label}: ${hr}% hit"
                            [[ -n "$ti" ]] && METRIC_SUMMARY+=", ${ti}% improv"
                            METRIC_SUMMARY+=" | "
                        fi
                    fi
                fi
            done <<< "$NEW_DIRS"
            # Trim trailing " | "
            METRIC_SUMMARY="${METRIC_SUMMARY% | }"
        fi
        RUN_METRICS+=("$METRIC_SUMMARY")
    else
        log_error "Run $RUN_NUM failed (exit code $EXIT_CODE) after $(fmt_duration $RUN_ELAPSED)"
        RESULTS+=("fail")
        RUN_METRICS+=("")
        FAILED=$((FAILED + 1))

        if [[ "$STOP_ON_FAILURE" == true ]]; then
            log_error "Stopping suite due to --stop-on-failure"
            # Mark remaining as skipped
            for ((j=i+1; j<TOTAL_RUNS; j++)); do
                RESULTS+=("skipped")
                DURATIONS+=(0)
                REPORT_DIRS+=("-")
                RUN_METRICS+=("")
                SKIPPED=$((SKIPPED + 1))
            done
            break
        fi
    fi

    # Pause between runs (unless this is the last one or next will be skipped)
    if [[ $((i + 1)) -lt $TOTAL_RUNS && "$INTERRUPTED" != true ]]; then
        # Find the next run that won't be skipped
        NEXT_ACTIVE=""
        for ((na=i+1; na<TOTAL_RUNS; na++)); do
            if ! should_skip $((na + 1)); then
                NEXT_ACTIVE=$((na + 1))
                break
            fi
        done
        if [[ -n "$NEXT_ACTIVE" ]]; then
            log_info "Pausing ${PAUSE_BETWEEN_RUNS}s before next run (GPU cooldown)..."
            sleep "$PAUSE_BETWEEN_RUNS"
        fi
    fi
done

# Restore default SIGINT behavior for summary section
trap - INT

RUNNER_END_TS=$(date +%s)
RUNNER_ELAPSED=$((RUNNER_END_TS - RUNNER_START_TS))

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------

echo ""
log_header "Benchmark Runner Summary"
echo ""
echo "Suite:     $SUITE"
echo "Total:     $TOTAL_RUNS runs"
echo "Passed:    $PASSED"
echo "Failed:    $FAILED"
echo "Skipped:   $SKIPPED"
echo "Duration:  $(fmt_duration $RUNNER_ELAPSED)"
echo ""

# Print per-run table
printf "  %-4s  %-8s  %-10s  %-40s  %s\n" "#" "Status" "Duration" "Label" "Key Metrics"
printf "  %-4s  %-8s  %-10s  %-40s  %s\n" "---" "------" "--------" "-----" "-----------"
for ((i=0; i<TOTAL_RUNS; i++)); do
    RUN_NUM=$((i + 1))
    STATUS="${RESULTS[$i]}"
    DUR="${DURATIONS[$i]}"
    LABEL="${LABELS[$i]}"
    METRICS="${RUN_METRICS[$i]:-}"

    case "$STATUS" in
        pass)    STATUS_FMT="${GREEN}pass${NC}" ;;
        fail)    STATUS_FMT="${RED}FAIL${NC}" ;;
        skipped) STATUS_FMT="${YELLOW}skip${NC}" ;;
    esac

    if [[ "$DUR" -eq 0 ]]; then
        DUR_FMT="-"
    else
        DUR_FMT="$(fmt_duration $DUR)"
    fi

    printf "  %-4s  " "$RUN_NUM"
    echo -ne "$STATUS_FMT"
    printf "      %-10s  %-40s  %s\n" "$DUR_FMT" "$LABEL" "$METRICS"
done

echo ""
log_info "Runner log: $RUNNER_LOG"

# -----------------------------------------------------------------------------
# Generate markdown summary
# -----------------------------------------------------------------------------

{
    echo "# Benchmark Runner Summary"
    echo ""
    echo "- **Date:** $(date)"
    echo "- **Host:** $(hostname)"
    echo "- **Command:** \`$ORIGINAL_CMD\`"
    echo "- **Suite:** $SUITE ($TOTAL_RUNS runs)"
    echo "- **Total duration:** $(fmt_duration $RUNNER_ELAPSED)"
    echo "- **Results:** $PASSED passed, $FAILED failed, $SKIPPED skipped"
    echo ""
    echo "## Runs"
    echo ""
    echo "| # | Status | Duration | Label | Key Metrics | Report Dir |"
    echo "|---|--------|----------|-------|-------------|------------|"
    for ((i=0; i<TOTAL_RUNS; i++)); do
        RUN_NUM=$((i + 1))
        STATUS="${RESULTS[$i]}"
        DUR="${DURATIONS[$i]}"
        LABEL="${LABELS[$i]}"
        RDIR="${REPORT_DIRS[$i]}"
        METRICS="${RUN_METRICS[$i]:-}"

        if [[ "$DUR" -eq 0 ]]; then
            DUR_FMT="-"
        else
            DUR_FMT="$(fmt_duration $DUR)"
        fi

        echo "| $RUN_NUM | $STATUS | $DUR_FMT | $LABEL | ${METRICS:-—} | ${RDIR:-—} |"
    done
    echo ""
    echo "## Commands"
    echo ""
    for ((i=0; i<TOTAL_RUNS; i++)); do
        RUN_NUM=$((i + 1))
        echo "**Run $RUN_NUM — ${LABELS[$i]}:**"
        echo '```bash'
        echo "./scripts/bench.sh ${RUNS[$i]}"
        echo '```'
        echo ""
    done
    echo "---"
    echo "*Generated by bench-runner.sh*"
} > "$RUNNER_SUMMARY"

log_ok "Summary report: $RUNNER_SUMMARY"

# Final status
echo ""
if [[ $FAILED -eq 0 && $SKIPPED -eq 0 ]]; then
    log_ok "All $TOTAL_RUNS benchmarks completed successfully"
elif [[ $FAILED -eq 0 ]]; then
    log_ok "All executed benchmarks passed ($SKIPPED skipped)"
else
    log_warn "$FAILED of $TOTAL_RUNS benchmarks failed"
fi

# Suggest comparison if we have results
if [[ $PASSED -ge 2 ]]; then
    echo ""
    log_info "To compare results across runs:"
    echo "  python3 tests/integration/results_parser.py compare \\"
    echo "    ${RUNNER_OUTPUT_DIR}/*round_robin*/benchmark.log \\"
    echo "    ${RUNNER_OUTPUT_DIR}/*prefix*/benchmark.log"
fi

exit $FAILED
