# Ranvier Core Makefile
# Build and test targets for the Ranvier LLM routing layer

.PHONY: all build clean test test-unit test-integration integration-up integration-down integration-logs benchmark benchmark-up benchmark-down help

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
	@python3 tests/integration/test_cluster.py

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

# Run load testing benchmark in headless mode
# Runs for 5 minutes by default, outputs CSV and HTML reports
# Fails if P99 latency > 50ms or if cluster sync errors occur
benchmark:
	@echo "======================================"
	@echo "Running Ranvier Load Test Benchmark"
	@echo "======================================"
	@echo ""
	@echo "Configuration:"
	@echo "  Users: $(BENCHMARK_USERS)"
	@echo "  Spawn rate: $(BENCHMARK_SPAWN_RATE)/s"
	@echo "  Duration: $(BENCHMARK_DURATION)"
	@echo "  Reports: $(BENCHMARK_REPORT_DIR)/"
	@echo ""
	@if ! (command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1) && \
	    ! command -v docker-compose >/dev/null 2>&1; then \
		echo "Error: Neither 'docker compose' nor 'docker-compose' found"; \
		exit 1; \
	fi
	@mkdir -p $(BENCHMARK_REPORT_DIR)
	@echo "Starting test cluster..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark up -d --build
	@echo "Waiting for cluster to become healthy..."
	@sleep 15
	@echo ""
	@echo "Starting Locust load test..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark run \
		--name ranvier-benchmark-run \
		locust \
		-f /mnt/locust/locustfile.py \
		--host=http://172.28.2.1:8080 \
		--users $(BENCHMARK_USERS) \
		--spawn-rate $(BENCHMARK_SPAWN_RATE) \
		--run-time $(BENCHMARK_DURATION) \
		--headless \
		--csv=/tmp/benchmark \
		--html=/tmp/benchmark.html \
		--only-summary \
		--exit-code-on-error 1 \
	; LOCUST_EXIT=$$?; \
	echo ""; \
	echo "Extracting reports..."; \
	docker cp ranvier-benchmark-run:/tmp/benchmark.html $(BENCHMARK_REPORT_DIR)/benchmark.html 2>/dev/null || true; \
	docker cp ranvier-benchmark-run:/tmp/benchmark_stats.csv $(BENCHMARK_REPORT_DIR)/benchmark_stats.csv 2>/dev/null || true; \
	docker cp ranvier-benchmark-run:/tmp/benchmark_stats_history.csv $(BENCHMARK_REPORT_DIR)/benchmark_stats_history.csv 2>/dev/null || true; \
	docker cp ranvier-benchmark-run:/tmp/benchmark_failures.csv $(BENCHMARK_REPORT_DIR)/benchmark_failures.csv 2>/dev/null || true; \
	docker cp ranvier-benchmark-run:/tmp/benchmark_exceptions.csv $(BENCHMARK_REPORT_DIR)/benchmark_exceptions.csv 2>/dev/null || true; \
	docker rm ranvier-benchmark-run 2>/dev/null || true; \
	echo "Stopping test cluster..."; \
	$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark down -v --remove-orphans; \
	echo ""; \
	if [ $$LOCUST_EXIT -ne 0 ]; then \
		echo "======================================"; \
		echo "BENCHMARK FAILED (exit code: $$LOCUST_EXIT)"; \
		echo "======================================"; \
		echo "Check $(BENCHMARK_REPORT_DIR)/ for detailed reports"; \
		exit $$LOCUST_EXIT; \
	else \
		echo "======================================"; \
		echo "BENCHMARK PASSED"; \
		echo "======================================"; \
		echo "Reports available in $(BENCHMARK_REPORT_DIR)/"; \
		echo "  - benchmark.html (HTML report)"; \
		echo "  - benchmark_stats.csv (request statistics)"; \
		echo "  - benchmark_stats_history.csv (time series)"; \
		echo "  - benchmark_failures.csv (failure details)"; \
	fi

# Start benchmark cluster for interactive testing via web UI
# After starting, open http://localhost:8089 to access Locust web UI
benchmark-up:
	@echo "Starting benchmark cluster with Locust web UI..."
	@$(DOCKER_COMPOSE) $(COMPOSE_ARGS) --profile benchmark up -d --build
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
	@echo ""
	@echo "Benchmark targets:"
	@echo "  make benchmark      - Run Locust load test (headless, 5 min)"
	@echo "                        Outputs: benchmark-reports/*.csv, *.html"
	@echo "                        Fails if: P99 > 50ms or sync errors > 0"
	@echo "  make benchmark-up   - Start cluster with Locust web UI (port 8089)"
	@echo "  make benchmark-down - Stop benchmark cluster"
	@echo ""
	@echo "  Benchmark variables (override with make benchmark VAR=value):"
	@echo "    BENCHMARK_USERS=10       - Number of concurrent users"
	@echo "    BENCHMARK_SPAWN_RATE=2   - Users spawned per second"
	@echo "    BENCHMARK_DURATION=5m    - Test duration"
	@echo "    BENCHMARK_REPORT_DIR=benchmark-reports - Report output dir"
	@echo ""
	@echo "Integration test helpers:"
	@echo "  make integration-up   - Start test cluster for debugging"
	@echo "  make integration-down - Stop test cluster"
	@echo "  make integration-logs - View cluster logs"
	@echo ""
	@echo "Code quality:"
	@echo "  make format         - Format C++ code with clang-format"
	@echo "  make lint           - Run clang-tidy static analysis"
