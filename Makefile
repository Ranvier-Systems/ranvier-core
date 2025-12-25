# Ranvier Core Makefile
# Build and test targets for the Ranvier LLM routing layer

.PHONY: all build clean test test-unit test-integration integration-up integration-down integration-logs help

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
	@echo "  - Docker and docker-compose installed"
	@echo "  - Python 3 with 'requests' library"
	@echo ""
	@if ! command -v docker-compose >/dev/null 2>&1; then \
		echo "Error: docker-compose is not installed"; \
		exit 1; \
	fi
	@if ! python3 -c "import requests" 2>/dev/null; then \
		echo "Installing Python 'requests' library..."; \
		pip3 install --user requests || pip install --user requests; \
	fi
	@echo "Starting integration tests..."
	@python3 tests/integration/test_cluster.py || \
		(echo "Tests failed. Cleaning up..." && \
		docker-compose -f docker-compose.test.yml -p ranvier-integration-test down -v --remove-orphans && \
		exit 1)

# Start the integration test cluster (for manual testing/debugging)
integration-up:
	@echo "Starting integration test cluster..."
	@docker-compose -f docker-compose.test.yml -p ranvier-integration-test up -d --build
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
	@docker-compose -f docker-compose.test.yml -p ranvier-integration-test down -v --remove-orphans

# View logs from the integration test cluster
integration-logs:
	@docker-compose -f docker-compose.test.yml -p ranvier-integration-test logs -f

# View logs from a specific service (usage: make integration-log-SERVICE SERVICE=ranvier1)
integration-log-%:
	@docker-compose -f docker-compose.test.yml -p ranvier-integration-test logs -f $*

# Build Docker production image
docker-build:
	@echo "Building production Docker image..."
	@docker build -f Dockerfile.production -t ranvier:latest .

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
	@echo "Integration test helpers:"
	@echo "  make integration-up   - Start test cluster for debugging"
	@echo "  make integration-down - Stop test cluster"
	@echo "  make integration-logs - View cluster logs"
	@echo ""
	@echo "Code quality:"
	@echo "  make format         - Format C++ code with clang-format"
	@echo "  make lint           - Run clang-tidy static analysis"
