#!/bin/bash
# =============================================================================
# Ranvier Benchmark - Simplified Lambda Labs Edition
# =============================================================================
#
# One-command benchmarking for Lambda Labs multi-GPU instances.
# Auto-detects GPUs, starts vLLM, runs Ranvier benchmark, cleans up.
#
# Usage:
#   export HF_TOKEN=your_huggingface_token
#   ./scripts/bench.sh                                    # Defaults
#   ./scripts/bench.sh --model meta-llama/Llama-3.1-8B-Instruct
#   ./scripts/bench.sh --gpus 4 --duration 10m --users 20
#   ./scripts/bench.sh --compare                          # A/B test
#
# From a fresh Lambda instance:
#   curl -fsSL https://raw.githubusercontent.com/Ranvier-Systems/ranvier-core/main/scripts/bench.sh | bash
#
# =============================================================================

set -e

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------

# Defaults
DEFAULT_MODEL="meta-llama/Llama-3.2-1B-Instruct"
DEFAULT_DURATION="5m"
DEFAULT_USERS="10"
DEFAULT_SPAWN_RATE="2"
DEFAULT_VLLM_PORT_START=8000
DEFAULT_PROMPT_DIST="long"
DEFAULT_PREFIX_RATIO="0.9"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# State
VLLM_PIDS=()
CLEANUP_DONE=false

# -----------------------------------------------------------------------------
# Logging
# -----------------------------------------------------------------------------

log_header() { echo -e "\n${BOLD}${CYAN}$1${NC}"; echo -e "${CYAN}$(printf '─%.0s' {1..50})${NC}"; }
log_info()   { echo -e "${BLUE}▸${NC} $1"; }
log_ok()     { echo -e "${GREEN}✓${NC} $1"; }
log_warn()   { echo -e "${YELLOW}⚠${NC} $1"; }
log_error()  { echo -e "${RED}✗${NC} $1"; }
log_step()   { echo -e "  ${BLUE}[$1/$2]${NC} $3"; }

# -----------------------------------------------------------------------------
# Cleanup handler
# -----------------------------------------------------------------------------

