# Ranvier Core Makefile
# Build and test targets for the Ranvier LLM routing layer

# Use bash for PIPESTATUS support in benchmark targets
SHELL := /bin/bash

.PHONY: all build clean test test-unit test-integration integration-up integration-down integration-logs bench benchmark benchmark-up benchmark-down benchmark-real benchmark-real-local benchmark-single-gpu benchmark-comparison benchmark-real-up benchmark-real-down helm-lint helm-template helm-dry-run help

# Default target
all: build

# Build the project using CMake
build:
	@echo "Building Ranvier Core..."
	@mkdir -p build
	@cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . -j$$(nproc)

# Build in debug mode
build-debug:
	@echo "Building Ranvier Core (Debug)..."
	@mkdir -p build
	@cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . -j$$(nproc)

# Clean build artifacts
clean:
	@echo "Cleaning build directory..."
	@rm -rf build

# Run all tests
test: test-unit

# Run unit tests (requires build first)
test-unit: build
	@echo "Running unit tests..."
	@cd build && ctest --output-on-failure

# Run integration tests with Docker Compose
# This starts a 3-node cluster with 2 mock backends, runs tests, and cleans up
test-integration:
	@echo "======================================"
	@echo "Running Multi-Node Integration Tests"
	@echo "======================================"
	@echo ""
	@echo "Prerequisites:"
	@echo "  - Docker with Compose (plugin or standalone)"
	@echo "  - Python 3 with 'requests' library"
	@echo ""
	@if ! (command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1) && \
	    ! command -v docker-compose >/dev/null 2>&1; then \
		echo "Error: Neither 'docker compose' nor 'docker-compose' found"; \
		exit 1; \
	fi
	@if ! python3 -c "import requests" 2>/dev/null; then \
		echo "Installing Python 'requests' library..."; \
		pip3 install --user requests || pip install --user requests; \
	fi
	@echo "Starting integration tests..."
	@echo ""
	@echo "=== Test Suite 1/4: Cluster Behavior ==="
	@python3 tests/integration/test_cluster.py
	@echo ""
	@echo "=== Test Suite 2/4: Prefix Routing ==="
	@python3 tests/integration/test_prefix_routing.py
	@echo ""
	@echo "=== Test Suite 3/4: Graceful Shutdown ==="
	@python3 tests/integration/test_graceful_shutdown.py
	@echo ""
	@echo "=== Test Suite 4/4: Negative Paths ==="
	@python3 tests/integration/test_negative_paths.py

# Helper to detect docker compose command
DOCKER_COMPOSE := $(shell if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then echo "docker compose"; elif command -v docker-compose >/dev/null 2>&1; then echo "docker-compose"; fi)
COMPOSE_ARGS := -f docker-compose.test.yml -p ranvier-integration-test

# Start the integration test cluster (for manual testing/debugging)
integration-up:
	@echo "Starting integration test cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) up -d --build
	@echo ""
	@echo "Cluster started. Endpoints:"
	@echo "  Node 1: http://localhost:8081 (metrics: http://localhost:9181)"
	@echo "  Node 2: http://localhost:8082 (metrics: http://localhost:9182)"
	@echo "  Node 3: http://localhost:8083 (metrics: http://localhost:9183)"
	@echo "  Backend 1: http://localhost:11434"
	@echo "  Backend 2: http://localhost:11435"
	@echo ""
	@echo "Use 'make integration-logs' to view logs"
	@echo "Use 'make integration-down' to stop the cluster"

# Stop the integration test cluster
integration-down:
	@echo "Stopping integration test cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) down -v --remove-orphans

# View logs from the integration test cluster
integration-logs:
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) logs -f

# View logs from a specific service (usage: make integration-log-SERVICE SERVICE=ranvier1)
integration-log-%:
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) logs -f $*

# Benchmark configuration
BENCHMARK_USERS ?= 10
BENCHMARK_SPAWN_RATE ?= 2
BENCHMARK_DURATION ?= 5m
BENCHMARK_REPORT_DIR ?= benchmark-reports
P99_LATENCY_THRESHOLD_MS ?= 100
BENCHMARK_BUILD ?= 1
BENCHMARK_TOKEN_FORWARDING ?= 0

