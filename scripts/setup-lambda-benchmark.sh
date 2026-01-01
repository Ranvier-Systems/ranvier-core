#!/bin/bash
# Lambda Labs Ranvier Benchmark Setup Script
#
# Run this script ON a Lambda Labs instance to set up Ranvier and run benchmarks
# against vLLM backends with minimal network latency.
#
# Usage:
#   # Copy this script to your Lambda instance and run:
#   curl -fsSL https://raw.githubusercontent.com/Ranvier-Systems/ranvier-core/main/scripts/setup-lambda-benchmark.sh | bash
#
#   # Or clone the repo and run:
#   ./scripts/setup-lambda-benchmark.sh
#
# Prerequisites:
#   - Lambda Labs instance (GPU or CPU)
#   - Docker installed (Lambda instances have this by default)
#   - vLLM running on one or more endpoints

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

echo ""
echo "========================================"
echo "Lambda Labs Ranvier Benchmark Setup"
echo "========================================"
echo ""

# Check if we're on a Lambda instance (or similar Linux environment)
if [[ ! -f /etc/os-release ]]; then
    log_error "This script is designed for Linux instances"
    exit 1
fi

# Check for Docker
if ! command -v docker &> /dev/null; then
    log_error "Docker not found. Installing..."
    curl -fsSL https://get.docker.com | sh
    sudo usermod -aG docker $USER
    log_warn "Docker installed. You may need to log out and back in, then re-run this script."
    exit 1
fi

log_success "Docker found"

# Check for docker compose
if docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
elif command -v docker-compose &> /dev/null; then
    DOCKER_COMPOSE="docker-compose"
else
    log_error "Docker Compose not found"
    exit 1
fi

log_success "Docker Compose found: $DOCKER_COMPOSE"

# Clone or update repo
REPO_DIR="${HOME}/ranvier-core"
if [[ -d "$REPO_DIR" ]]; then
    log_info "Updating existing repo..."
    cd "$REPO_DIR"
    git pull origin main || git pull origin master || true
else
    log_info "Cloning Ranvier Core..."
    git clone https://github.com/Ranvier-Systems/ranvier-core.git "$REPO_DIR"
    cd "$REPO_DIR"
fi

log_success "Repo ready at $REPO_DIR"

# Increase AIO limit for Seastar
log_info "Configuring system limits for Seastar..."
sudo sysctl -w fs.aio-max-nr=1048576 2>/dev/null || log_warn "Could not set aio-max-nr (may need root)"

# Create convenience wrapper script
WRAPPER_SCRIPT="${REPO_DIR}/run-benchmark.sh"
cat > "$WRAPPER_SCRIPT" << 'WRAPPER_EOF'
#!/bin/bash
# Ranvier Benchmark Runner (Lambda Labs Edition)
#
# Usage:
#   ./run-benchmark.sh <VLLM_IP_1> <VLLM_IP_2> [options]
#
# Examples:
#   # Both vLLM on same instance (localhost)
#   ./run-benchmark.sh localhost localhost --model meta-llama/Llama-3.1-8B-Instruct
#
#   # vLLM on separate instances
#   ./run-benchmark.sh 10.0.0.1 10.0.0.2 --model meta-llama/Llama-3.1-8B-Instruct
#
#   # With custom duration
#   ./run-benchmark.sh 10.0.0.1 10.0.0.2 --duration 10m --users 20

set -e

cd "$(dirname "$0")"

# Defaults
PORT="8000"
MODEL="${VLLM_MODEL:-meta-llama/Llama-3.2-1B-Instruct}"
DURATION="5m"
USERS="10"
SPAWN_RATE="2"
ROUTING_MODE="prefix"
PROMPT_DIST="long"
PREFIX_RATIO="0.9"

print_usage() {
    echo "Usage: $0 <VLLM_IP_1> <VLLM_IP_2> [options]"
    echo ""
    echo "Options:"
    echo "  --port PORT           vLLM port (default: 8000)"
    echo "  --model MODEL         Model name (default: $MODEL)"
    echo "  --duration TIME       Benchmark duration (default: 5m)"
    echo "  --users N             Concurrent users (default: 10)"
    echo "  --spawn-rate N        Users/second (default: 2)"
    echo "  --prompt-dist DIST    short|medium|long|mixed (default: long)"
    echo "  --prefix-ratio N      0.0-1.0 shared prefix ratio (default: 0.9)"
    echo "  --round-robin         Use round-robin routing (baseline comparison)"
    echo ""
}

if [[ $# -lt 2 ]]; then
    print_usage
    exit 1
fi

VLLM_IP_1="$1"
VLLM_IP_2="$2"
shift 2

while [[ $# -gt 0 ]]; do
    case $1 in
        --port) PORT="$2"; shift 2 ;;
        --model) MODEL="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --users) USERS="$2"; shift 2 ;;
        --spawn-rate) SPAWN_RATE="$2"; shift 2 ;;
        --prompt-dist) PROMPT_DIST="$2"; shift 2 ;;
        --prefix-ratio) PREFIX_RATIO="$2"; shift 2 ;;
        --round-robin) ROUTING_MODE="round_robin"; shift ;;
        -h|--help) print_usage; exit 0 ;;
        *) echo "Unknown option: $1"; print_usage; exit 1 ;;
    esac
done

