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
# vLLM v1's gpu_cache_usage_perc counts only blocks held by RUNNING requests
# (cached prefixes are reclaimable = free), so the residency signal fires only
# when ACTIVE demand nears KV capacity. 0.50 sizes the KV cache toward live
# demand for the reference config (8B on 40GB A100, 30 users); the probe
# prints a measured suggestion for anything else.
GPU_MEM_UTIL="0.50"
THRESHOLD_ON="0.2"           # Server default; the ON leg's threshold
PROBE_DURATION="8m"
OUTPUT_ROOT="benchmark-reports/residency-ab_$(date +%Y%m%d_%H%M%S)"
SKIP_PROBE=false
PROBE_ONLY=false
WITH_LOAD_AWARE=false        # Default: --no-load-aware in BOTH legs (isolation)
WITH_RR_BASELINE=false       # Default: 2 prefix legs only (no --compare RR legs)
ORDER="off-first"
# Default 8192: covers the workload's 8k max prefix + output, and avoids the
# startup failure where a 128k native context can't fit one sequence in a
# deliberately-small KV cache (observed with Llama-3.1-8B at low mem-util).
MAX_MODEL_LEN="8192"
TP_SIZE=""
PREFIX_MAX_TOKENS=""
BUILD_IMAGE=false
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
    --gpu-mem-util F       vLLM GPU memory utilization (default: 0.50; tuned for
                           8B on 40GB A100 at 30 users). LOWER this to shrink KV
                           capacity toward live demand — see PRESSURE TUNING.
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
    --max-model-len N      vLLM max sequence length (default: 8192 — covers the
                           workload's 8k max prefix; prevents vLLM refusing to
                           start when the native context can't fit in a small KV)
    --tp N                 Forwarded to bench.sh
    --prefix-max-tokens N  Forwarded to bench.sh
    --build-image          Rebuild ranvier:latest from this checkout before the
                           runs (forwarded to bench.sh). REQUIRED when the
                           branch carries C++ changes — e.g. the dual-name
                           cache-usage parser fix — or the servers run a stale
                           build and the A/B is invalid.
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

PRESSURE TUNING (if the probe shows 0 downgrades):
    vLLM v1 semantics: gpu_cache_usage_perc counts only blocks held by RUNNING
    requests — cached prefixes are reclaimable and count as FREE (the engine
    log shows "Running: 0 reqs ... usage: 0.0%" even with a hot prefix cache).
    The residency signal (1 - usage) therefore crosses the threshold only when
    ACTIVE demand (in-flight prefill+decode tokens) nears KV capacity.
    The probe samples the gauge and prints a measured --gpu-mem-util
    suggestion on failure. Levers, in order of preference:
    1. Lower --gpu-mem-util — shrink KV capacity toward live demand (primary)
    2. Raise --users — more in-flight tokens holding blocks
    3. Raise --max-tokens (bench.sh) — longer decode holds blocks longer
    4. Bigger prefixes (--prefix-max-tokens / LARGE_PREFIX_MIN_TOKENS)
    Note: --churn-active shapes eviction realism (stale-hit frequency), but it
    does NOT move the usage gauge — don't use it to force firing. And avoid
    raising --threshold: that changes the decision rule under test.
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
        --build-image)      BUILD_IMAGE=true; shift ;;
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

# Usage level (%) above which the residency downgrade can fire:
# residency = 1 - usage < threshold  ⇔  usage > 1 - threshold.
FIRE_PCT=$(awk -v t="$THRESHOLD_ON" 'BEGIN{printf "%.0f", (1-t)*100}')

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

# ---------------------------------------------------------------------------
# vLLM KV-usage sampler
# ---------------------------------------------------------------------------
# Samples vllm:gpu_cache_usage_perc from the local vLLM backends (bench.sh
# default ports 8000..8015) every 5s — the same instantaneous gauge, at the
# same cadence, that Ranvier's health scrape feeds the residency signal, so
# its peak ≈ the best the router could have seen. vLLM v1 counts only blocks
# held by RUNNING requests (cached prefixes are reclaimable = free), which is
# why this gauge — not prefix-cache fullness — decides whether residency
# fires. Best-effort: dead/not-yet-started ports are skipped silently.
USAGE_SAMPLER_PID=""

start_usage_sampler() {
    local out="$1"
    echo "epoch,port,kv_cache_usage" > "$out"
    (
        while :; do
            ts=$(date +%s)
            for port in $(seq 8000 8015); do
                # vLLM v1 renamed the gauge (gpu_ -> kv_); match either. A
                # responding endpoint with NO match records "nomatch" — that
                # distinguishes "metric renamed/absent" (Ranvier is blind too)
                # from "backend not up yet" (no row at all).
                body=$(curl -sf --max-time 2 "http://localhost:$port/metrics" 2>/dev/null || true)
                [[ -z "$body" ]] && continue
                v=$(printf '%s\n' "$body" \
                    | awk '/^vllm:(gpu|kv)_cache_usage_perc/ {print $NF; exit}' || true)
                echo "$ts,$port,${v:-nomatch}"
            done
            sleep 5
        done >> "$out"
    ) &
    USAGE_SAMPLER_PID=$!
}