# Benchmark run naming: combines optional label with timestamp
# Usage: make benchmark BENCHMARK_LABEL=token_on → token_on_20251227_012810_*
#        make benchmark                          → 20251227_012810_*
BENCHMARK_TIMESTAMP := $(shell date +%Y%m%d_%H%M%S)
BENCHMARK_LABEL ?=
BENCHMARK_RUN_NAME := $(if $(BENCHMARK_LABEL),$(BENCHMARK_LABEL)_$(BENCHMARK_TIMESTAMP),$(BENCHMARK_TIMESTAMP))

# Run load testing benchmark in headless mode
# Runs for 5 minutes by default, outputs CSV and HTML reports
# Fails if P99 TTFT latency exceeds threshold or if cluster sync errors occur
benchmark:
	@echo "======================================"
	@echo "Running Ranvier Load Test Benchmark"
	@echo "======================================"
	@echo ""
	@echo "Configuration:"
	@echo "  Users: $(BENCHMARK_USERS)"
	@echo "  Spawn rate: $(BENCHMARK_SPAWN_RATE)/s"
	@echo "  Duration: $(BENCHMARK_DURATION)"
	@echo "  P99 TTFT threshold: $(P99_LATENCY_THRESHOLD_MS)ms"
	@echo "  Token forwarding: $(BENCHMARK_TOKEN_FORWARDING)"
	@echo "  Reports: $(BENCHMARK_REPORT_DIR)/"
	@echo ""
	@if ! (command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1) && \
	    ! command -v docker-compose >/dev/null 2>&1; then \
		echo "Error: Neither 'docker compose' nor 'docker-compose' found"; \
		exit 1; \
	fi
	@mkdir -p $(BENCHMARK_REPORT_DIR)
	@echo "Starting test cluster..."
	@if [ "$(BENCHMARK_BUILD)" = "1" ]; then \
		RANVIER_ENABLE_TOKEN_FORWARDING=$(BENCHMARK_TOKEN_FORWARDING) \
		$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark up -d --build; \
	else \
		echo "  (using cached images, set BENCHMARK_BUILD=1 to rebuild)"; \
		RANVIER_ENABLE_TOKEN_FORWARDING=$(BENCHMARK_TOKEN_FORWARDING) \
		$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark up -d; \
	fi
	@echo "Waiting for cluster to become healthy..."
	@sleep 15
	@echo ""
	@echo "Starting Locust load test..."
	@echo "Run name: $(BENCHMARK_RUN_NAME)"
	@P99_LATENCY_THRESHOLD_MS=$(P99_LATENCY_THRESHOLD_MS) \
	$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark run --rm \
		-e P99_LATENCY_THRESHOLD_MS=$(P99_LATENCY_THRESHOLD_MS) \
		-e BENCHMARK_RUN_NAME=$(BENCHMARK_RUN_NAME) \
		locust \
		-f /mnt/locust/locustfile.py \
		--host=http://172.28.2.1:8080 \
		--users $(BENCHMARK_USERS) \
		--spawn-rate $(BENCHMARK_SPAWN_RATE) \
		--run-time $(BENCHMARK_DURATION) \
		--headless \
		--only-summary \
		--exit-code-on-error 1 \
		2>&1 | tee $(BENCHMARK_REPORT_DIR)/$(BENCHMARK_RUN_NAME)_output.log \
	; LOCUST_EXIT=$${PIPESTATUS[0]}; \
	echo ""; \
	echo "Parsing results..."; \
	python3 tests/integration/results_parser.py parse \
		$(BENCHMARK_REPORT_DIR)/$(BENCHMARK_RUN_NAME)_output.log \
		-o $(BENCHMARK_REPORT_DIR)/$(BENCHMARK_RUN_NAME)_stats.csv \
		2>/dev/null || echo "  (parser not available, raw log saved)"; \
	echo "Stopping test cluster..."; \
	$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark down -v --remove-orphans; \
	echo ""; \
	if [ $$LOCUST_EXIT -ne 0 ]; then \
		echo "======================================"; \
		echo "BENCHMARK FAILED (exit code: $$LOCUST_EXIT)"; \
		echo "======================================"; \
		echo "Results saved to: $(BENCHMARK_REPORT_DIR)/$(BENCHMARK_RUN_NAME)_*"; \
		exit $$LOCUST_EXIT; \
	else \
		echo "======================================"; \
		echo "BENCHMARK PASSED"; \
		echo "======================================"; \
		echo "Results saved to: $(BENCHMARK_REPORT_DIR)/$(BENCHMARK_RUN_NAME)_*"; \
		echo ""; \
		echo "Compare with previous runs:"; \
		echo "  python3 tests/integration/results_parser.py compare $(BENCHMARK_REPORT_DIR)/<baseline>.log $(BENCHMARK_REPORT_DIR)/$(BENCHMARK_RUN_NAME)_output.log"; \
	fi