cleanup() {
    if [ "$CLEANUP_DONE" = true ]; then
        return
    fi
    CLEANUP_DONE=true

    echo ""
    log_header "Cleaning up"

    # Kill vLLM processes
    if [ ${#VLLM_PIDS[@]} -gt 0 ]; then
        log_info "Stopping vLLM processes..."
        for pid in "${VLLM_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null || true
            fi
        done
        sleep 2
        # Force kill if still running
        for pid in "${VLLM_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        done
        log_ok "vLLM processes stopped"
    fi

    # Stop Docker containers
    if [ -f docker-compose.benchmark-real.yml ]; then
        log_info "Stopping Ranvier containers..."
        docker compose -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real \
            --profile benchmark down -v --remove-orphans 2>/dev/null || true
        log_ok "Ranvier containers stopped"
    fi
}

trap cleanup EXIT INT TERM

# -----------------------------------------------------------------------------
# Help
# -----------------------------------------------------------------------------

print_help() {
    cat << 'EOF'
Ranvier Benchmark - Lambda Labs Edition

USAGE:
    ./scripts/bench.sh [OPTIONS]

REQUIRED:
    HF_TOKEN        Hugging Face token (env var) for gated models

OPTIONS:
    --model MODEL       Model to benchmark (default: meta-llama/Llama-3.2-1B-Instruct)
    --gpus N            Number of GPUs to use (default: auto-detect)
    --duration TIME     Benchmark duration (default: 5m)
    --users N           Concurrent users (default: 10)
    --spawn-rate N      Users spawned per second (default: 2)
    --prompt-dist DIST  Prompt distribution: short|medium|long|mixed (default: long)
    --prefix-ratio R    Shared prefix ratio 0.0-1.0 (default: 0.9)
    --compare           Run A/B comparison (prefix vs round-robin)
    --skip-setup        Skip system configuration (for repeated runs)
    --skip-vllm         Don't start vLLM (use existing endpoints)
    --vllm-host HOST    vLLM host IP (default: localhost, for --skip-vllm)
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Simple run with defaults
    export HF_TOKEN=hf_xxx
    ./scripts/bench.sh

    # Larger model, longer duration
    ./scripts/bench.sh --model meta-llama/Llama-3.1-8B-Instruct --duration 10m

    # Use only 4 of 8 GPUs
    ./scripts/bench.sh --gpus 4

    # A/B comparison test
    ./scripts/bench.sh --compare

    # Use existing vLLM endpoints (manual setup)
    ./scripts/bench.sh --skip-vllm --vllm-host 10.0.0.1 --gpus 2

EOF
}

# -----------------------------------------------------------------------------
# Argument parsing
# -----------------------------------------------------------------------------

MODEL="$DEFAULT_MODEL"
GPUS=""
DURATION="$DEFAULT_DURATION"
USERS="$DEFAULT_USERS"
SPAWN_RATE="$DEFAULT_SPAWN_RATE"
PROMPT_DIST="$DEFAULT_PROMPT_DIST"
PREFIX_RATIO="$DEFAULT_PREFIX_RATIO"
COMPARE=false
SKIP_SETUP=false
SKIP_VLLM=false
VLLM_HOST="localhost"
DRY_RUN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --model)        MODEL="$2"; shift 2 ;;
        --gpus)         GPUS="$2"; shift 2 ;;
        --duration)     DURATION="$2"; shift 2 ;;
        --users)        USERS="$2"; shift 2 ;;
        --spawn-rate)   SPAWN_RATE="$2"; shift 2 ;;
        --prompt-dist)  PROMPT_DIST="$2"; shift 2 ;;
        --prefix-ratio) PREFIX_RATIO="$2"; shift 2 ;;
        --compare)      COMPARE=true; shift ;;
        --skip-setup)   SKIP_SETUP=true; shift ;;
        --skip-vllm)    SKIP_VLLM=true; shift ;;
        --vllm-host)    VLLM_HOST="$2"; shift 2 ;;
        --dry-run)      DRY_RUN=true; shift ;;
        -h|--help)      print_help; exit 0 ;;
        *)              log_error "Unknown option: $1"; print_help; exit 1 ;;
    esac
done

# -----------------------------------------------------------------------------
# Preflight checks
# -----------------------------------------------------------------------------

log_header "Ranvier Benchmark (Lambda Labs Edition)"

# Check if running from repo root or need to clone
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/../docker-compose.benchmark-real.yml" ]]; then
    REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
    cd "$REPO_DIR"
elif [[ -f "./docker-compose.benchmark-real.yml" ]]; then
    REPO_DIR="$(pwd)"
else
    # Running from curl pipe - need to clone repo
    log_info "Cloning Ranvier repository..."
    REPO_DIR="${HOME}/ranvier-core"
    if [[ -d "$REPO_DIR" ]]; then
        cd "$REPO_DIR"
        git pull origin main 2>/dev/null || true
    else
        git clone https://github.com/Ranvier-Systems/ranvier-core.git "$REPO_DIR"
        cd "$REPO_DIR"
    fi
    log_ok "Repository ready at $REPO_DIR"
fi

# Check HF_TOKEN
if [[ -z "${HF_TOKEN:-}" ]]; then
    log_warn "HF_TOKEN not set - required for gated models like Llama"
    log_info "Set with: export HF_TOKEN=your_token"
    if [[ "$MODEL" == *"llama"* ]] || [[ "$MODEL" == *"Llama"* ]]; then
        log_error "Llama models require HF_TOKEN. Exiting."
        exit 1
    fi
else
    log_ok "HF_TOKEN found"
fi

# Check Docker
if ! command -v docker &> /dev/null; then
    log_error "Docker not found. Please install Docker first."
    exit 1
fi
log_ok "Docker available"

# Check docker compose
if docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
else
    log_error "Docker Compose not found"
    exit 1
fi
log_ok "Docker Compose available"

