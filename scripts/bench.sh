#!/bin/bash
# =============================================================================
# Ranvier Benchmark - Consolidated Edition
# =============================================================================
#
# One-command benchmarking for Lambda Labs multi-GPU instances.
# Auto-detects GPUs, starts vLLM, runs Ranvier benchmark, cleans up.
#
# This script consolidates functionality from:
#   - setup-lambda-benchmark.sh (use --setup)
#   - run-multi-gpu-benchmark.sh (use --skip-vllm --vllm-endpoints)
#
# Usage:
#   export HF_TOKEN=your_huggingface_token
#   ./scripts/bench.sh                                    # Defaults
#   ./scripts/bench.sh --model meta-llama/Llama-3.1-8B-Instruct
#   ./scripts/bench.sh --gpus 4 --duration 10m --users 20
#   ./scripts/bench.sh --compare                          # A/B test
#
# First-time setup on fresh instance:
#   ./scripts/bench.sh --setup
#
# Using external vLLM endpoints:
#   ./scripts/bench.sh --skip-vllm --vllm-endpoints 10.0.0.1:8000,10.0.0.2:8000
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
DEFAULT_PROMPT_DIST="stress"
DEFAULT_PREFIX_RATIO="0.9"
DEFAULT_OUTPUT_DIR="benchmark-reports"
DEFAULT_WARMUP_DURATION="1m"
DEFAULT_WARMUP_USERS="2"

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
VLLM_ENDPOINTS=()  # Array of host:port pairs for external vLLM

# Early exit for --help (before trap is set)
for arg in "$@"; do
    if [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
        # Help is printed later after print_help is defined
        SHOW_HELP=true
        break
    fi
done

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

# -----------------------------------------------------------------------------
# Help (check early, before trap is set)
# -----------------------------------------------------------------------------

print_help() {
    cat << 'EOF'
Ranvier Benchmark - Consolidated Edition

USAGE:
    ./scripts/bench.sh [OPTIONS]

REQUIRED:
    HF_TOKEN        Hugging Face token (env var) for gated models

SETUP OPTIONS:
    --setup             One-time system setup (Docker, limits, dependencies)
    --install-deps      Install vLLM and dependencies before running

BENCHMARK OPTIONS:
    --model MODEL       Model to benchmark (default: meta-llama/Llama-3.2-1B-Instruct)
    --gpus N            Number of GPUs to use (default: auto-detect)
    --duration TIME     Benchmark duration (default: 5m)
    --users N           Concurrent users (default: 10)
    --spawn-rate N      Users spawned per second (default: 2)
    --prompt-dist DIST  Prompt distribution: short|medium|long|mixed|stress (default: stress)
    --prefix-ratio R    Shared prefix ratio 0.0-1.0 (default: 0.9)
    --compare           Run A/B comparison (prefix vs round-robin)
    --warmup            Run a short warm-up before the main benchmark
    --output-dir DIR    Custom output directory (default: benchmark-reports)

EXTERNAL VLLM OPTIONS:
    --skip-vllm             Don't start vLLM (use existing endpoints)
    --vllm-host HOST        vLLM host IP (default: localhost, assumes sequential ports)
    --vllm-endpoints LIST   Comma-separated host:port pairs (alternative to --vllm-host)
                            Example: --vllm-endpoints 10.0.0.1:8000,10.0.0.2:8000

OTHER OPTIONS:
    --skip-setup        Skip system configuration (for repeated runs)
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # First-time setup on fresh Lambda instance
    ./scripts/bench.sh --setup

    # Simple run with defaults (auto-detects GPUs, starts vLLM)
    export HF_TOKEN=hf_xxx
    ./scripts/bench.sh

    # 8x A100 with larger model
    ./scripts/bench.sh --model meta-llama/Llama-3.1-8B-Instruct --duration 10m --users 30

    # Use only 4 of 8 GPUs
    ./scripts/bench.sh --gpus 4

    # A/B comparison test (prefix vs round-robin)
    ./scripts/bench.sh --compare --duration 10m

    # With warm-up run before main benchmark
    ./scripts/bench.sh --warmup --duration 10m

    # Use external vLLM endpoints (explicit list)
    ./scripts/bench.sh --skip-vllm --vllm-endpoints 10.0.0.1:8000,10.0.0.2:8000,10.0.0.3:8000

    # Use external vLLM on single host with sequential ports
    ./scripts/bench.sh --skip-vllm --vllm-host 10.0.0.1 --gpus 8

EOF
}

# Handle early --help (before trap is set)
if [[ "${SHOW_HELP:-}" == "true" ]]; then
    print_help
    exit 0
fi

# Now set the trap (after help check, so cleanup doesn't run on --help)
trap cleanup EXIT INT TERM

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
OUTPUT_DIR="$DEFAULT_OUTPUT_DIR"
COMPARE=false
SKIP_SETUP=false
SKIP_VLLM=false
VLLM_HOST="localhost"
VLLM_ENDPOINTS_RAW=""
INSTALL_DEPS=false
DRY_RUN=false
SETUP_ONLY=false
WARMUP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --model)          MODEL="$2"; shift 2 ;;
        --gpus)           GPUS="$2"; shift 2 ;;
        --duration)       DURATION="$2"; shift 2 ;;
        --users)          USERS="$2"; shift 2 ;;
        --spawn-rate)     SPAWN_RATE="$2"; shift 2 ;;
        --prompt-dist)    PROMPT_DIST="$2"; shift 2 ;;
        --prefix-ratio)   PREFIX_RATIO="$2"; shift 2 ;;
        --output-dir)     OUTPUT_DIR="$2"; shift 2 ;;
        --compare)        COMPARE=true; shift ;;
        --skip-setup)     SKIP_SETUP=true; shift ;;
        --skip-vllm)      SKIP_VLLM=true; shift ;;
        --vllm-host)      VLLM_HOST="$2"; shift 2 ;;
        --vllm-endpoints) VLLM_ENDPOINTS_RAW="$2"; shift 2 ;;
        --install-deps)   INSTALL_DEPS=true; shift ;;
        --dry-run)        DRY_RUN=true; shift ;;
        --setup)          SETUP_ONLY=true; shift ;;
        --warmup)         WARMUP=true; shift ;;
        -h|--help)        print_help; exit 0 ;;
        *)                log_error "Unknown option: $1"; print_help; exit 1 ;;
    esac