# Start benchmark cluster for interactive testing via web UI
# After starting, open http://localhost:8089 to access Locust web UI
benchmark-up:
	@echo "Starting benchmark cluster with Locust web UI..."
	@if [ "$(BENCHMARK_BUILD)" = "1" ]; then \
		$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark up -d --build; \
	else \
		echo "  (using cached images, set BENCHMARK_BUILD=1 to rebuild)"; \
		$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark up -d; \
	fi
	@echo ""
	@echo "Cluster started. Endpoints:"
	@echo "  Locust Web UI: http://localhost:8089"
	@echo "  Node 1: http://localhost:8081 (metrics: http://localhost:9181)"
	@echo "  Node 2: http://localhost:8082 (metrics: http://localhost:9182)"
	@echo "  Node 3: http://localhost:8083 (metrics: http://localhost:9183)"
	@echo ""
	@echo "Use 'make benchmark-down' to stop the cluster"

# Stop benchmark cluster
benchmark-down:
	@echo "Stopping benchmark cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark down -v --remove-orphans
	@echo "Cleanup complete"

# ============================================================================
# Simplified Lambda Labs Benchmark (One Command)
# ============================================================================
# Use this for quick benchmarking on Lambda Labs GPU instances.
# Auto-detects GPUs, starts vLLM, runs benchmark, cleans up.
#
# Usage:
#   make bench                              # Simple defaults
#   make bench MODEL=meta-llama/Llama-3.1-8B-Instruct
#   make bench GPUS=4 DURATION=10m
#   make bench COMPARE=1                    # A/B comparison

.PHONY: bench

bench:
	@./scripts/bench.sh \
		$(if $(MODEL),--model $(MODEL)) \
		$(if $(GPUS),--gpus $(GPUS)) \
		$(if $(DURATION),--duration $(DURATION)) \
		$(if $(USERS),--users $(USERS)) \
		$(if $(filter 1 true yes,$(COMPARE)),--compare) \
		$(if $(filter 1 true yes,$(SKIP_SETUP)),--skip-setup) \
		$(if $(filter 1 true yes,$(SKIP_VLLM)),--skip-vllm) \
		$(if $(VLLM_HOST),--vllm-host $(VLLM_HOST))

# ============================================================================
# Real vLLM Backend Benchmarking
# ============================================================================
# These targets run benchmarks against real vLLM backends instead of mock backends.
# This validates the actual value proposition of prefix-aware routing.

COMPOSE_REAL_ARGS := -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real
BENCHMARK_REAL_DURATION ?= 5m
BENCHMARK_REAL_USERS ?= 10
BENCHMARK_REAL_SPAWN_RATE ?= 2
BENCHMARK_REAL_REPORT_DIR ?= benchmark-reports