# Detect GPUs
if [[ -z "$GPUS" ]]; then
    if command -v nvidia-smi &> /dev/null; then
        GPUS=$(nvidia-smi --list-gpus 2>/dev/null | wc -l)
        if [[ "$GPUS" -gt 0 ]]; then
            GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)
            log_ok "Detected $GPUS GPUs: $GPU_NAME"
        else
            log_warn "No GPUs detected, defaulting to 2"
            GPUS=2
        fi
    else
        log_warn "nvidia-smi not found, defaulting to 2 GPUs"
        GPUS=2
    fi
fi

# Cap at 8 GPUs for now (can be increased)
if [[ "$GPUS" -gt 8 ]]; then
    log_warn "Capping at 8 GPUs (detected $GPUS)"
    GPUS=8
fi

# -----------------------------------------------------------------------------
# System setup (one-time)
# -----------------------------------------------------------------------------

if [[ "$SKIP_SETUP" = false ]]; then
    log_header "System Configuration"

    # Increase AIO limit for Seastar
    if [[ -w /proc/sys/fs/aio-max-nr ]] || command -v sudo &> /dev/null; then
        sudo sysctl -w fs.aio-max-nr=1048576 2>/dev/null && log_ok "Set fs.aio-max-nr=1048576" || log_warn "Could not set aio-max-nr"
    fi

    # Create reports directory
    mkdir -p benchmark-reports
    log_ok "Created benchmark-reports directory"
fi

# -----------------------------------------------------------------------------
# Dry run output
# -----------------------------------------------------------------------------

if [[ "$DRY_RUN" = true ]]; then
    log_header "Dry Run - Configuration"
    echo "  Model:           $MODEL"
    echo "  GPUs:            $GPUS"
    echo "  Duration:        $DURATION"
    echo "  Users:           $USERS"
    echo "  Spawn Rate:      $SPAWN_RATE/s"
    echo "  Prompt Dist:     $PROMPT_DIST"
    echo "  Prefix Ratio:    $PREFIX_RATIO"
    echo "  Compare Mode:    $COMPARE"
    echo "  Skip vLLM:       $SKIP_VLLM"
    echo "  vLLM Host:       $VLLM_HOST"
    echo ""
    log_info "Would start $GPUS vLLM instances on ports $DEFAULT_VLLM_PORT_START-$((DEFAULT_VLLM_PORT_START + GPUS - 1))"
    log_info "Would start Ranvier cluster (3 nodes)"
    log_info "Would run Locust benchmark"
    exit 0
fi

# -----------------------------------------------------------------------------
# Start vLLM backends
# -----------------------------------------------------------------------------