done

# Parse --vllm-endpoints into array if provided
if [[ -n "$VLLM_ENDPOINTS_RAW" ]]; then
    IFS=',' read -ra VLLM_ENDPOINTS <<< "$VLLM_ENDPOINTS_RAW"
    SKIP_VLLM=true  # Implicit --skip-vllm when endpoints are provided
fi

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

# Check HF_TOKEN (not required for --setup mode)
if [[ "$SETUP_ONLY" != true ]]; then
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
fi

# Check Docker
if ! command -v docker &> /dev/null; then
    log_error "Docker not found. Please install Docker first."
    exit 1
fi

# Check if user can run docker
if ! docker ps &> /dev/null; then
    log_error "Cannot run docker commands. You may need to add yourself to the docker group:"
    log_info "  sudo usermod -aG docker \$USER"
    log_info "  newgrp docker  # or log out and back in"
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
# Setup-only mode (--setup flag)
# -----------------------------------------------------------------------------

if [[ "$SETUP_ONLY" = true ]]; then
    log_header "One-Time System Setup"

    # Check/install Docker
    if ! command -v docker &> /dev/null; then
        log_info "Docker not found. Installing..."
        if command -v curl &> /dev/null; then
            curl -fsSL https://get.docker.com | sh
            sudo usermod -aG docker "$USER" 2>/dev/null || true
            log_ok "Docker installed"
            log_warn "Run 'newgrp docker' or log out/in for Docker group membership"
        else
            log_error "curl not available. Please install Docker manually."
            exit 1
        fi
    else
        log_ok "Docker already installed"
    fi

    # Ensure user is in docker group (even if Docker was pre-installed)
    if ! docker ps &> /dev/null 2>&1; then
        log_info "Adding user to docker group..."
        sudo usermod -aG docker "$USER" 2>/dev/null || true
        log_warn "Run 'newgrp docker' or log out/in to apply docker group membership"
        log_info "Then re-run: ./scripts/bench.sh --setup"
    fi

    # Check docker compose
    if docker compose version &> /dev/null; then
        log_ok "Docker Compose available"
    else
        log_warn "Docker Compose not available - may need to install docker-compose-plugin"
    fi

    # System limits for Seastar
    log_info "Configuring system limits..."
    if [[ -w /proc/sys/fs/aio-max-nr ]] || command -v sudo &> /dev/null; then
        sudo sysctl -w fs.aio-max-nr=1048576 2>/dev/null && log_ok "Set fs.aio-max-nr=1048576" || log_warn "Could not set aio-max-nr"
    fi

    # Persist the setting
    if [[ -w /etc/sysctl.conf ]] || command -v sudo &> /dev/null; then
        if ! grep -q "fs.aio-max-nr" /etc/sysctl.conf 2>/dev/null; then
            echo "fs.aio-max-nr=1048576" | sudo tee -a /etc/sysctl.conf > /dev/null 2>&1 && \
                log_ok "Persisted aio-max-nr setting" || log_warn "Could not persist setting"
        fi
    fi

    # Create directories
    mkdir -p "$OUTPUT_DIR"
    log_ok "Created $OUTPUT_DIR directory"

    # Pre-build Docker images
    if [[ -f "Dockerfile.production" ]]; then
        log_info "Pre-building Ranvier Docker image..."
        docker build -t ranvier:latest -f Dockerfile.production . > /dev/null 2>&1 && \
            log_ok "Ranvier image built" || log_warn "Could not build Ranvier image"
    fi

    if [[ -f "tests/integration/Dockerfile.locust" ]]; then
        log_info "Pre-building Locust Docker image..."
        docker build -t ranvier-locust:latest -f tests/integration/Dockerfile.locust tests/integration/ > /dev/null 2>&1 && \
            log_ok "Locust image built" || log_warn "Could not build Locust image"
    fi

    # Summary
    log_header "Setup Complete"
    echo ""
    echo "Next steps:"
    echo ""
    echo "  1. Set HuggingFace token (for gated models):"
    echo "     export HF_TOKEN=hf_your_token_here"
    echo ""
    echo "  2. Run benchmark:"
    echo "     ./scripts/bench.sh --model meta-llama/Llama-3.1-8B-Instruct --duration 10m"
    echo ""
    echo "  3. Or run with warm-up:"
    echo "     ./scripts/bench.sh --warmup --duration 10m --users 30"
    echo ""
    echo "  4. For A/B comparison (prefix vs round-robin):"
    echo "     ./scripts/bench.sh --compare --duration 10m"
    echo ""
    exit 0
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
    mkdir -p "$OUTPUT_DIR"
    log_ok "Created $OUTPUT_DIR directory"
