#!/bin/bash
# =============================================================================
# Cache-Residency Routing A/B Benchmark (Experiment E)
# =============================================================================
#
# Measures the TTFT effect of cache-residency-aware routing (#527) under real
# KV-cache pressure: two identical churn-workload runs, residency OFF
# (threshold 0.0) vs ON (threshold 0.2), preceded by a short pressure PROBE
# that verifies residency downgrades actually fire before committing to two
# long runs. A residency A/B where residency never fires measures nothing —
# every pre-June-2026 run had residency_route_downgrades_total == 0.
#
# Methodology: docs/benchmarks/cache-residency-ab-benchmark.md
# Requires:    GPUs + vLLM (same prerequisites as scripts/bench.sh)
#
# Usage:
#   export HF_TOKEN=hf_xxx
#   ./scripts/bench-residency-ab.sh                       # probe + 2x30m A/B
#   ./scripts/bench-residency-ab.sh --quick               # probe + 2x10m
#   ./scripts/bench-residency-ab.sh --probe-only          # just verify pressure
#   ./scripts/bench-residency-ab.sh --skip-probe          # straight to A/B
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_DIR"

BENCH="$SCRIPT_DIR/bench.sh"
PARSER="tests/integration/results_parser.py"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
log_header() { echo -e "\n${BOLD}${CYAN}$1${NC}"; echo -e "${CYAN}$(printf '─%.0s' {1..50})${NC}"; }
log_info()   { echo -e "${BLUE}▸${NC} $1"; }
log_ok()     { echo -e "${GREEN}✓${NC} $1"; }
log_warn()   { echo -e "${YELLOW}⚠${NC} $1"; }
log_error()  { echo -e "${RED}✗${NC} $1"; }

ORIGINAL_CMD="$0 $*"

# -----------------------------------------------------------------------------
# Defaults
# -----------------------------------------------------------------------------

MODEL="meta-llama/Llama-3.1-8B-Instruct"
DURATION="30m"
USERS="30"
GPU_MEM_UTIL="0.80"          # Deliberately low: less KV headroom -> pressure
THRESHOLD_ON="0.2"           # Server default; the ON leg's threshold
PROBE_DURATION="8m"
OUTPUT_ROOT="benchmark-reports/residency-ab_$(date +%Y%m%d_%H%M%S)"
SKIP_PROBE=false
PROBE_ONLY=false
WITH_LOAD_AWARE=false        # Default: --no-load-aware in BOTH legs (isolation)
WITH_RR_BASELINE=false       # Default: 2 prefix legs only (no --compare RR legs)
ORDER="off-first"
MAX_MODEL_LEN=""
TP_SIZE=""
PREFIX_MAX_TOKENS=""
EXTRA_BENCH_ARGS=""

# Churn workload knobs (forwarded to bench.sh -> locust as env vars)
CHURN_UNIVERSE="${CHURN_PREFIX_UNIVERSE:-200}"
CHURN_ACTIVE="${CHURN_ACTIVE_PREFIXES:-24}"
CHURN_ROT_SECS="${CHURN_ROTATION_SECONDS:-20}"
CHURN_STEP="${CHURN_ROTATION_STEP:-8}"
CHURN_SEED_VAL="${CHURN_SEED:-42}"