stop_usage_sampler() {
    if [[ -n "${USAGE_SAMPLER_PID:-}" ]] && kill -0 "$USAGE_SAMPLER_PID" 2>/dev/null; then
        kill "$USAGE_SAMPLER_PID" 2>/dev/null || true
        wait "$USAGE_SAMPLER_PID" 2>/dev/null || true
    fi
    USAGE_SAMPLER_PID=""
}
trap stop_usage_sampler EXIT

# Prints "<peak> <mean> <nomatch_count>": peak/mean in percent across numeric
# samples (or NA NA), plus how many scrapes responded WITHOUT the cache-usage
# metric. nomatch>0 with no numeric samples means the metric name is wrong for
# this vLLM build — the router's own scrape is blind to it as well.
usage_summary() {
    local csv="$1"
    if [[ ! -f "$csv" ]]; then echo "NA NA 0"; return; fi
    awk -F, '
        NR > 1 && $3 == "nomatch" { nm++ }
        NR > 1 && $3 ~ /^[0-9.eE+-]+$/ { if ($3+0 > max) max=$3+0; s+=$3; n++ }
        END {
            if (n) printf "%.1f %.1f %d", max*100, s/n*100, nm+0
            else   printf "NA NA %d", nm+0
        }' "$csv"
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
    if [[ "$BUILD_IMAGE" = true ]]; then
        args+=(--build-image)
    fi
    echo "${args[@]}"
}