fi

# -----------------------------------------------------------------------------
# Install dependencies (optional)
# -----------------------------------------------------------------------------

if [[ "$INSTALL_DEPS" = true ]]; then
    log_header "Installing Dependencies"

    if command -v pip3 &> /dev/null; then
        PIP="pip3"
    elif command -v pip &> /dev/null; then
        PIP="pip"
    else
        log_error "pip not found. Please install Python pip first."
        exit 1
    fi

    log_info "Installing vLLM (this may take a few minutes)..."
    $PIP install vllm 2>&1 | tail -5
    log_ok "vLLM installed"

    # numpy<2 compatibility fix for vLLM
    log_info "Installing numpy<2 (vLLM compatibility fix)..."
    $PIP install "numpy<2" 2>&1 | tail -2
    log_ok "numpy<2 installed"
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
    echo "  Output Dir:      $OUTPUT_DIR"
    echo "  Compare Mode:    $COMPARE"
    echo "  Warmup:          $WARMUP"
    echo "  Skip vLLM:       $SKIP_VLLM"
    if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
        echo "  vLLM Endpoints:  ${VLLM_ENDPOINTS[*]}"
    else
        echo "  vLLM Host:       $VLLM_HOST"
    fi
    echo ""
    if [[ "$SKIP_VLLM" = false ]]; then
        log_info "Would start $GPUS vLLM instances on ports $DEFAULT_VLLM_PORT_START-$((DEFAULT_VLLM_PORT_START + GPUS - 1))"
    else
        if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
            log_info "Would use ${#VLLM_ENDPOINTS[@]} external vLLM endpoints"
        else
            log_info "Would use $GPUS external vLLM endpoints at $VLLM_HOST:$DEFAULT_VLLM_PORT_START-$((DEFAULT_VLLM_PORT_START + GPUS - 1))"
        fi
    fi
    log_info "Would start Ranvier cluster (3 nodes)"
    if [[ "$WARMUP" = true ]]; then
        log_info "Would run warm-up ($DEFAULT_WARMUP_DURATION, $DEFAULT_WARMUP_USERS users)"
    fi
    log_info "Would run Locust benchmark ($DURATION, $USERS users)"
    exit 0
fi

# -----------------------------------------------------------------------------
# Start vLLM backends
# -----------------------------------------------------------------------------