echo ""
echo "========================================"
echo "Ranvier Benchmark (Lambda Edition)"
echo "========================================"
echo ""
echo "Configuration:"
echo "  vLLM Backend 1: ${VLLM_IP_1}:${PORT}"
echo "  vLLM Backend 2: ${VLLM_IP_2}:${PORT}"
echo "  Model: ${MODEL}"
echo "  Routing: ${ROUTING_MODE}"
echo "  Prompt Distribution: ${PROMPT_DIST}"
echo "  Shared Prefix Ratio: ${PREFIX_RATIO}"
echo "  Duration: ${DURATION}"
echo "  Users: ${USERS}"
echo ""

# Health check backends
echo "Checking vLLM endpoints..."
for ip in "$VLLM_IP_1" "$VLLM_IP_2"; do
    if curl -sf --connect-timeout 5 "http://${ip}:${PORT}/health" > /dev/null 2>&1; then
        echo "  ${ip}:${PORT} - OK"
    else
        echo "  ${ip}:${PORT} - UNREACHABLE"
        echo ""
        echo "ERROR: Cannot reach vLLM at ${ip}:${PORT}"
        echo "Make sure vLLM is running:"
        echo "  python -m vllm.entrypoints.openai.api_server --model $MODEL --host 0.0.0.0 --port $PORT --enable-prefix-caching"
        exit 1
    fi
done
echo ""

# Export environment
export VLLM_ENDPOINT_1="http://${VLLM_IP_1}:${PORT}"
export VLLM_ENDPOINT_2="http://${VLLM_IP_2}:${PORT}"
export BACKEND1_IP="${VLLM_IP_1}"
export BACKEND1_PORT="${PORT}"
export BACKEND2_IP="${VLLM_IP_2}"
export BACKEND2_PORT="${PORT}"
export VLLM_MODEL="${MODEL}"
export RANVIER_ROUTING_MODE="${ROUTING_MODE}"
export BENCHMARK_REAL_DURATION="${DURATION}"
export BENCHMARK_REAL_USERS="${USERS}"
export BENCHMARK_REAL_SPAWN_RATE="${SPAWN_RATE}"
export PROMPT_DISTRIBUTION="${PROMPT_DIST}"
export SHARED_PREFIX_RATIO="${PREFIX_RATIO}"

echo "Starting benchmark..."
make benchmark-real

echo ""
echo "Results saved to: benchmark-reports/"
WRAPPER_EOF

chmod +x "$WRAPPER_SCRIPT"
log_success "Created wrapper script: $WRAPPER_SCRIPT"

# Create A/B comparison script
AB_SCRIPT="${REPO_DIR}/run-ab-comparison.sh"
cat > "$AB_SCRIPT" << 'AB_EOF'
#!/bin/bash
# A/B Comparison: Prefix-Aware vs Round-Robin Routing
#
# Usage:
#   ./run-ab-comparison.sh <VLLM_IP_1> <VLLM_IP_2> [options]

set -e
cd "$(dirname "$0")"

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <VLLM_IP_1> <VLLM_IP_2> [--model MODEL] [--duration TIME]"
    exit 1
fi

VLLM_IP_1="$1"
VLLM_IP_2="$2"
MODEL="${3:-meta-llama/Llama-3.2-1B-Instruct}"
DURATION="${4:-5m}"

echo "========================================"
echo "A/B Comparison: Prefix vs Round-Robin"
echo "========================================"
echo ""

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo ">>> Running Round-Robin (baseline)..."
./run-benchmark.sh "$VLLM_IP_1" "$VLLM_IP_2" --model "$MODEL" --duration "$DURATION" --round-robin 2>&1 | tee "benchmark-reports/${TIMESTAMP}_round_robin.log"

echo ""
echo ">>> Running Prefix-Aware (optimized)..."
./run-benchmark.sh "$VLLM_IP_1" "$VLLM_IP_2" --model "$MODEL" --duration "$DURATION" 2>&1 | tee "benchmark-reports/${TIMESTAMP}_prefix.log"

echo ""
echo "========================================"
echo "A/B Comparison Complete"
echo "========================================"
echo ""
echo "Results:"
echo "  Round-Robin: benchmark-reports/${TIMESTAMP}_round_robin.log"
echo "  Prefix-Aware: benchmark-reports/${TIMESTAMP}_prefix.log"
echo ""
echo "Compare with:"
echo "  grep 'TTFT Improvement' benchmark-reports/${TIMESTAMP}_*.log"
AB_EOF

chmod +x "$AB_SCRIPT"
log_success "Created A/B script: $AB_SCRIPT"

# Summary
echo ""
echo "========================================"
echo "Setup Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo ""
echo "1. Start vLLM on your GPU instance(s):"
echo "   HF_TOKEN=your_token python -m vllm.entrypoints.openai.api_server \\"
echo "     --model meta-llama/Llama-3.1-8B-Instruct \\"
echo "     --host 0.0.0.0 --port 8000 \\"
echo "     --enable-prefix-caching"
echo ""
echo "2. Run benchmark:"
echo "   cd $REPO_DIR"
echo "   ./run-benchmark.sh <VLLM_IP_1> <VLLM_IP_2> --model meta-llama/Llama-3.1-8B-Instruct"
echo ""
echo "3. Or run A/B comparison:"
echo "   ./run-ab-comparison.sh <VLLM_IP_1> <VLLM_IP_2>"
echo ""
echo "Examples:"
echo "   # Both vLLM backends on localhost (same GPU instance)"
echo "   ./run-benchmark.sh localhost localhost --model meta-llama/Llama-3.2-1B-Instruct"
echo ""
echo "   # vLLM on separate instances"
echo "   ./run-benchmark.sh 10.0.0.1 10.0.0.2 --model meta-llama/Llama-3.1-8B-Instruct"
echo ""