if [[ "$SKIP_VLLM" = false ]]; then
    log_header "Starting vLLM Backends"

    # Check if vLLM is available
    if ! python3 -c "import vllm" 2>/dev/null; then
        log_error "vLLM not installed. Install with: pip install vllm"
        exit 1
    fi

    for ((i=0; i<GPUS; i++)); do
        PORT=$((DEFAULT_VLLM_PORT_START + i))
        LOG_FILE="/tmp/vllm_gpu${i}.log"

        log_step "$((i+1))" "$GPUS" "GPU $i: Starting vLLM on :$PORT..."

        CUDA_VISIBLE_DEVICES=$i HF_TOKEN="$HF_TOKEN" \
            python3 -m vllm.entrypoints.openai.api_server \
            --model "$MODEL" \
            --host 0.0.0.0 \
            --port "$PORT" \
            --enable-prefix-caching \
            --gpu-memory-utilization 0.85 \
            > "$LOG_FILE" 2>&1 &

        VLLM_PIDS+=($!)
    done

    # Wait for all vLLM instances to be healthy
    log_info "Waiting for vLLM backends to be ready..."

    MAX_WAIT=300  # 5 minutes
    WAIT_INTERVAL=5
    ELAPSED=0

    while [[ $ELAPSED -lt $MAX_WAIT ]]; do
        ALL_HEALTHY=true

        for ((i=0; i<GPUS; i++)); do
            PORT=$((DEFAULT_VLLM_PORT_START + i))
            if ! curl -sf --connect-timeout 2 "http://localhost:$PORT/health" > /dev/null 2>&1; then
                ALL_HEALTHY=false
                break
            fi
        done

        if [[ "$ALL_HEALTHY" = true ]]; then
            break
        fi

        # Check if any vLLM process died
        for ((i=0; i<${#VLLM_PIDS[@]}; i++)); do
            if ! kill -0 "${VLLM_PIDS[$i]}" 2>/dev/null; then
                log_error "vLLM process for GPU $i died. Check /tmp/vllm_gpu${i}.log"
                cat "/tmp/vllm_gpu${i}.log" | tail -20
                exit 1
            fi
        done

        sleep $WAIT_INTERVAL
        ELAPSED=$((ELAPSED + WAIT_INTERVAL))
        echo -ne "\r  Waiting... ${ELAPSED}s / ${MAX_WAIT}s"
    done

    echo ""

    if [[ "$ALL_HEALTHY" = true ]]; then
        for ((i=0; i<GPUS; i++)); do
            PORT=$((DEFAULT_VLLM_PORT_START + i))
            log_ok "GPU $i: vLLM healthy on :$PORT"
        done
    else
        log_error "vLLM backends did not become healthy within ${MAX_WAIT}s"
        exit 1
    fi
else
    log_header "Using External vLLM"
    log_info "Host: $VLLM_HOST"
    log_info "Ports: $DEFAULT_VLLM_PORT_START - $((DEFAULT_VLLM_PORT_START + GPUS - 1))"

    # Health check external endpoints
    for ((i=0; i<GPUS; i++)); do
        PORT=$((DEFAULT_VLLM_PORT_START + i))
        if curl -sf --connect-timeout 5 "http://${VLLM_HOST}:$PORT/health" > /dev/null 2>&1; then
            log_ok "GPU $i: $VLLM_HOST:$PORT healthy"
        else
            log_error "GPU $i: $VLLM_HOST:$PORT unreachable"
            exit 1
        fi
    done
fi

# -----------------------------------------------------------------------------
# Build backend environment variables
# -----------------------------------------------------------------------------

# Build BACKEND{N}_IP and BACKEND{N}_PORT env vars
BACKEND_ENV=""
for ((i=1; i<=GPUS; i++)); do
    PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
    if [[ "$SKIP_VLLM" = true ]]; then
        HOST="$VLLM_HOST"
    else
        HOST="host.docker.internal"
    fi
    BACKEND_ENV+="BACKEND${i}_IP=$HOST BACKEND${i}_PORT=$PORT "
done

# -----------------------------------------------------------------------------
# Start Ranvier cluster
# -----------------------------------------------------------------------------

log_header "Starting Ranvier Cluster"

# Build Ranvier image if needed
if ! docker image inspect ranvier:latest &> /dev/null; then
    log_info "Building Ranvier image (first run)..."
    docker build -t ranvier:latest -f Dockerfile.production . > /dev/null 2>&1
    log_ok "Ranvier image built"
fi

# Build locust image if needed
if ! docker image inspect ranvier-locust:latest &> /dev/null; then
    log_info "Building Locust image..."
    docker build -t ranvier-locust:latest -f tests/integration/Dockerfile.locust tests/integration/ > /dev/null 2>&1
    log_ok "Locust image built"
fi

# Start Ranvier nodes
log_info "Starting Ranvier nodes..."
$DOCKER_COMPOSE -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real up -d ranvier1 ranvier2 ranvier3 2>/dev/null

# Wait for Ranvier to be healthy
MAX_WAIT=60
ELAPSED=0
while [[ $ELAPSED -lt $MAX_WAIT ]]; do
    HEALTHY=0
    for node in ranvier-bench1 ranvier-bench2 ranvier-bench3; do
        if docker exec "$node" curl -sf http://localhost:9180/metrics > /dev/null 2>&1; then
            HEALTHY=$((HEALTHY + 1))
        fi
    done

    if [[ $HEALTHY -eq 3 ]]; then
        break
    fi

    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

if [[ $HEALTHY -eq 3 ]]; then
    log_ok "ranvier-bench1 (172.29.2.1:8080)"
    log_ok "ranvier-bench2 (172.29.2.2:8080)"
    log_ok "ranvier-bench3 (172.29.2.3:8080)"
else
    log_error "Ranvier cluster did not become healthy"
    exit 1
fi

# -----------------------------------------------------------------------------
# Run benchmark function
# -----------------------------------------------------------------------------

run_benchmark() {
    local ROUTING_MODE="$1"
    local LABEL="$2"

    log_header "Running Benchmark: $LABEL"
    echo "  Model:        $MODEL"
    echo "  Backends:     $GPUS"
    echo "  Duration:     $DURATION"
    echo "  Users:        $USERS"
    echo "  Routing:      $ROUTING_MODE"
    echo "  Prompt Dist:  $PROMPT_DIST"
    echo ""

    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    REPORT_DIR="benchmark-reports/${TIMESTAMP}_${GPUS}gpu_${ROUTING_MODE}"
    mkdir -p "$REPORT_DIR"

    # Set environment for locust
    export NUM_BACKENDS="$GPUS"
    export VLLM_MODEL="$MODEL"
    export RANVIER_ROUTING_MODE="$ROUTING_MODE"
    export PROMPT_DISTRIBUTION="$PROMPT_DIST"
    export SHARED_PREFIX_RATIO="$PREFIX_RATIO"

    # Export backend configuration
    for ((i=1; i<=GPUS; i++)); do
        PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        if [[ "$SKIP_VLLM" = true ]]; then
            HOST="$VLLM_HOST"
        else
            HOST="host.docker.internal"
        fi
        export "BACKEND${i}_IP=$HOST"
        export "BACKEND${i}_PORT=$PORT"
    done

    # Run locust via docker compose
    $DOCKER_COMPOSE -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real \
        --profile benchmark run --rm \
        -e NUM_BACKENDS="$GPUS" \
        -e VLLM_MODEL="$MODEL" \
        -e RANVIER_ROUTING_MODE="$ROUTING_MODE" \
        -e PROMPT_DISTRIBUTION="$PROMPT_DIST" \
        -e SHARED_PREFIX_RATIO="$PREFIX_RATIO" \
        $(for ((i=1; i<=GPUS; i++)); do
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
            if [[ "$SKIP_VLLM" = true ]]; then
                HOST="$VLLM_HOST"
            else
                HOST="host.docker.internal"
            fi
            echo "-e BACKEND${i}_IP=$HOST -e BACKEND${i}_PORT=$PORT"
        done) \
        locust \
        --headless \
        --users "$USERS" \
        --spawn-rate "$SPAWN_RATE" \
        --run-time "$DURATION" \
        --csv "/mnt/locust/results" \
        --html "/mnt/locust/report.html" \
        2>&1 | tee "$REPORT_DIR/benchmark.log"

    # Copy results
    docker cp locust-real:/mnt/locust/results_stats.csv "$REPORT_DIR/" 2>/dev/null || true
    docker cp locust-real:/mnt/locust/report.html "$REPORT_DIR/" 2>/dev/null || true

    log_ok "Results saved to: $REPORT_DIR/"

    echo "$REPORT_DIR"
}

# -----------------------------------------------------------------------------
# Execute benchmarks
# -----------------------------------------------------------------------------

if [[ "$COMPARE" = true ]]; then
    log_header "A/B Comparison Mode"
    log_info "Running two benchmarks: Round-Robin (baseline) vs Prefix-Aware (optimized)"

    REPORT_RR=$(run_benchmark "round_robin" "Round-Robin (Baseline)")

    # Brief pause between tests
    log_info "Pausing 30s between tests to clear caches..."
    sleep 30

    REPORT_PREFIX=$(run_benchmark "prefix" "Prefix-Aware (Optimized)")

    log_header "A/B Comparison Complete"
    echo ""
    echo "Results:"
    echo "  Round-Robin:  $REPORT_RR"
    echo "  Prefix-Aware: $REPORT_PREFIX"
    echo ""
    log_info "Compare TTFT improvements in the benchmark logs"
else
    run_benchmark "prefix" "Prefix-Aware Routing"
fi

log_header "Benchmark Complete"
log_ok "All results saved to benchmark-reports/"