if [[ "$SKIP_VLLM" = false ]]; then
    log_header "Starting vLLM Backends"

    # Check if vLLM is available
    if ! python3 -c "import vllm" 2>/dev/null; then
        log_error "vLLM not installed."
        log_info "Install manually:"
        log_info "  pip install vllm"
        log_info "  pip install 'numpy<2'  # compatibility fix"
        log_info ""
        log_info "Or re-run with --install-deps:"
        log_info "  $0 --install-deps"
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

    # Handle explicit endpoints vs host+port pattern
    if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
        log_info "Using ${#VLLM_ENDPOINTS[@]} explicit endpoints"
        GPUS=${#VLLM_ENDPOINTS[@]}

        # Health check each endpoint
        FAILED=0
        for ((i=0; i<${#VLLM_ENDPOINTS[@]}; i++)); do
            ENDPOINT="${VLLM_ENDPOINTS[$i]}"
            HOST="${ENDPOINT%:*}"
            PORT="${ENDPOINT#*:}"
            if curl -sf --connect-timeout 5 "http://${HOST}:${PORT}/health" > /dev/null 2>&1; then
                log_ok "Backend $((i+1)): $HOST:$PORT healthy"
            else
                log_warn "Backend $((i+1)): $HOST:$PORT unreachable"
                FAILED=$((FAILED + 1))
            fi
        done

        if [[ $FAILED -gt 0 ]]; then
            log_warn "$FAILED endpoint(s) unreachable from this host"
            log_info "Docker containers may still be able to reach them"
            read -p "Continue anyway? [y/N] " -n 1 -r
            echo
            if [[ ! $REPLY =~ ^[Yy]$ ]]; then
                log_info "Aborted."
                exit 0
            fi
        fi
    else
        log_info "Host: $VLLM_HOST"
        log_info "Ports: $DEFAULT_VLLM_PORT_START - $((DEFAULT_VLLM_PORT_START + GPUS - 1))"

        # Health check external endpoints (sequential ports)
        FAILED=0
        for ((i=0; i<GPUS; i++)); do
            PORT=$((DEFAULT_VLLM_PORT_START + i))
            if curl -sf --connect-timeout 5 "http://${VLLM_HOST}:$PORT/health" > /dev/null 2>&1; then
                log_ok "GPU $i: $VLLM_HOST:$PORT healthy"
            else
                log_warn "GPU $i: $VLLM_HOST:$PORT unreachable"
                FAILED=$((FAILED + 1))
            fi
        done

        if [[ $FAILED -gt 0 ]]; then
            log_warn "$FAILED endpoint(s) unreachable from this host"
            log_info "Docker containers may still be able to reach them"
            read -p "Continue anyway? [y/N] " -n 1 -r
            echo
            if [[ ! $REPLY =~ ^[Yy]$ ]]; then
                log_info "Aborted."
                exit 0
            fi
        fi
    fi
fi

# -----------------------------------------------------------------------------
# Build backend environment variables
# -----------------------------------------------------------------------------

# Build BACKEND{N}_IP and BACKEND{N}_PORT env vars
BACKEND_ENV=""
for ((i=1; i<=GPUS; i++)); do
    if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
        # Use explicit endpoints
        ENDPOINT="${VLLM_ENDPOINTS[$((i-1))]}"
        HOST="${ENDPOINT%:*}"
        PORT="${ENDPOINT#*:}"
    elif [[ "$SKIP_VLLM" = true ]]; then
        # Use host + sequential ports
        HOST="$VLLM_HOST"
        PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
    else
        # Local vLLM via Docker network
        HOST="host.docker.internal"
        PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
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
    REPORT_DIR="${OUTPUT_DIR}/${TIMESTAMP}_${GPUS}gpu_${ROUTING_MODE}"
    mkdir -p "$REPORT_DIR"

    # Set environment for locust
    export NUM_BACKENDS="$GPUS"
    export VLLM_MODEL="$MODEL"
    export RANVIER_ROUTING_MODE="$ROUTING_MODE"
    export PROMPT_DISTRIBUTION="$PROMPT_DIST"
    export SHARED_PREFIX_RATIO="$PREFIX_RATIO"

    # Export backend configuration
    for ((i=1; i<=GPUS; i++)); do
        if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
            # Use explicit endpoints
            ENDPOINT="${VLLM_ENDPOINTS[$((i-1))]}"
            HOST="${ENDPOINT%:*}"
            PORT="${ENDPOINT#*:}"
        elif [[ "$SKIP_VLLM" = true ]]; then
            # Use host + sequential ports
            HOST="$VLLM_HOST"
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        else
            # Local vLLM via Docker network
            HOST="host.docker.internal"
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        fi
        export "BACKEND${i}_IP=$HOST"
        export "BACKEND${i}_PORT=$PORT"
    done

    # Build backend env args for docker compose
    BACKEND_ARGS=""
    for ((i=1; i<=GPUS; i++)); do
        if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
            ENDPOINT="${VLLM_ENDPOINTS[$((i-1))]}"
            HOST="${ENDPOINT%:*}"
            PORT="${ENDPOINT#*:}"
        elif [[ "$SKIP_VLLM" = true ]]; then
            HOST="$VLLM_HOST"
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        else
            HOST="host.docker.internal"
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        fi
        BACKEND_ARGS+=" -e BACKEND${i}_IP=$HOST -e BACKEND${i}_PORT=$PORT"
    done

    # Run locust via docker compose
    $DOCKER_COMPOSE -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real \
        --profile benchmark run --rm \
        -e NUM_BACKENDS="$GPUS" \
        -e VLLM_MODEL="$MODEL" \
        -e RANVIER_ROUTING_MODE="$ROUTING_MODE" \
        -e PROMPT_DISTRIBUTION="$PROMPT_DIST" \
        -e SHARED_PREFIX_RATIO="$PREFIX_RATIO" \
        $BACKEND_ARGS \
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
# Warm-up run (optional)
# -----------------------------------------------------------------------------

if [[ "$WARMUP" = true ]]; then
    log_header "Warm-up Run"
    log_info "Running short warm-up to prime KV caches and model weights..."
    log_info "Duration: $DEFAULT_WARMUP_DURATION, Users: $DEFAULT_WARMUP_USERS"
    echo ""

    # Save original values
    ORIG_DURATION="$DURATION"
    ORIG_USERS="$USERS"

    # Run warm-up with reduced params
    DURATION="$DEFAULT_WARMUP_DURATION"
    USERS="$DEFAULT_WARMUP_USERS"

    # Create warmup directory
    WARMUP_DIR="${OUTPUT_DIR}/warmup_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$WARMUP_DIR"

    # Build backend env args
    BACKEND_ARGS=""
    for ((i=1; i<=GPUS; i++)); do
        if [[ ${#VLLM_ENDPOINTS[@]} -gt 0 ]]; then
            ENDPOINT="${VLLM_ENDPOINTS[$((i-1))]}"
            HOST="${ENDPOINT%:*}"
            PORT="${ENDPOINT#*:}"
        elif [[ "$SKIP_VLLM" = true ]]; then
            HOST="$VLLM_HOST"
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        else
            HOST="host.docker.internal"
            PORT=$((DEFAULT_VLLM_PORT_START + i - 1))
        fi
        BACKEND_ARGS+=" -e BACKEND${i}_IP=$HOST -e BACKEND${i}_PORT=$PORT"
    done

    # Run warm-up benchmark
    $DOCKER_COMPOSE -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real \
        --profile benchmark run --rm \
        -e NUM_BACKENDS="$GPUS" \
        -e VLLM_MODEL="$MODEL" \
        -e RANVIER_ROUTING_MODE="prefix" \
        -e PROMPT_DISTRIBUTION="$PROMPT_DIST" \
        -e SHARED_PREFIX_RATIO="$PREFIX_RATIO" \
        $BACKEND_ARGS \
        locust \
        --headless \
        --users "$USERS" \
        --spawn-rate "$SPAWN_RATE" \
        --run-time "$DURATION" \
        2>&1 | tee "$WARMUP_DIR/warmup.log"

    log_ok "Warm-up complete"
    log_info "Pausing 10s before main benchmark..."
    sleep 10

    # Restore original values
    DURATION="$ORIG_DURATION"
    USERS="$ORIG_USERS"
fi

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
log_ok "All results saved to $OUTPUT_DIR/"
echo ""

# Display quick summary if results exist
LATEST_REPORT=$(ls -td ${OUTPUT_DIR}/*/ 2>/dev/null | head -1)
if [[ -n "$LATEST_REPORT" && -f "${LATEST_REPORT}/benchmark.log" ]]; then
    log_info "Quick Summary (from ${LATEST_REPORT}):"
    echo ""
    # Extract key metrics from log if available
    if grep -q "TTFT" "${LATEST_REPORT}/benchmark.log" 2>/dev/null; then
        grep -E "(P50|P99|Cache hit|Requests/s|Error)" "${LATEST_REPORT}/benchmark.log" | head -10 || true
    fi
    echo ""
    log_info "Full results:"
    echo "  Log:    ${LATEST_REPORT}/benchmark.log"
    echo "  Stats:  ${LATEST_REPORT}/results_stats.csv"
    echo "  Report: ${LATEST_REPORT}/report.html"
fi

echo ""
log_info "To compare results, use:"
echo "  python3 tests/integration/compare_results.py <baseline.csv> <optimized.csv>"