# Run benchmark against real vLLM backends (external endpoints)
# Requires VLLM_ENDPOINT_1 and VLLM_ENDPOINT_2 environment variables
benchmark-real:
	@echo "======================================"
	@echo "Running Real vLLM Backend Benchmark"
	@echo "======================================"
	@echo ""
	@if [ -z "$$VLLM_ENDPOINT_1" ] || [ -z "$$VLLM_ENDPOINT_2" ]; then \
		echo "Error: VLLM_ENDPOINT_1 and VLLM_ENDPOINT_2 must be set"; \
		echo ""; \
		echo "Example:"; \
		echo "  VLLM_ENDPOINT_1=http://gpu-server1:8000 \\"; \
		echo "  VLLM_ENDPOINT_2=http://gpu-server2:8000 \\"; \
		echo "  make benchmark-real"; \
		echo ""; \
		echo "Or use 'make benchmark-real-local' for local vLLM (requires GPU)"; \
		exit 1; \
	fi
	@echo "Configuration:"
	@echo "  vLLM Endpoint 1: $$VLLM_ENDPOINT_1"
	@echo "  vLLM Endpoint 2: $$VLLM_ENDPOINT_2"
	@echo "  Users: $(BENCHMARK_REAL_USERS)"
	@echo "  Spawn rate: $(BENCHMARK_REAL_SPAWN_RATE)/s"
	@echo "  Duration: $(BENCHMARK_REAL_DURATION)"
	@echo "  Routing mode: $${RANVIER_ROUTING_MODE:-prefix}"
	@echo ""
	@mkdir -p $(BENCHMARK_REAL_REPORT_DIR)
	@echo "Starting Ranvier cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile benchmark up -d --build
	@echo "Waiting for cluster to become healthy..."
	@sleep 15
	@echo ""
	@echo "Starting load test..."
	@BENCHMARK_RUN_NAME=$$(date +%Y%m%d_%H%M%S)_real; \
	$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile benchmark run --rm \
		-e BENCHMARK_MODE=$${RANVIER_ROUTING_MODE:-prefix} \
		locust \
		-f /mnt/locust/locustfile_real.py \
		--host=http://172.29.2.1:8080 \
		--users $(BENCHMARK_REAL_USERS) \
		--spawn-rate $(BENCHMARK_REAL_SPAWN_RATE) \
		--run-time $(BENCHMARK_REAL_DURATION) \
		--headless \
		--only-summary \
		--exit-code-on-error 1 \
		2>&1 | tee $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_output.log \
	; LOCUST_EXIT=$${PIPESTATUS[0]}; \
	echo ""; \
	echo "Parsing results..."; \
	python3 tests/integration/results_parser.py parse \
		$(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_output.log \
		-o $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_stats.csv \
		2>/dev/null || echo "  (parser output above)"; \
	echo "Stopping cluster..."; \
	$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile benchmark down -v --remove-orphans; \
	echo ""; \
	if [ $$LOCUST_EXIT -ne 0 ]; then \
		echo "======================================"; \
		echo "BENCHMARK FAILED (exit code: $$LOCUST_EXIT)"; \
		echo "======================================"; \
	else \
		echo "======================================"; \
		echo "BENCHMARK PASSED"; \
		echo "======================================"; \
	fi; \
	echo "Results saved to: $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_*"; \
	exit $$LOCUST_EXIT

# Run benchmark with local vLLM containers (requires NVIDIA GPU)
benchmark-real-local:
	@echo "======================================"
	@echo "Running Local vLLM Backend Benchmark"
	@echo "======================================"
	@echo ""
	@# Pre-flight checks for required environment
	@if ! command -v nvidia-smi >/dev/null 2>&1; then \
		echo "Error: nvidia-smi not found. GPU required for local vLLM."; \
		exit 1; \
	fi
	@if [ -z "$${HF_TOKEN:-}" ]; then \
		echo "======================================"; \
		echo "Error: HF_TOKEN environment variable not set"; \
		echo "======================================"; \
		echo ""; \
		echo "The Llama model requires authentication with Hugging Face."; \
		echo ""; \
		echo "To fix:"; \
		echo "  1. Get a token from https://huggingface.co/settings/tokens"; \
		echo "  2. Accept the license at https://huggingface.co/meta-llama/Llama-3.2-1B-Instruct"; \
		echo "  3. Run: export HF_TOKEN=your_token_here"; \
		echo "  4. Re-run this benchmark"; \
		echo ""; \
		exit 1; \
	fi
	@echo "GPU detected. Starting local vLLM backends..."
	@echo "Model: $${VLLM_MODEL:-meta-llama/Llama-3.2-1B-Instruct}"
	@echo ""
	@mkdir -p $(BENCHMARK_REAL_REPORT_DIR)
	@echo "Starting vLLM backends and Ranvier cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile local-vllm --profile benchmark up -d --build
	@echo "Waiting for vLLM backends to load model (this may take a few minutes)..."
	@sleep 120
	@echo ""
	@echo "Starting load test..."
	@BENCHMARK_RUN_NAME=$$(date +%Y%m%d_%H%M%S)_local; \
	$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile local-vllm --profile benchmark run --rm \
		-e BENCHMARK_MODE=$${RANVIER_ROUTING_MODE:-prefix} \
		locust \
		-f /mnt/locust/locustfile_real.py \
		--host=http://172.29.2.1:8080 \
		--users $(BENCHMARK_REAL_USERS) \
		--spawn-rate $(BENCHMARK_REAL_SPAWN_RATE) \
		--run-time $(BENCHMARK_REAL_DURATION) \
		--headless \
		--only-summary \
		--exit-code-on-error 1 \
		2>&1 | tee $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_output.log \
	; LOCUST_EXIT=$${PIPESTATUS[0]}; \
	echo ""; \
	echo "Parsing results..."; \
	python3 tests/integration/results_parser.py parse \
		$(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_output.log \
		-o $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_stats.csv \
		2>/dev/null || echo "  (parser output above)"; \
	echo "Stopping cluster..."; \
	$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile local-vllm --profile benchmark down -v --remove-orphans; \
	echo ""; \
	if [ $$LOCUST_EXIT -ne 0 ]; then \
		echo "======================================"; \
		echo "BENCHMARK FAILED (exit code: $$LOCUST_EXIT)"; \
		echo "======================================"; \
	else \
		echo "======================================"; \
		echo "BENCHMARK PASSED"; \
		echo "======================================"; \
	fi; \
	echo "Results saved to: $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_*"; \
	exit $$LOCUST_EXIT

# Run benchmark with single GPU (sanity check mode)
# Useful when you only have 1 GPU available for testing
benchmark-single-gpu:
	@echo "======================================"
	@echo "Running Single-GPU Benchmark Test"
	@echo "======================================"
	@echo ""
	@# Pre-flight checks for required environment
	@if ! command -v nvidia-smi >/dev/null 2>&1; then \
		echo "Error: nvidia-smi not found. GPU required."; \
		exit 1; \
	fi
	@if [ -z "$${HF_TOKEN:-}" ]; then \
		echo "======================================"; \
		echo "Error: HF_TOKEN environment variable not set"; \
		echo "======================================"; \
		echo ""; \
		echo "The Llama model requires authentication with Hugging Face."; \
		echo ""; \
		echo "To fix:"; \
		echo "  1. Get a token from https://huggingface.co/settings/tokens"; \
		echo "  2. Accept the license at https://huggingface.co/meta-llama/Llama-3.2-1B-Instruct"; \
		echo "  3. Run: export HF_TOKEN=your_token_here"; \
		echo "  4. Re-run this benchmark"; \
		echo ""; \
		exit 1; \
	fi
	@echo "GPU detected:"
	@nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null || true
	@echo ""
	@echo "This is a sanity check test with a single vLLM backend."
	@echo "For A/B comparison of routing strategies, use 2+ GPUs."
	@echo "Model: $${VLLM_MODEL:-meta-llama/Llama-3.2-1B-Instruct}"
	@echo ""
	@mkdir -p $(BENCHMARK_REAL_REPORT_DIR)
	@echo "Starting vLLM backend and Ranvier cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile single-gpu up -d --build
	@echo "Waiting for vLLM backend to load model (this may take 2-3 minutes)..."
	@sleep 120
	@echo ""
	@echo "Starting load test..."
	@BENCHMARK_RUN_NAME=$$(date +%Y%m%d_%H%M%S)_single_gpu; \
	$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile single-gpu run --rm \
		-e BENCHMARK_MODE=$${RANVIER_ROUTING_MODE:-prefix} \
		locust-single-gpu \
		-f /mnt/locust/locustfile_real.py \
		--host=http://172.29.2.1:8080 \
		--users $(BENCHMARK_REAL_USERS) \
		--spawn-rate $(BENCHMARK_REAL_SPAWN_RATE) \
		--run-time $(BENCHMARK_REAL_DURATION) \
		--headless \
		--only-summary \
		--exit-code-on-error 1 \
		--stop-timeout 10 \
		2>&1 | tee $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_output.log \
	; LOCUST_EXIT=$${PIPESTATUS[0]}; \
	echo ""; \
	echo "Parsing results..."; \
	python3 tests/integration/results_parser.py parse \
		$(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_output.log \
		-o $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_stats.csv \
		2>/dev/null || echo "  (parser output above)"; \
	echo "Stopping cluster..."; \
	$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile single-gpu down -v --remove-orphans; \
	echo ""; \
	if [ $$LOCUST_EXIT -ne 0 ]; then \
		echo "======================================"; \
		echo "BENCHMARK FAILED (exit code: $$LOCUST_EXIT)"; \
		echo "======================================"; \
	else \
		echo "======================================"; \
		echo "BENCHMARK PASSED"; \
		echo "======================================"; \
	fi; \
	echo "Results saved to: $(BENCHMARK_REAL_REPORT_DIR)/$${BENCHMARK_RUN_NAME}_*"; \
	exit $$LOCUST_EXIT

# Run A/B comparison: prefix-aware vs round-robin routing
benchmark-comparison:
	@echo "======================================"
	@echo "Running Routing Strategy Comparison"
	@echo "======================================"
	@echo ""
	@echo "This will run two benchmarks:"
	@echo "  1. Round-robin routing (baseline)"
	@echo "  2. Prefix-aware routing (optimized)"
	@echo ""
	@python3 tests/integration/run_benchmark_comparison.py \
		--duration $(BENCHMARK_REAL_DURATION) \
		--users $(BENCHMARK_REAL_USERS) \
		--spawn-rate $(BENCHMARK_REAL_SPAWN_RATE) \
		$(if $(LOCAL_VLLM),--local-vllm,)

# Start real benchmark cluster for interactive testing
benchmark-real-up:
	@echo "Starting real vLLM benchmark cluster..."
	@if [ -z "$$VLLM_ENDPOINT_1" ] || [ -z "$$VLLM_ENDPOINT_2" ]; then \
		echo "Warning: VLLM_ENDPOINT_* not set, using defaults"; \
	fi
	@$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile benchmark up -d --build
	@echo ""
	@echo "Cluster started. Endpoints:"
	@echo "  Locust Web UI: http://localhost:8089"
	@echo "  Node 1: http://localhost:8081 (metrics: http://localhost:9181)"
	@echo "  Node 2: http://localhost:8082 (metrics: http://localhost:9182)"
	@echo "  Node 3: http://localhost:8083 (metrics: http://localhost:9183)"
	@echo ""
	@echo "Use 'make benchmark-real-down' to stop"

# Stop real benchmark cluster
benchmark-real-down:
	@echo "Stopping real benchmark cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_REAL_ARGS) --profile local-vllm --profile benchmark --profile single-gpu down -v --remove-orphans
	@echo "Cleanup complete"

# Build Docker production image (uses docker-compose to work in devcontainers)
docker-build:
	@echo "Building production Docker image..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) build ranvier1
	@echo "Image built and tagged. Subsequent test runs will use cached image."

# Format C++ code (requires clang-format)
format:
	@echo "Formatting C++ code..."
	@find src tests -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i

# Run clang-tidy static analysis (requires clang-tidy and compile_commands.json)
lint:
	@echo "Running clang-tidy..."
	@if [ ! -f build/compile_commands.json ]; then \
		echo "Error: compile_commands.json not found. Run 'make build' first."; \
		exit 1; \
	fi
	@find src -name "*.cpp" | xargs clang-tidy -p build

# ============================================================================
# Helm Chart Targets
# ============================================================================
# These targets help with developing and testing the Kubernetes Helm chart.
# Requires: helm 3.8+ installed locally

HELM_CHART_DIR := deploy/helm/ranvier
HELM_RELEASE_NAME ?= ranvier
HELM_NAMESPACE ?= ranvier

# Lint the Helm chart for errors and best practices
helm-lint:
	@echo "======================================"
	@echo "Linting Ranvier Helm Chart"
	@echo "======================================"
	@if ! command -v helm >/dev/null 2>&1; then \
		echo "Error: helm not found. Install from https://helm.sh/docs/intro/install/"; \
		exit 1; \
	fi
	helm lint $(HELM_CHART_DIR)

# Render Helm templates locally (no cluster required)
helm-template:
	@echo "======================================"
	@echo "Rendering Ranvier Helm Templates"
	@echo "======================================"
	@if ! command -v helm >/dev/null 2>&1; then \
		echo "Error: helm not found. Install from https://helm.sh/docs/intro/install/"; \
		exit 1; \
	fi
	helm template $(HELM_RELEASE_NAME) $(HELM_CHART_DIR) \
		--namespace $(HELM_NAMESPACE) \
		--debug

# Dry-run installation against a cluster (requires cluster access)
helm-dry-run:
	@echo "======================================"
	@echo "Dry-Run Ranvier Helm Installation"
	@echo "======================================"
	@if ! command -v helm >/dev/null 2>&1; then \
		echo "Error: helm not found. Install from https://helm.sh/docs/intro/install/"; \
		exit 1; \
	fi
	@if ! kubectl cluster-info >/dev/null 2>&1; then \
		echo "Error: Cannot connect to Kubernetes cluster."; \
		echo "Configure kubectl or use 'make helm-template' for offline validation."; \
		exit 1; \
	fi
	helm install $(HELM_RELEASE_NAME) $(HELM_CHART_DIR) \
		--namespace $(HELM_NAMESPACE) \
		--create-namespace \
		--dry-run

# =============================================================================
# Validation Suite
# =============================================================================

# Run all validation tests (requires built binary)
test-validation: build
	@echo "Running validation suite unit tests..."
	@python3 tests/unit/test_validation_gossip.py -v
	@bash tests/unit/test_validation_common.sh

# Run production readiness validation (full suite)
# Requires: wrk, stress-ng (optional), perf (optional)
validate: build
	@echo "Running Production Readiness Validation Suite..."
	@./validation/validate_v1.sh

# Run quick validation (shorter durations)
validate-quick: build
	@echo "Running Quick Validation..."
	@./validation/validate_v1.sh --quick

# Run validation in CI mode (strict thresholds)
validate-ci: build
	@echo "Running CI Validation..."
	@./validation/validate_v1.sh --ci --output validation/reports/ci_results.json

# Show help
help:
	@echo "Ranvier Core Build System"
	@echo ""
	@echo "Build targets:"
	@echo "  make build          - Build the project (Release mode)"
	@echo "  make build-debug    - Build the project (Debug mode)"
	@echo "  make clean          - Clean build artifacts"
	@echo "  make docker-build   - Build production Docker image"
	@echo ""
	@echo "Test targets:"
	@echo "  make test           - Run all tests (currently: unit tests)"
	@echo "  make test-unit      - Run unit tests"
	@echo "  make test-integration - Run multi-node integration tests"
	@echo "  make test-validation  - Run validation suite unit tests"
	@echo ""
	@echo "Production Readiness Validation:"
	@echo "  make validate       - Run full validation suite (all 4 tests)"
	@echo "  make validate-quick - Run quick validation (shorter durations)"
	@echo "  make validate-ci    - Run CI validation (strict thresholds)"
	@echo ""
	@echo "Mock Backend Benchmark (fast, validates router overhead):"
	@echo "  make benchmark      - Run Locust load test with mock backends"
	@echo "  make benchmark-up   - Start cluster with Locust web UI (port 8089)"
	@echo "  make benchmark-down - Stop benchmark cluster"
	@echo ""
	@echo "Lambda Labs Benchmark (RECOMMENDED - one command, auto-detects GPUs):"
	@echo "  make bench          - Auto-detect GPUs, start vLLM, run benchmark"
	@echo "  make bench MODEL=meta-llama/Llama-3.1-8B-Instruct"
	@echo "  make bench GPUS=4 DURATION=10m USERS=20"
	@echo "  make bench COMPARE=1              - A/B: prefix vs round-robin"
	@echo "  ./scripts/bench.sh --help         - Full options"
	@echo ""
	@echo "Real vLLM Backend Benchmark (validates prefix-aware routing value):"
	@echo "  make benchmark-real       - Run with external vLLM endpoints"
	@echo "                              Requires: VLLM_ENDPOINT_1, VLLM_ENDPOINT_2"
	@echo "  make benchmark-real-local - Run with local vLLM (requires 2 GPUs)"
	@echo "  make benchmark-single-gpu - Sanity check with 1 GPU (no A/B test)"
	@echo "  make benchmark-comparison - A/B test: prefix vs round-robin routing"
	@echo "  make benchmark-real-up    - Start cluster for interactive testing"
	@echo "  make benchmark-real-down  - Stop real benchmark cluster"
	@echo ""
	@echo "  Example:"
	@echo "    VLLM_ENDPOINT_1=http://gpu1:8000 VLLM_ENDPOINT_2=http://gpu2:8000 \\"
	@echo "    make benchmark-real"
	@echo ""
	@echo "  Benchmark variables (override with make benchmark VAR=value):"
	@echo "    BENCHMARK_USERS=10       - Number of concurrent users"
	@echo "    BENCHMARK_SPAWN_RATE=2   - Users spawned per second"
	@echo "    BENCHMARK_DURATION=5m    - Test duration"
	@echo "    BENCHMARK_REPORT_DIR=benchmark-reports - Report output dir"
	@echo "    P99_LATENCY_THRESHOLD_MS=100 - P99 TTFT threshold (mock: 100ms, real: 5000ms)"
	@echo "    BENCHMARK_LABEL=<name>   - Label prefix for output files"
	@echo "    BENCHMARK_BUILD=1        - Set to 0 to skip rebuilding images"
	@echo "    RANVIER_ROUTING_MODE=prefix - Routing mode: prefix, hash, or random"
	@echo "    PROMPT_DISTRIBUTION=mixed   - Prompt length: short, medium, long, mixed"
	@echo "    SHARED_PREFIX_RATIO=0.7     - Ratio of requests with shared prefix"
	@echo ""
	@echo "  Compare benchmark results:"
	@echo "    python3 tests/integration/results_parser.py compare <baseline/benchmark.log> <new/benchmark.log>"
	@echo ""
	@echo "Integration test helpers:"
	@echo "  make integration-up   - Start test cluster for debugging"
	@echo "  make integration-down - Stop test cluster"
	@echo "  make integration-logs - View cluster logs"
	@echo ""
	@echo "Code quality:"
	@echo "  make format         - Format C++ code with clang-format"
	@echo "  make lint           - Run clang-tidy static analysis"
	@echo ""
	@echo "Helm chart (Kubernetes deployment):"
	@echo "  make helm-lint      - Lint the Helm chart"
	@echo "  make helm-template  - Render templates locally (no cluster needed)"
	@echo "  make helm-dry-run   - Dry-run install against a cluster"
	@echo ""
	@echo "  Helm variables:"
	@echo "    HELM_RELEASE_NAME=ranvier  - Helm release name"
	@echo "    HELM_NAMESPACE=ranvier     - Kubernetes namespace"