print_help() {
    cat << 'EOF'
Cache-Residency Routing A/B Benchmark (Experiment E)

USAGE:
    ./scripts/bench-residency-ab.sh [OPTIONS]

OPTIONS:
    --model MODEL          Model (default: meta-llama/Llama-3.1-8B-Instruct)
    --duration TIME        Duration per A/B leg (default: 30m; 30m minimum for
                           quotable numbers per the benchmark methodology)
    --users N              Concurrent users per leg (default: 30)
    --gpu-mem-util F       vLLM GPU memory utilization (default: 0.80).
                           LOWER this to shrink the KV cache and raise pressure.
    --threshold F          Residency threshold for the ON leg (default: 0.2)
    --probe-duration TIME  Pressure probe duration (default: 8m)
    --output-dir DIR       Output root (default: benchmark-reports/residency-ab_<ts>)
    --order ORDER          Leg order: off-first (default) | on-first
    --quick                10m legs, 5m probe (smoke test; NOT quotable numbers)
    --skip-probe           Skip the pressure probe (probe already passed)
    --probe-only           Run only the pressure probe and exit
    --with-load-aware      Keep load-aware routing ON in both legs (production-
                           shaped net effect). Default is --no-load-aware in both
                           legs so the delta isolates residency routing.
    --with-rr-baseline     Run each leg with --compare (adds a round-robin
                           baseline per leg; ~2x total runtime). Anchors the A/B
                           against environmental drift.
    --max-model-len N      Forwarded to bench.sh
    --tp N                 Forwarded to bench.sh
    --prefix-max-tokens N  Forwarded to bench.sh
    --churn-universe N     Churn prefix universe size (default: 200)
    --churn-active N       Active window size (default: 24)
    --churn-rotation-seconds N  Window advance period (default: 20)
    --churn-step N         Prefixes rotated per advance (default: 8)
    --churn-seed N         Universe content seed (default: 42)
    --extra-bench-args "..."    Extra args appended to every bench.sh call
    -h, --help             This help

WHAT IT DOES:
    1. PROBE: short churn run with residency ON. Gate: the run must show
       residency_route_downgrades_total > 0 (the KV cache crossed the
       threshold and the router actually diverted). If 0, the A/B would be
       meaningless; the script stops with tuning guidance.
    2. A/B: two identical runs (vLLM restarted cold + 1m warmup each):
         OFF: --cache-residency-threshold 0.0
         ON:  --cache-residency-threshold <threshold>
    3. Compares the two prefix legs with results_parser.py, checks validity
       (OFF leg downgrades == 0, ON leg > 0, gossip flowing in both), and
       writes REPORT.md + ab-summary.txt into the output dir.

PRESSURE TUNING (if the probe shows 0 downgrades), in order of preference:
    1. Lower --gpu-mem-util (e.g. 0.80 -> 0.75) — smaller KV cache
    2. Raise --churn-active (24 -> 40) — bigger live working set
    3. Raise --users (30 -> 40)
    4. Raise --prefix-max-tokens / LARGE_PREFIX_MIN_TOKENS — bigger prefixes
    Avoid raising --threshold to force firing: that changes what is measured.
EOF
}

# -----------------------------------------------------------------------------
# Argument parsing
# -----------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case $1 in
        --model)            MODEL="$2"; shift 2 ;;
        --duration)         DURATION="$2"; shift 2 ;;
        --users)            USERS="$2"; shift 2 ;;
        --gpu-mem-util)     GPU_MEM_UTIL="$2"; shift 2 ;;
        --threshold)        THRESHOLD_ON="$2"; shift 2 ;;
        --probe-duration)   PROBE_DURATION="$2"; shift 2 ;;
        --output-dir)       OUTPUT_ROOT="$2"; shift 2 ;;
        --order)            ORDER="$2"; shift 2 ;;
        --quick)            DURATION="10m"; PROBE_DURATION="5m"; shift ;;
        --skip-probe)       SKIP_PROBE=true; shift ;;
        --probe-only)       PROBE_ONLY=true; shift ;;
        --with-load-aware)  WITH_LOAD_AWARE=true; shift ;;
        --with-rr-baseline) WITH_RR_BASELINE=true; shift ;;
        --max-model-len)    MAX_MODEL_LEN="$2"; shift 2 ;;
        --tp)               TP_SIZE="$2"; shift 2 ;;
        --prefix-max-tokens) PREFIX_MAX_TOKENS="$2"; shift 2 ;;
        --churn-universe)   CHURN_UNIVERSE="$2"; shift 2 ;;
        --churn-active)     CHURN_ACTIVE="$2"; shift 2 ;;
        --churn-rotation-seconds) CHURN_ROT_SECS="$2"; shift 2 ;;
        --churn-step)       CHURN_STEP="$2"; shift 2 ;;
        --churn-seed)       CHURN_SEED_VAL="$2"; shift 2 ;;
        --extra-bench-args) EXTRA_BENCH_ARGS="$2"; shift 2 ;;
        -h|--help)          print_help; exit 0 ;;
        *)                  log_error "Unknown option: $1"; print_help; exit 1 ;;
    esac