# Run one leg. $1 = label, $2 = threshold, $3 = leg output dir.
# Each bench.sh invocation cold-starts vLLM and the Ranvier cluster and tears
# both down on exit, so legs are symmetric by construction. A KV-usage sampler
# runs alongside and leaves <outdir>/vllm_usage_samples.csv as the pressure
# record for the leg.
run_leg() {
    local label="$1" threshold="$2" outdir="$3"
    local args
    args=$(build_bench_args "$threshold" "$outdir")

    log_header "LEG: $label (RANVIER_CACHE_RESIDENCY_THRESHOLD=$threshold)"
    log_info "bench.sh $args $EXTRA_BENCH_ARGS"
    echo ""

    mkdir -p "$outdir"
    start_usage_sampler "$outdir/vllm_usage_samples.csv"

    # shellcheck disable=SC2086
    if ! "$BENCH" $args $EXTRA_BENCH_ARGS; then
        stop_usage_sampler
        log_error "bench.sh failed for leg '$label' — see logs under $outdir"
        exit 1
    fi
    stop_usage_sampler

    local leg_peak leg_mean leg_nomatch
    read -r leg_peak leg_mean leg_nomatch <<< "$(usage_summary "$outdir/vllm_usage_samples.csv")"
    log_info "Leg '$label' sampled KV usage: peak ${leg_peak}%, mean ${leg_mean}%  -> $outdir/vllm_usage_samples.csv"
    if [[ "$leg_peak" == "NA" && "$leg_nomatch" -gt 0 ]]; then
        log_warn "vLLM /metrics responded ($leg_nomatch scrapes) but exposed neither"
        log_warn "vllm:gpu_cache_usage_perc nor vllm:kv_cache_usage_perc — the residency"
        log_warn "signal is blind on this vLLM build. Identify the real gauge name with:"
        log_warn "  curl -s localhost:8000/metrics | grep -iE '^vllm:.*(cache|usage)'"
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
    read -r PROBE_PEAK PROBE_MEAN PROBE_NOMATCH <<< "$(usage_summary "$PROBE_DIR/vllm_usage_samples.csv")"
    log_info "Sampled KV usage: peak ${PROBE_PEAK}%, mean ${PROBE_MEAN}% (residency fires above ${FIRE_PCT}% at threshold $THRESHOLD_ON)"

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
        log_error "residency_route_downgrades_total == 0 — an A/B now would measure nothing."

        if [[ "$PROBE_PEAK" == "NA" && "$PROBE_NOMATCH" -gt 0 ]]; then
            # Root cause class, not a tuning problem: the cache-usage gauge is
            # missing from this vLLM build's /metrics. Ranvier's health scrape
            # looks for the same names, so residency_weight was a constant 1.0
            # all run — no amount of pressure can fire the downgrade.
            log_error "vLLM /metrics responded ($PROBE_NOMATCH scrapes) but exposed neither"
            log_error "vllm:gpu_cache_usage_perc nor vllm:kv_cache_usage_perc. The router's"
            log_error "scrape is blind to cache usage on this vLLM build — pressure tuning"
            log_error "cannot help until the signal parses."
            echo ""
            log_info "Identify the real gauge name (fast — model weights are already cached):"
            log_info "  curl -s localhost:8000/metrics | grep -iE '^vllm:.*(cache|usage)'"
            log_info "with a vLLM instance up, and check the Ranvier image carries the"
            log_info "dual-name parser fix (health_service.cpp): rerun with --build-image"
            log_info "so ranvier:latest is rebuilt from this checkout."
            exit 1
        fi

        if [[ "$PROBE_PEAK" == "NA" ]]; then
            log_error "No KV-usage samples at all (endpoints never responded to the sampler"
            log_error "while bench.sh's own health checks passed) — sampler environment issue."
            log_info "Inspect: $PROBE_DIR/vllm_usage_samples.csv"
            exit 1
        fi

        if awk -v p="$PROBE_PEAK" -v f="$FIRE_PCT" 'BEGIN{exit !(p >= f)}'; then
            # Gauge crossed the line at some sampled instant yet the router never
            # downgraded: the pressured windows were likely too brief for the 5s
            # health scrape, or ART hits aren't landing on pressured backends.
            log_warn "Sampled peak (${PROBE_PEAK}%) DID cross ${FIRE_PCT}% — but the router never saw it"
            log_warn "at scrape time, or no ART hit targeted a pressured backend. Lengthen the"
            log_warn "probe (--probe-duration 15m), raise --users for sustained pressure, and check"
            log_warn "the cache hit rate in the probe report before re-running."
        else
            echo ""
            # vLLM v1: the usage gauge counts only blocks held by RUNNING requests,
            # so firing requires ACTIVE demand near KV capacity. Size capacity so
            # the demand this probe actually measured sits at ~85% of it.
            KV_GIB=$(grep -h "Available KV cache memory" /tmp/vllm_gpu0.log 2>/dev/null | tail -1 | grep -oE '[0-9]+(\.[0-9]+)?' | tail -1 || true)
            VRAM_GIB=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1 | awk '{printf "%.1f", $1/1024}' || true)
            if [[ -n "$KV_GIB" && -n "$VRAM_GIB" && "$PROBE_PEAK" != "NA" ]]; then
                SUGGEST_UTIL=$(awk -v vram="$VRAM_GIB" -v util="$GPU_MEM_UTIL" -v kv="$KV_GIB" -v peak="$PROBE_PEAK" '
                    BEGIN {
                        nonkv = vram*util - kv            # weights + activations + graphs
                        need_kv = kv * (peak/100) / 0.85  # capacity putting observed demand at 85%
                        if (need_kv < 1.5) need_kv = 1.5  # keep room for one max-len sequence
                        s = (nonkv + need_kv) / vram
                        if (s > 0.92) s = 0.92
                        printf "%.2f", s
                    }')
                NONKV_GIB=$(awk -v v="$VRAM_GIB" -v u="$GPU_MEM_UTIL" -v k="$KV_GIB" 'BEGIN{printf "%.1f", v*u-k}')
                log_info "Measured on this box: KV capacity ${KV_GIB} GiB at util ${GPU_MEM_UTIL} (non-KV ≈ ${NONKV_GIB} GiB of ${VRAM_GIB} GiB/GPU)."
                log_info "Suggested next probe: --gpu-mem-util ${SUGGEST_UTIL}  (sizes KV so this probe's peak demand ≈ 85% of capacity)"
            else
                log_info "Could not compute a suggestion (missing vLLM log / nvidia-smi / samples)."
            fi
            log_info "Other levers (vLLM v1: only ACTIVE in-flight demand moves the gauge):"
            log_info "  --users $((USERS + 10))   |   --max-tokens up (via --extra-bench-args)   |   bigger prefixes"
            log_info "(--churn-active changes eviction realism, not the gauge — don't use it to force firing)"
        fi
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
read -r OFF_PEAK OFF_MEAN OFF_NOMATCH <<< "$(usage_summary "$OFF_DIR/vllm_usage_samples.csv")"
read -r ON_PEAK ON_MEAN ON_NOMATCH <<< "$(usage_summary "$ON_DIR/vllm_usage_samples.csv")"
if [[ ( "$OFF_PEAK" == "NA" && "$OFF_NOMATCH" -gt 0 ) || ( "$ON_PEAK" == "NA" && "$ON_NOMATCH" -gt 0 ) ]]; then
    log_error "INVALID: cache-usage gauge missing from vLLM /metrics in at least one leg —"
    log_error "the residency signal was blind (constant 1.0); the legs measured nothing."
    VALID=false
fi
echo "OFF leg ($OFF_LEG):"
leg_counters "$OFF_LEG"
echo "  kv_usage peak/mean: ${OFF_PEAK}% / ${OFF_MEAN}%"
echo "ON leg ($ON_LEG):"
leg_counters "$ON_LEG"
echo "  kv_usage peak/mean: ${ON_PEAK}% / ${ON_MEAN}%"
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
    echo "  kv_usage peak/mean: ${OFF_PEAK}% / ${OFF_MEAN}%"
    echo "ON ($ON_LEG):"
    leg_counters "$ON_LEG"
    echo "  kv_usage peak/mean: ${ON_PEAK}% / ${ON_MEAN}%"
    echo "(kv_usage = sampled vllm:gpu_cache_usage_perc; residency fires above ${FIRE_PCT}%)"
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
    echo "| Sampled KV usage peak/mean | ${OFF_PEAK}% / ${OFF_MEAN}% | ${ON_PEAK}% / ${ON_MEAN}% | — |"
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
