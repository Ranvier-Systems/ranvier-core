#!/bin/bash
# Multi-GPU Benchmark Runner for Ranvier Core
#
# This script simplifies running multi-GPU benchmarks against real vLLM backends.
# It handles endpoint validation, environment variable setup, and provides clear
# status output.
#
# Usage:
#   ./scripts/run-multi-gpu-benchmark.sh <GPU1_IP> <GPU2_IP> [options]
#
# Examples:
#   # Run benchmark with two external GPU servers
#   ./scripts/run-multi-gpu-benchmark.sh 129.213.118.109 123.45.67.89
#
#   # Run with custom duration and users
#   ./scripts/run-multi-gpu-benchmark.sh 129.213.118.109 123.45.67.89 --duration 10m --users 20
#
#   # Skip endpoint health checks (useful if endpoints are behind NAT)
#   ./scripts/run-multi-gpu-benchmark.sh 129.213.118.109 123.45.67.89 --skip-health-check

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
PORT="${VLLM_PORT:-8000}"
DURATION="${BENCHMARK_REAL_DURATION:-5m}"
USERS="${BENCHMARK_REAL_USERS:-10}"
SPAWN_RATE="${BENCHMARK_REAL_SPAWN_RATE:-2}"
ROUTING_MODE="${RANVIER_ROUTING_MODE:-prefix}"
SKIP_HEALTH_CHECK=false
MODEL="${VLLM_MODEL:-meta-llama/Llama-3.1-8B-Instruct}"

print_usage() {
    echo "Usage: $0 <GPU1_IP> <GPU2_IP> [options]"
    echo ""
    echo "Required arguments:"
    echo "  GPU1_IP    IP address of first GPU server running vLLM"
    echo "  GPU2_IP    IP address of second GPU server running vLLM"
    echo ""
    echo "Options:"
    echo "  --port PORT           vLLM port (default: 8000)"
    echo "  --duration TIME       Benchmark duration (default: 5m)"
    echo "  --users N             Concurrent users (default: 10)"
    echo "  --spawn-rate N        Users spawned per second (default: 2)"
    echo "  --routing-mode MODE   Routing mode: prefix or round_robin (default: prefix)"
    echo "  --model MODEL         Model name (default: meta-llama/Llama-3.1-8B-Instruct)"
    echo "  --skip-health-check   Skip endpoint health validation"
    echo "  --dry-run             Show commands without executing"
    echo "  -h, --help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 129.213.118.109 123.45.67.89"
    echo "  $0 129.213.118.109 123.45.67.89 --duration 10m --users 20"
    echo ""
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse command line arguments
if [ $# -lt 2 ]; then
    print_usage
    exit 1
fi

GPU1_IP="$1"
GPU2_IP="$2"
shift 2

DRY_RUN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --port)
            PORT="$2"
            shift 2
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --users)
            USERS="$2"
            shift 2
            ;;
        --spawn-rate)
            SPAWN_RATE="$2"
            shift 2
            ;;
        --routing-mode)
            ROUTING_MODE="$2"
            shift 2
            ;;
        --model)
            MODEL="$2"
            shift 2
            ;;
        --skip-health-check)
            SKIP_HEALTH_CHECK=true
            shift
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

echo ""
echo "========================================"
echo "Ranvier Multi-GPU Benchmark"
echo "========================================"
echo ""

# Display configuration
log_info "Configuration:"
echo "  GPU Server 1: ${GPU1_IP}:${PORT}"
echo "  GPU Server 2: ${GPU2_IP}:${PORT}"
echo "  Model: ${MODEL}"
echo "  Routing Mode: ${ROUTING_MODE}"
echo "  Duration: ${DURATION}"
echo "  Users: ${USERS}"
echo "  Spawn Rate: ${SPAWN_RATE}/s"
echo ""