done

if [[ "$ORDER" != "off-first" && "$ORDER" != "on-first" ]]; then
    log_error "--order must be off-first or on-first (got: $ORDER)"
    exit 1
fi

if [[ ! -x "$BENCH" ]]; then
    log_error "bench.sh not found or not executable at $BENCH"
    exit 1
fi

# Churn knobs flow: wrapper -> env -> bench.sh -> locust container
export CHURN_PREFIX_UNIVERSE="$CHURN_UNIVERSE"
export CHURN_ACTIVE_PREFIXES="$CHURN_ACTIVE"
export CHURN_ROTATION_SECONDS="$CHURN_ROT_SECS"
export CHURN_ROTATION_STEP="$CHURN_STEP"
export CHURN_SEED="$CHURN_SEED_VAL"

mkdir -p "$OUTPUT_ROOT"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# Sum a Ranvier counter across all shard lines of a Prometheus text dump.
# Prints an integer; prints "NA" when the file is missing or has no match.
sum_metric() {
    local file="$1" name="$2"
    if [[ ! -f "$file" ]]; then echo "NA"; return; fi
    awk -v pat="^(seastar_)?ranvier_${name}([{ ]|$)" '
        $0 !~ /^#/ && $0 ~ pat { s += $NF; found = 1 }
        END { if (found) printf "%.0f", s; else print "NA" }
    ' "$file"
}