# Health check endpoints
if [ "$SKIP_HEALTH_CHECK" = false ]; then
    log_info "Checking vLLM endpoint health..."

    ENDPOINT1_OK=false
    ENDPOINT2_OK=false

    # Check endpoint 1
    echo -n "  Checking ${GPU1_IP}:${PORT}... "
    if curl -sf --connect-timeout 5 "http://${GPU1_IP}:${PORT}/health" >/dev/null 2>&1; then
        echo -e "${GREEN}OK${NC}"
        ENDPOINT1_OK=true
    else
        echo -e "${YELLOW}UNREACHABLE${NC}"
    fi

    # Check endpoint 2
    echo -n "  Checking ${GPU2_IP}:${PORT}... "
    if curl -sf --connect-timeout 5 "http://${GPU2_IP}:${PORT}/health" >/dev/null 2>&1; then
        echo -e "${GREEN}OK${NC}"
        ENDPOINT2_OK=true
    else
        echo -e "${YELLOW}UNREACHABLE${NC}"
    fi

    echo ""

    if [ "$ENDPOINT1_OK" = false ] || [ "$ENDPOINT2_OK" = false ]; then
        log_warn "Some endpoints are unreachable from this machine."
        log_warn "This could be due to network restrictions or firewall rules."
        log_warn "The benchmark containers may still be able to reach them."
        echo ""
        read -p "Continue anyway? [y/N] " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            log_info "Aborted."
            exit 0
        fi
    else
        log_success "Both endpoints are healthy"
    fi
    echo ""
fi

# Set environment variables for benchmark
export VLLM_ENDPOINT_1="http://${GPU1_IP}:${PORT}"
export VLLM_ENDPOINT_2="http://${GPU2_IP}:${PORT}"
export BACKEND1_IP="${GPU1_IP}"
export BACKEND1_PORT="${PORT}"
export BACKEND2_IP="${GPU2_IP}"
export BACKEND2_PORT="${PORT}"
export VLLM_MODEL="${MODEL}"
export RANVIER_ROUTING_MODE="${ROUTING_MODE}"
export BENCHMARK_REAL_DURATION="${DURATION}"
export BENCHMARK_REAL_USERS="${USERS}"
export BENCHMARK_REAL_SPAWN_RATE="${SPAWN_RATE}"

# For external endpoints, need to use host network or extra_hosts
# The docker-compose already has extra_hosts configured for host.docker.internal

log_info "Environment variables set:"
echo "  VLLM_ENDPOINT_1=${VLLM_ENDPOINT_1}"
echo "  VLLM_ENDPOINT_2=${VLLM_ENDPOINT_2}"
echo "  BACKEND1_IP=${BACKEND1_IP}"
echo "  BACKEND2_IP=${BACKEND2_IP}"
echo "  VLLM_MODEL=${VLLM_MODEL}"
echo "  RANVIER_ROUTING_MODE=${RANVIER_ROUTING_MODE}"
echo ""

if [ "$DRY_RUN" = true ]; then
    log_info "DRY RUN - Would execute:"
    echo "  make benchmark-real"
    echo ""
    log_info "Or run manually with:"
    echo ""
    echo "  VLLM_ENDPOINT_1=http://${GPU1_IP}:${PORT} \\"
    echo "  VLLM_ENDPOINT_2=http://${GPU2_IP}:${PORT} \\"
    echo "  BACKEND1_IP=${GPU1_IP} \\"
    echo "  BACKEND2_IP=${GPU2_IP} \\"
    echo "  VLLM_MODEL=${MODEL} \\"
    echo "  RANVIER_ROUTING_MODE=${ROUTING_MODE} \\"
    echo "  BENCHMARK_REAL_DURATION=${DURATION} \\"
    echo "  BENCHMARK_REAL_USERS=${USERS} \\"
    echo "  make benchmark-real"
    echo ""
    exit 0
fi

log_info "Starting benchmark..."
echo ""

# Run the benchmark
make benchmark-real

echo ""
log_success "Benchmark complete!"
echo ""