# Most recent *_<mode> report dir under a given output dir. Prints empty when
# none exists ("|| true" keeps the failed glob from tripping set -e/pipefail).
find_leg_dir() {
    local root="$1" mode="$2"
    ls -td "$root"/*_"$mode" 2>/dev/null | head -1 || true
}

# Assemble per-leg bench.sh args. $1 = residency threshold, $2 = leg output dir.
build_bench_args() {
    local threshold="$1" outdir="$2"
    local args=()
    args+=(--model "$MODEL")
    args+=(--duration "$DURATION")
    args+=(--users "$USERS")
    args+=(--gpu-mem-util "$GPU_MEM_UTIL")
    args+=(--prompt-dist churn)
    args+=(--warmup)
    args+=(--cache-residency-threshold "$threshold")
    args+=(--output-dir "$outdir")
    if [[ "$WITH_LOAD_AWARE" = false ]]; then
        args+=(--no-load-aware)
    fi
    if [[ "$WITH_RR_BASELINE" = true ]]; then
        args+=(--compare)
    fi
    [[ -n "$MAX_MODEL_LEN" ]] && args+=(--max-model-len "$MAX_MODEL_LEN")
    [[ -n "$TP_SIZE" ]] && args+=(--tp "$TP_SIZE")
    [[ -n "$PREFIX_MAX_TOKENS" ]] && args+=(--prefix-max-tokens "$PREFIX_MAX_TOKENS")
    echo "${args[@]}"
}

# Run one leg. $1 = label, $2 = threshold, $3 = leg output dir.
# Each bench.sh invocation cold-starts vLLM and the Ranvier cluster and tears
# both down on exit, so legs are symmetric by construction.
run_leg() {
    local label="$1" threshold="$2" outdir="$3"
    local args
    args=$(build_bench_args "$threshold" "$outdir")

    log_header "LEG: $label (RANVIER_CACHE_RESIDENCY_THRESHOLD=$threshold)"
    log_info "bench.sh $args $EXTRA_BENCH_ARGS"
    echo ""

    # shellcheck disable=SC2086
    if ! "$BENCH" $args $EXTRA_BENCH_ARGS; then
        log_error "bench.sh failed for leg '$label' — see logs under $outdir"
        exit 1
    fi
}

# Print + record one leg's residency/gossip counters. $1 = leg dir.
leg_counters() {
    local leg_dir="$1"
    local prom="$leg_dir/prometheus_metrics.txt"
    echo "  downgrades: $(sum_metric "$prom" "router_residency_route_downgrades_total")"
    echo "  cache_states_sent: $(sum_metric "$prom" "gossip_cache_states_sent_total")"
    echo "  cache_states_received: $(sum_metric "$prom" "gossip_cache_states_received_total")"
    echo "  residency_cache_size: $(sum_metric "$prom" "router_residency_cache_size")"
    echo "  load_aware_fallbacks: $(sum_metric "$prom" "routing_load_aware_fallbacks_total")"
}

# -----------------------------------------------------------------------------
# Banner
# -----------------------------------------------------------------------------

log_header "Cache-Residency A/B Benchmark"
log_info "Model:           $MODEL"
log_info "Duration:        $DURATION per leg (probe: $PROBE_DURATION)"
log_info "Users:           $USERS"
log_info "GPU mem util:    $GPU_MEM_UTIL  (lower = more cache pressure)"
log_info "Threshold (ON):  $THRESHOLD_ON   (OFF leg: 0.0)"
log_info "Load-aware:      $( [[ "$WITH_LOAD_AWARE" = true ]] && echo "ON in both legs" || echo "OFF in both legs (isolated residency delta)" )"
log_info "RR baseline:     $( [[ "$WITH_RR_BASELINE" = true ]] && echo "yes (--compare per leg)" || echo "no (prefix legs only)" )"
log_info "Leg order:       $ORDER"
log_info "Churn workload:  universe=$CHURN_UNIVERSE active=$CHURN_ACTIVE step=$CHURN_STEP/${CHURN_ROT_SECS}s seed=$CHURN_SEED_VAL"
log_info "Output:          $OUTPUT_ROOT"
echo ""
log_warn "Each leg cold-starts vLLM (model load adds minutes per leg). Expect"
log_warn "total wall time well above 2x duration; check ETA banners per leg."

# -----------------------------------------------------------------------------
# Step 1 — Pressure probe (residency ON; gate on downgrades > 0)
# -----------------------------------------------------------------------------

if [[ "$SKIP_PROBE" = false ]]; then
    PROBE_DIR="$OUTPUT_ROOT/probe"
    SAVED_DURATION="$DURATION"
    DURATION="$PROBE_DURATION"
    run_leg "PRESSURE PROBE" "$THRESHOLD_ON" "$PROBE_DIR"
    DURATION="$SAVED_DURATION"

    PROBE_LEG=$(find_leg_dir "$PROBE_DIR" "prefix")
    PROBE_PROM="$PROBE_LEG/prometheus_metrics.txt"
    PROBE_DOWNGRADES=$(sum_metric "$PROBE_PROM" "router_residency_route_downgrades_total")
    PROBE_RECEIVED=$(sum_metric "$PROBE_PROM" "gossip_cache_states_received_total")

    log_header "Probe Result"
    log_info "Report dir: ${PROBE_LEG:-<missing>}"
    leg_counters "$PROBE_LEG"

    if [[ "$PROBE_DOWNGRADES" == "NA" ]]; then
        log_error "Could not read $PROBE_PROM — probe inconclusive. Check the run logs."
        exit 1
    fi
    if [[ "$PROBE_RECEIVED" == "NA" || "$PROBE_RECEIVED" == "0" ]]; then
        log_error "gossip_cache_states_received_total is 0 — the residency signal is not"
        log_error "flowing (gossip or vLLM /metrics scrape broken). Fix before any A/B."
        exit 1
    fi
    if [[ "$PROBE_DOWNGRADES" == "0" ]]; then
        log_error "residency_route_downgrades_total == 0: the KV cache never crossed the"
        log_error "threshold (usage stayed below ~$(python3 -c "print(f'{(1-float('$THRESHOLD_ON'))*100:.0f}')" 2>/dev/null || echo 80)%) — an A/B now would measure nothing."
        echo ""
        log_info "Raise the pressure and re-probe (in order of preference):"
        log_info "  1. --gpu-mem-util $(python3 -c "print(f'{float('$GPU_MEM_UTIL')-0.05:.2f}')" 2>/dev/null || echo "lower")  (smaller KV cache)"
        log_info "  2. --churn-active $((CHURN_ACTIVE + 16))  (bigger live working set)"
        log_info "  3. --users $((USERS + 10))"
        log_info "  4. --prefix-max-tokens / LARGE_PREFIX_MIN_TOKENS up (bigger prefixes)"
        log_info "Also check vLLM KV capacity: grep 'KV cache' /tmp/vllm_gpu0.log"
        exit 1
    fi

    log_ok "Probe PASSED: $PROBE_DOWNGRADES residency downgrades — pressure is real."
    if [[ "$PROBE_ONLY" = true ]]; then
        log_ok "--probe-only: stopping here. Re-run with --skip-probe to do the A/B."
        exit 0
    fi
elif [[ "$PROBE_ONLY" = true ]]; then
    log_error "--probe-only and --skip-probe are mutually exclusive"
    exit 1
fi

# -----------------------------------------------------------------------------
# Step 2 — The A/B legs
# -----------------------------------------------------------------------------

OFF_DIR="$OUTPUT_ROOT/off"
ON_DIR="$OUTPUT_ROOT/on"

if [[ "$ORDER" == "off-first" ]]; then
    run_leg "RESIDENCY OFF (baseline)" "0.0" "$OFF_DIR"
    run_leg "RESIDENCY ON" "$THRESHOLD_ON" "$ON_DIR"
else
    run_leg "RESIDENCY ON" "$THRESHOLD_ON" "$ON_DIR"
    run_leg "RESIDENCY OFF (baseline)" "0.0" "$OFF_DIR"
fi

OFF_LEG=$(find_leg_dir "$OFF_DIR" "prefix")
ON_LEG=$(find_leg_dir "$ON_DIR" "prefix")

if [[ -z "$OFF_LEG" || -z "$ON_LEG" ]]; then
    log_error "Could not locate prefix report dirs (off='$OFF_LEG' on='$ON_LEG')"
    exit 1
fi

# -----------------------------------------------------------------------------
# Step 3 — Validity checks + comparison
# -----------------------------------------------------------------------------

log_header "Validity Checks"

OFF_PROM="$OFF_LEG/prometheus_metrics.txt"
ON_PROM="$ON_LEG/prometheus_metrics.txt"
OFF_DOWN=$(sum_metric "$OFF_PROM" "router_residency_route_downgrades_total")
ON_DOWN=$(sum_metric "$ON_PROM" "router_residency_route_downgrades_total")
OFF_RECV=$(sum_metric "$OFF_PROM" "gossip_cache_states_received_total")
ON_RECV=$(sum_metric "$ON_PROM" "gossip_cache_states_received_total")

VALID=true
echo "OFF leg ($OFF_LEG):"
leg_counters "$OFF_LEG"
echo "ON leg ($ON_LEG):"
leg_counters "$ON_LEG"
echo ""

if [[ "$OFF_DOWN" != "0" ]]; then
    log_error "INVALID: OFF leg shows $OFF_DOWN downgrades (expected 0). The toggle did"
    log_error "not take effect — verify the 'Effective Routing Config' banner in the OFF run log."
    VALID=false
fi
if [[ "$ON_DOWN" == "0" || "$ON_DOWN" == "NA" ]]; then
    log_error "INCONCLUSIVE: ON leg shows no residency downgrades — residency never fired"
    log_error "during the measured run (pressure drifted below threshold?). The delta is noise."
    VALID=false
fi
if [[ "$OFF_RECV" == "0" || "$ON_RECV" == "0" || "$OFF_RECV" == "NA" || "$ON_RECV" == "NA" ]]; then
    log_warn "gossip_cache_states_received_total is 0/unreadable in at least one leg — signal plumbing suspect."
    VALID=false
fi
if [[ "$VALID" = true ]]; then
    log_ok "Validity checks passed (OFF: 0 downgrades; ON: $ON_DOWN downgrades; gossip flowing)"
fi

log_header "Comparison (OFF = baseline, ON = new)"

SUMMARY="$OUTPUT_ROOT/ab-summary.txt"
{
    echo "Cache-Residency A/B Summary"
    echo "Command: $ORIGINAL_CMD"
    echo "Git Commit: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
    echo "Date: $(date -Iseconds)"
    echo "Valid: $VALID"
    echo ""
    echo "Leg counters:"
    echo "OFF ($OFF_LEG):"
    leg_counters "$OFF_LEG"
    echo "ON ($ON_LEG):"
    leg_counters "$ON_LEG"
    echo ""
} > "$SUMMARY"

if [[ -f "$PARSER" ]]; then
    if python3 "$PARSER" compare "$OFF_LEG/benchmark.log" "$ON_LEG/benchmark.log" >> "$SUMMARY" 2>&1; then
        log_ok "Comparison appended to $SUMMARY"
    else
        log_warn "results_parser comparison failed — inspect the logs manually"
    fi
    cat "$SUMMARY"
else
    log_warn "$PARSER not found; compare the benchmark logs manually"
fi

# -----------------------------------------------------------------------------
# Step 4 — Report skeleton
# -----------------------------------------------------------------------------

REPORT="$OUTPUT_ROOT/REPORT.md"
GPU_INFO=$(nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader 2>/dev/null | sort | uniq -c | sed 's/^ *//' || echo "nvidia-smi unavailable")
{
    echo "# Cache-Residency Routing A/B — Run Report"
    echo ""
    echo "- **Date:** $(date -Iseconds)"
    echo "- **Git commit:** $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
    echo "- **Command:** \`$ORIGINAL_CMD\`"
    echo "- **Hardware:** $GPU_INFO"
    echo "- **Model:** $MODEL"
    echo "- **Config:** ${USERS}u / $DURATION per leg, gpu-mem-util $GPU_MEM_UTIL, load-aware $( [[ "$WITH_LOAD_AWARE" = true ]] && echo on || echo off ), threshold ON=$THRESHOLD_ON"
    echo "- **Churn:** universe=$CHURN_UNIVERSE active=$CHURN_ACTIVE step=$CHURN_STEP/${CHURN_ROT_SECS}s seed=$CHURN_SEED_VAL"
    echo "- **Validity:** $VALID (OFF downgrades=$OFF_DOWN, ON downgrades=$ON_DOWN)"
    echo ""
    echo "## Headline (fill from ab-summary.txt)"
    echo ""
    echo "| Metric | Residency OFF | Residency ON | Delta |"
    echo "|--------|---------------|--------------|-------|"
    echo "| TTFT P50 | TBD | TBD | TBD |"
    echo "| TTFT P99 | TBD | TBD | TBD |"
    echo "| Cache Hit P99 | TBD | TBD | TBD |"
    echo "| Cache Miss P99 | TBD | TBD | TBD |"
    echo "| Client cache hit rate | TBD | TBD | TBD |"
    echo "| Incomplete/timeouts | TBD | TBD | TBD |"
    echo "| residency_route_downgrades_total | $OFF_DOWN | $ON_DOWN | — |"
    echo ""
    echo "Raw comparison: [ab-summary.txt](ab-summary.txt)"
    echo ""
    echo "Methodology: docs/benchmarks/cache-residency-ab-benchmark.md"
} > "$REPORT"

log_header "Done"
log_ok "Summary: $SUMMARY"
log_ok "Report skeleton: $REPORT"
if [[ "$VALID" != true ]]; then
    log_warn "Run flagged INVALID/INCONCLUSIVE — do not quote these numbers."
    exit 2
fi
