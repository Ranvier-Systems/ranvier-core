cmake_minimum_required(VERSION 3.24)
project(ranvier VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# FetchContent for external dependencies (fallback when system packages unavailable)
include(FetchContent)

# Google Test Configuration
# Try system package first, fall back to FetchContent
find_package(GTest QUIET)
if(GTest_FOUND)
    message(STATUS "Found system GTest: ${GTest_VERSION}")
else()
    message(STATUS "System GTest not found - fetching from source")
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest
      GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# yaml-cpp Configuration
# Try system package first, fall back to FetchContent
find_package(yaml-cpp QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "Found system yaml-cpp")
else()
    message(STATUS "System yaml-cpp not found - fetching from source")
    FetchContent_Declare(
      yaml-cpp
      GIT_REPOSITORY https://github.com/jbeder/yaml-cpp
      GIT_TAG        0.8.0
    )
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(yaml-cpp)
endif()

# Abseil Configuration
# Try system package first, fall back to FetchContent
find_package(absl QUIET)
if(absl_FOUND)
    message(STATUS "Found system Abseil")
else()
    message(STATUS "System Abseil not found - fetching from source")
    FetchContent_Declare(
      abseil-cpp
      GIT_REPOSITORY https://github.com/abseil/abseil-cpp
      GIT_TAG        20240116.1
    )
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(abseil-cpp)
endif()

# RapidJSON Configuration
FetchContent_Declare(
  rapidjson
  GIT_REPOSITORY https://github.com/Tencent/rapidjson
  GIT_TAG        v1.1.0
)
set(RAPIDJSON_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_THIRDPARTY_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidjson)

# RapidJSON is header-only. We must define the include path for the project.
# The headers are located in the 'include' subdirectory of the fetched source.
add_library(rapidjson_interface INTERFACE)
target_include_directories(rapidjson_interface SYSTEM INTERFACE ${rapidjson_SOURCE_DIR}/include)

# Option to disable OpenTelemetry (avoids static initialization that can block)
option(WITH_TELEMETRY "Enable OpenTelemetry distributed tracing" ON)

# Option to enable debug metrics (adds scrape overhead, use for development only)
# See BACKLOG.md Section 8.5 for rationale
option(RANVIER_DEBUG_METRICS "Enable gossip debug metrics (crypto offload stats, overflow counters)" OFF)

if(WITH_TELEMETRY)
    # OpenTelemetry C++ SDK Configuration
    # Provides distributed tracing with Zipkin HTTP export support
    # Note: OTLP export requires protobuf; use WITH_OTLP_HTTP=ON if protobuf is installed
    find_package(Protobuf QUIET)
    FetchContent_Declare(
      opentelemetry-cpp
      GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp
      GIT_TAG        v1.14.2
    )
    if(Protobuf_FOUND)
        message(STATUS "Protobuf found - enabling OTLP exporter")
        set(WITH_OTLP_HTTP ON CACHE BOOL "" FORCE)
    else()
        message(STATUS "Protobuf not found - using Zipkin exporter (install libprotobuf-dev for OTLP)")
        set(WITH_OTLP_HTTP OFF CACHE BOOL "" FORCE)
    endif()
    set(WITH_OTLP_GRPC OFF CACHE BOOL "" FORCE)  # Avoid gRPC dependency
    set(WITH_ZIPKIN ON CACHE BOOL "" FORCE)  # Zipkin works without protobuf
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(OTELCPP_MAINTAINER_MODE OFF CACHE BOOL "" FORCE)
    set(WITH_ABSEIL OFF CACHE BOOL "" FORCE)  # Don't use external abseil
    set(WITH_STL "CXX20" CACHE STRING "" FORCE)  # Use C++20 stdlib instead of internal absl
    FetchContent_MakeAvailable(opentelemetry-cpp)
else()
    message(STATUS "OpenTelemetry disabled - build with -DWITH_TELEMETRY=ON to enable")
endif()

# SQLite3 Configuration
# Try system SQLite first, fall back to building from source
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    message(STATUS "System SQLite3 not found - fetching from source")
    FetchContent_Declare(
      sqlite3
      URL https://www.sqlite.org/2024/sqlite-amalgamation-3450000.zip
      URL_HASH SHA256=bde30d13ebdf84926ddd5e8b6df145be03a577a48fd075a087a5dd815bcdf740
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sqlite3)

    # Build SQLite as a library
    add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
    set(SQLITE3_LIBRARIES sqlite3_lib)
    set(SQLITE3_INCLUDE_DIRS ${sqlite3_SOURCE_DIR})
else()
    message(STATUS "Found system SQLite3: ${SQLite3_VERSION}")
    set(SQLITE3_LIBRARIES SQLite::SQLite3)
    set(SQLITE3_INCLUDE_DIRS ${SQLite3_INCLUDE_DIRS})
endif()

# Seastar Configuration (Optional - for full server build)
find_package(Seastar QUIET)

if(Seastar_FOUND)
    message(STATUS "Seastar found - building full server and all tests")

    # OpenSSL Configuration (Required for DTLS gossip encryption)
    find_package(OpenSSL REQUIRED)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")

    # Tokenizers-CPP Configuration (Downloads it automatically)
    # Pinned to a specific commit for reproducible builds. Older tags (v0.1.0,
    # v0.1.1) have API incompatibilities with the tokenizers crate version that
    # Cargo resolves to. There's a known crash in tokenizers 0.21.4
    # NormalizedString::slice — this pin uses HF tokenizer 0.21.2.
    # To update: see "Updating pinned tokenizers-cpp" in BACKLOG.md §20.10.
    FetchContent_Declare(
      tokenizers_cpp
      GIT_REPOSITORY https://github.com/mlc-ai/tokenizers-cpp
      GIT_TAG        34885cfd7b9ef27b859c28a41e71413dd31926f5  # 2026-01-23 (CMake compat + HF 0.21.2)
    )
    # Suppress warnings from third-party code compiled within tokenizers_cpp
    # (vendored protobuf-lite emits -Warray-bounds false positives with GCC 12+)
    set(_saved_c_flags "${CMAKE_C_FLAGS}")
    set(_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    string(APPEND CMAKE_C_FLAGS " -w")
    string(APPEND CMAKE_CXX_FLAGS " -w")
    FetchContent_MakeAvailable(tokenizers_cpp)
    set(CMAKE_C_FLAGS "${_saved_c_flags}")
    set(CMAKE_CXX_FLAGS "${_saved_cxx_flags}")

    # ==========================================================================
    # Patch tokenizers-cpp to use jemalloc as Rust's global allocator
    # ==========================================================================
    # WHY: Seastar replaces malloc globally with its per-shard allocator. When Rust
    # FFI code runs on worker threads, allocations are tracked as foreign_mallocs.
    # The interaction between Seastar's do_foreign_free and Rust's internal allocator
    # causes memory corruption under stress (SIGSEGV with corrupted pointers).
    #
    # SOLUTION: Build tokenizers-cpp with tikv-jemallocator as the global allocator.
    # This gives Rust its own memory allocator that bypasses Seastar entirely.
    #
    # See: Rule #15 in .dev-context/claude-context.md
    # See: BACKLOG.md "Rebuild tokenizers-cpp with statically-linked jemalloc"
    # ==========================================================================

    set(TOKENIZERS_CARGO_TOML "${tokenizers_cpp_SOURCE_DIR}/rust/Cargo.toml")
    set(TOKENIZERS_LIB_RS "${tokenizers_cpp_SOURCE_DIR}/rust/src/lib.rs")

    # Check if patches have already been applied (idempotent patching)
    file(READ "${TOKENIZERS_CARGO_TOML}" CARGO_TOML_CONTENT)
    string(FIND "${CARGO_TOML_CONTENT}" "tikv-jemallocator" JEMALLOC_FOUND)

    if(JEMALLOC_FOUND EQUAL -1)
        message(STATUS "Patching tokenizers-cpp to use jemalloc allocator...")

        # Patch Cargo.toml: Add tikv-jemallocator dependency
        string(REPLACE
            "[dependencies]"
            "[dependencies]\ntikv-jemallocator = \"0.6\""
            CARGO_TOML_PATCHED
            "${CARGO_TOML_CONTENT}"
        )
        file(WRITE "${TOKENIZERS_CARGO_TOML}" "${CARGO_TOML_PATCHED}")
        message(STATUS "  - Added tikv-jemallocator to Cargo.toml")

        # Patch lib.rs: Add global allocator declaration at the top
        file(READ "${TOKENIZERS_LIB_RS}" LIB_RS_CONTENT)
        set(JEMALLOC_DECLARATION
"// =============================================================================
// PATCHED BY RANVIER: Use jemalloc as global allocator for Seastar compatibility
// Seastar replaces malloc globally. Without this, Rust FFI causes memory corruption.
// See: Rule #15 in ranvier-core/.dev-context/claude-context.md
// =============================================================================
use tikv_jemallocator::Jemalloc;

#[global_allocator]
static GLOBAL: Jemalloc = Jemalloc;

")
        set(LIB_RS_PATCHED "${JEMALLOC_DECLARATION}${LIB_RS_CONTENT}")
        file(WRITE "${TOKENIZERS_LIB_RS}" "${LIB_RS_PATCHED}")
        message(STATUS "  - Added #[global_allocator] to lib.rs")

        message(STATUS "Tokenizers-cpp jemalloc patch complete")
    else()
        message(STATUS "Tokenizers-cpp jemalloc patch already applied (skipping)")
    endif()

    # Limit the Noise
    add_compile_options(-fmax-errors=3)

    # Define the Main Server Executable
    add_executable(ranvier_server
      src/main.cpp
      src/agent_registry.cpp
      src/application.cpp
      src/async_persistence.cpp
      src/config_loader.cpp
      src/crypto_offloader.cpp
      src/dtls_context.cpp
      src/gossip_consensus.cpp
      src/gossip_protocol.cpp
      src/gossip_service.cpp
      src/gossip_transport.cpp
      src/health_service.cpp
      src/http_controller.cpp
      src/intent_classifier.cpp
      src/k8s_discovery_service.cpp
      src/local_discovery.cpp
      src/node_slab.cpp
      src/router_service.cpp
      src/stream_parser.cpp
      src/tokenizer_service.cpp
      src/tokenizer_thread_pool.cpp
      src/sqlite_persistence.cpp
      src/tracing_service.cpp
    )

    target_include_directories(ranvier_server PRIVATE ${SQLITE3_INCLUDE_DIRS})

    # Debug metrics configuration (optional)
    # Enables 8 additional gossip metrics for crypto offload debugging
    if(RANVIER_DEBUG_METRICS)
        target_compile_definitions(ranvier_server PRIVATE RANVIER_DEBUG_METRICS)
        message(STATUS "Gossip debug metrics enabled")
    endif()

    # OpenTelemetry configuration (optional)
    # Note: We always use the Zipkin exporter because the OTLP exporter (which uses
    # protobuf) has static initialization that blocks before main() runs.
    # Zipkin exports to the same collectors (Jaeger, Tempo, etc.) via HTTP/JSON.
    if(WITH_TELEMETRY)
        set(OTEL_LIBS
            opentelemetry_trace
            opentelemetry_exporter_zipkin_trace
            opentelemetry_resources
            opentelemetry_common
        )
        message(STATUS "OpenTelemetry: Using Zipkin exporter (OTLP disabled due to protobuf static init issues)")
    else()
        target_compile_definitions(ranvier_server PRIVATE RANVIER_NO_TELEMETRY)
        set(OTEL_LIBS "")
    endif()

    target_link_libraries(ranvier_server
      PRIVATE
      Seastar::seastar
      tokenizers_cpp
      ${SQLITE3_LIBRARIES}
      yaml-cpp::yaml-cpp
      rapidjson_interface
      absl::flat_hash_map
      absl::flat_hash_set
      absl::inlined_vector
      OpenSSL::SSL
      OpenSSL::Crypto
      ${OTEL_LIBS}
    )

    # Copy the assets folder to the build directory automatically
    file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})
else()
    message(STATUS "Seastar not found - building unit tests only")
endif()

# =============================================================================
# Unit Tests
# =============================================================================
enable_testing()
include(GoogleTest)

# RadixTree Unit Tests (Pure C++, no Seastar dependency)
add_executable(radix_tree_test
  tests/unit/radix_tree_test.cpp
  src/node_slab.cpp
)
target_include_directories(radix_tree_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(radix_tree_test
  PRIVATE
  GTest::gtest_main
  absl::inlined_vector
)
gtest_discover_tests(radix_tree_test)

# ChatTemplate Unit Tests (Pure C++, header-only)
add_executable(chat_template_test
  tests/unit/chat_template_test.cpp
)
target_include_directories(chat_template_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(chat_template_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(chat_template_test)

# BoundaryDetector Unit Tests (Pure C++, header-only)
add_executable(boundary_detector_test
  tests/unit/boundary_detector_test.cpp
)
target_include_directories(boundary_detector_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(boundary_detector_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(boundary_detector_test)

# IntentClassifier Unit Tests (Pure C++, no Seastar)
add_executable(intent_classifier_test
  tests/unit/intent_classifier_test.cpp
  src/intent_classifier.cpp
)
target_include_directories(intent_classifier_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(intent_classifier_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(intent_classifier_test)

# TextValidator Unit Tests (Pure C++, header-only)
add_executable(text_validator_test
  tests/unit/text_validator_test.cpp
)
target_include_directories(text_validator_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(text_validator_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(text_validator_test)

# ParseUtils Unit Tests (Pure C++, header-only)
add_executable(parse_utils_test
  tests/unit/parse_utils_test.cpp
)
target_include_directories(parse_utils_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(parse_utils_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(parse_utils_test)

# Logging Unit Tests (Pure C++, uses stubs for Seastar/fmt)
# Tests generate_request_id format and header extraction helpers.
add_executable(logging_test
  tests/unit/logging_test.cpp
)
target_include_directories(logging_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/stubs
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(logging_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(logging_test)

# ShardLoadMetrics Unit Tests (Pure C++, uses stubs for Seastar)
# Tests RAII guards, atomic counters, load scoring, and snapshot semantics.
add_executable(shard_load_metrics_test
  tests/unit/shard_load_metrics_test.cpp
)
target_include_directories(shard_load_metrics_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/stubs
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(shard_load_metrics_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(shard_load_metrics_test)

# MetricsService Unit Tests (Pure C++, uses stubs for Seastar metrics)
# Tests histogram buckets, bounded containers (Rule #4), cache-hit-ratio,
# prefix skip averaging, and counter recording.
add_executable(metrics_service_test
  tests/unit/metrics_service_test.cpp
)
target_include_directories(metrics_service_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/stubs
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(metrics_service_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(metrics_service_test)

# NodeSlab Unit Tests (Pure C++, no Seastar dependency)
# Tests slab allocator: allocate/free round-trips, free-list integrity,
# peak tracking, auto-growth, and make_node factory methods.
add_executable(node_slab_test
  tests/unit/node_slab_test.cpp
  src/node_slab.cpp
)
target_include_directories(node_slab_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(node_slab_test
  PRIVATE
  GTest::gtest_main
  absl::inlined_vector
)
gtest_discover_tests(node_slab_test)

# MpscRingBuffer Unit Tests (Pure C++, no Seastar dependency)
# Tests lock-free MPSC ring buffer: push/pop, drain, wraparound, capacity
# rounding, move-only types, variant types, multi-producer correctness.
add_executable(mpsc_ring_buffer_test
  tests/unit/mpsc_ring_buffer_test.cpp
)
target_include_directories(mpsc_ring_buffer_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mpsc_ring_buffer_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(mpsc_ring_buffer_test)

# CrossShardRequest Unit Tests (Pure C++, uses stubs for Seastar)
# Tests validation limits, force_local_allocation, and result factories.
add_executable(cross_shard_request_test
  tests/unit/cross_shard_request_test.cpp
)
target_include_directories(cross_shard_request_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/stubs
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(cross_shard_request_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(cross_shard_request_test)

# TracingService Unit Tests (W3C traceparent parsing only, no OTel)
# Compiled with RANVIER_NO_TELEMETRY; real parse logic provided in test file.
add_executable(tracing_service_test
  tests/unit/tracing_service_test.cpp
)
target_compile_definitions(tracing_service_test PRIVATE RANVIER_NO_TELEMETRY)
target_include_directories(tracing_service_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tracing_service_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(tracing_service_test)

# Persistence Unit Tests (Pure C++, no Seastar dependency)
add_executable(persistence_test
  tests/unit/persistence_test.cpp
  src/sqlite_persistence.cpp
)
target_include_directories(persistence_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${SQLITE3_INCLUDE_DIRS})
target_link_libraries(persistence_test
  PRIVATE
  GTest::gtest_main
  ${SQLITE3_LIBRARIES}
)
gtest_discover_tests(persistence_test)

# Config Unit Tests (Pure C++, no Seastar dependency)
add_executable(config_test
  tests/unit/config_test.cpp
  src/config_loader.cpp
)
target_include_directories(config_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(config_test
  PRIVATE
  GTest::gtest_main
  yaml-cpp::yaml-cpp
)
gtest_discover_tests(config_test)

# Priority Tier Unit Tests (Pure C++, no Seastar dependency)
# Tests PriorityLevel enum, parsing, cascade logic, and PriorityTierConfig.
add_executable(priority_tier_test
  tests/unit/priority_tier_test.cpp
)
target_include_directories(priority_tier_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(priority_tier_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(priority_tier_test)

# Request Scheduler Unit Tests (No Seastar dependency, uses absl::flat_hash_map)
add_executable(request_scheduler_test
  tests/unit/request_scheduler_test.cpp
)
target_include_directories(request_scheduler_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(request_scheduler_test
  PRIVATE
  absl::flat_hash_map
  GTest::gtest_main
)
gtest_discover_tests(request_scheduler_test)

# Gossip Protocol Unit Tests (Pure C++, no Seastar dependency)
# Tests for gossip wire format, packet serialization/deserialization
add_executable(gossip_protocol_test
  tests/unit/gossip_protocol_test.cpp
)
target_include_directories(gossip_protocol_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gossip_protocol_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(gossip_protocol_test)

# Gossip Transport Ownership Pattern Tests (Pure C++, no Seastar dependency)
# Tests verifying shard-local ownership patterns (Rule #0: no std::shared_ptr)
add_executable(gossip_transport_ownership_test
  tests/unit/gossip_transport_ownership_test.cpp
)
target_include_directories(gossip_transport_ownership_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gossip_transport_ownership_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(gossip_transport_ownership_test)

# Route Batching Unit Tests (Pure C++, no Seastar dependency)
# Tests for route batching data structures and behavior
add_executable(route_batching_test
  tests/unit/route_batching_test.cpp
)
target_include_directories(route_batching_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(route_batching_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(route_batching_test)

# K8s Discovery Unit Tests (Pure C++, no Seastar dependency)
# Tests for K8s EndpointSlice parsing and JSON helpers
add_executable(k8s_discovery_test
  tests/unit/k8s_discovery_test.cpp
)
target_include_directories(k8s_discovery_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(k8s_discovery_test
  PRIVATE
  GTest::gtest_main
  rapidjson_interface
  absl::flat_hash_set
)
gtest_discover_tests(k8s_discovery_test)

# Local Discovery Unit Tests (Pure C++, no Seastar dependency)
# Tests for server type detection and model response parsing
add_executable(local_discovery_test
  tests/unit/local_discovery_test.cpp
)
target_include_directories(local_discovery_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(local_discovery_test
  PRIVATE
  GTest::gtest_main
  rapidjson_interface
)
gtest_discover_tests(local_discovery_test)

# Request Rewriter Unit Tests (Pure C++, no Seastar dependency)
# Tests for JSON request parsing and token ID injection
add_executable(request_rewriter_test
  tests/unit/request_rewriter_test.cpp
)
target_include_directories(request_rewriter_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(request_rewriter_test
  PRIVATE
  GTest::gtest_main
  rapidjson_interface
)
gtest_discover_tests(request_rewriter_test)

# Prefix Affinity Unit Tests (Pure C++, no Seastar dependency)
# Tests for prefix-affinity routing hash function and backend selection
add_executable(prefix_affinity_test
  tests/unit/prefix_affinity_test.cpp
)
target_include_directories(prefix_affinity_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(prefix_affinity_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(prefix_affinity_test)

# Load-Aware Routing Unit Tests (Pure C++, no Seastar dependency)
# Tests for load-aware backend selection algorithm and BackendRequestGuard
add_executable(load_aware_routing_test
  tests/unit/load_aware_routing_test.cpp
)
target_include_directories(load_aware_routing_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(load_aware_routing_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(load_aware_routing_test)

# Quorum / Split-Brain Detection Unit Tests (Pure C++, no Seastar dependency)
# Tests for quorum calculation, state transitions, and peer liveness with TestClock
add_executable(quorum_test
  tests/unit/quorum_test.cpp
)
target_include_directories(quorum_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests/unit
)
target_link_libraries(quorum_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(quorum_test)

# Circuit Breaker Unit Tests (Pure C++, no Seastar dependency)
# Tests for circuit breaker pattern, remove_circuit cleanup (Rule #4), and timing with TestClock
add_executable(circuit_breaker_test
  tests/unit/circuit_breaker_test.cpp
)
target_include_directories(circuit_breaker_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests/unit
)
target_link_libraries(circuit_breaker_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(circuit_breaker_test)

# Rate Limiter Unit Tests (Pure C++, no Seastar dependency)
# Tests for token bucket rate limiter, MAX_BUCKETS bound (Rule #4), and timing with TestClock
add_executable(rate_limiter_test
  tests/unit/rate_limiter_test.cpp
)
target_include_directories(rate_limiter_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests/unit
)
target_link_libraries(rate_limiter_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(rate_limiter_test)

# Proxy Retry Policy Unit Tests (Pure C++, no Seastar dependency)
# Tests for retry/backoff/fallback decisions, connection error classification,
# fallback backend selection, and backpressure policy with TestClock
add_executable(proxy_retry_policy_test
  tests/unit/proxy_retry_policy_test.cpp
)
target_include_directories(proxy_retry_policy_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests/unit
)
target_link_libraries(proxy_retry_policy_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(proxy_retry_policy_test)

# ShardLoadMetrics Concurrency Stress Tests (Pure C++, uses stubs for Seastar)
# Tests concurrent atomic increment/decrement, RAII guard balance, and snapshot under contention
add_executable(shard_load_metrics_concurrency_test
  tests/unit/shard_load_metrics_concurrency_test.cpp
)
target_include_directories(shard_load_metrics_concurrency_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests/stubs
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(shard_load_metrics_concurrency_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(shard_load_metrics_concurrency_test)

# Rate Limiter Concurrency Stress Tests (Pure C++, no Seastar dependency)
# Tests atomic overflow counter under MAX_BUCKETS pressure, concurrent allow() with unique IPs,
# and concurrent cleanup/config reads with TestClock
add_executable(rate_limiter_concurrency_test
  tests/unit/rate_limiter_concurrency_test.cpp
)
target_include_directories(rate_limiter_concurrency_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/tests/unit
)
target_link_libraries(rate_limiter_concurrency_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(rate_limiter_concurrency_test)

# ShardedConfig Unit Tests (Requires Seastar types for header includes)
# Tests ShardedConfig wrapper class for per-core config distribution
if(Seastar_FOUND)
    add_executable(sharded_config_test
      tests/unit/sharded_config_test.cpp
      src/config_loader.cpp
    )
    target_include_directories(sharded_config_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(sharded_config_test
      PRIVATE
      Seastar::seastar
      yaml-cpp::yaml-cpp
      GTest::gtest_main
    )
    gtest_discover_tests(sharded_config_test)

    # CryptoOffloader Unit Tests (Requires Seastar types)
    # Tests adaptive crypto offloading decision logic and configuration
    add_executable(crypto_offloader_test
      tests/unit/crypto_offloader_test.cpp
      src/crypto_offloader.cpp
    )
    target_include_directories(crypto_offloader_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(crypto_offloader_test
      PRIVATE
      Seastar::seastar
      GTest::gtest_main
    )
    gtest_discover_tests(crypto_offloader_test)

    # DtlsContext Unit Tests (Requires Seastar types for header includes)
    # Tests memory-based certificate loading (load_certs_from_memory) without reactor
    add_executable(dtls_context_test
      tests/unit/dtls_context_test.cpp
      src/dtls_context.cpp
    )
    target_include_directories(dtls_context_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(dtls_context_test
      PRIVATE
      Seastar::seastar
      OpenSSL::SSL
      OpenSSL::Crypto
      GTest::gtest_main
    )
    gtest_discover_tests(dtls_context_test)

# ConnectionPool Unit Tests (Requires Seastar types)
    # Tests pool management logic: per-host limits, global limits, idle timeout
    # eviction, max-age TTL enforcement, and half-open detection with TestClock.
    add_executable(connection_pool_test
      tests/unit/connection_pool_test.cpp
    )
    target_include_directories(connection_pool_test PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/unit
    )
    target_link_libraries(connection_pool_test
      PRIVATE
      Seastar::seastar
      absl::flat_hash_map
      GTest::gtest_main
    )
    gtest_discover_tests(connection_pool_test)

# StreamParser Unit Tests (Requires Seastar types)
    add_executable(stream_parser_test
      tests/unit/stream_parser_test.cpp
      src/stream_parser.cpp
    )
    target_include_directories(stream_parser_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(stream_parser_test
      PRIVATE
      Seastar::seastar
      GTest::gtest_main
    )
    gtest_discover_tests(stream_parser_test)

    # AsyncPersistence Unit Tests (Requires Seastar types for header includes)
    # Tests queue behavior, backpressure, and configuration without running Seastar event loop
    # Uses gmock for MockPersistenceStore (dependency isolation)
    add_executable(async_persistence_test
      tests/unit/async_persistence_test.cpp
      src/async_persistence.cpp
      src/sqlite_persistence.cpp
    )
    target_include_directories(async_persistence_test PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/unit
      ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(async_persistence_test
      PRIVATE
      Seastar::seastar
      ${SQLITE3_LIBRARIES}
      GTest::gtest_main
      GTest::gmock
    )
    gtest_discover_tests(async_persistence_test)

    # Application Unit Tests (Requires Seastar types for header includes)
    # Tests Application state machine, config builders, and accessors
    add_executable(application_test
      tests/unit/application_test.cpp
      src/application.cpp
      src/config_loader.cpp
      src/crypto_offloader.cpp
      src/gossip_consensus.cpp
      src/gossip_protocol.cpp
      src/gossip_service.cpp
      src/gossip_transport.cpp
      src/node_slab.cpp
      src/router_service.cpp
      src/health_service.cpp
      src/k8s_discovery_service.cpp
      src/local_discovery.cpp
      src/http_controller.cpp
      src/intent_classifier.cpp
      src/async_persistence.cpp
      src/sqlite_persistence.cpp
      src/stream_parser.cpp
      src/dtls_context.cpp
      src/tokenizer_service.cpp
      src/tokenizer_thread_pool.cpp
      src/tracing_service.cpp
    )
    target_include_directories(application_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${SQLITE3_INCLUDE_DIRS})
    if(NOT WITH_TELEMETRY)
        target_compile_definitions(application_test PRIVATE RANVIER_NO_TELEMETRY)
    endif()
    if(RANVIER_DEBUG_METRICS)
        target_compile_definitions(application_test PRIVATE RANVIER_DEBUG_METRICS)
    endif()
    target_link_libraries(application_test
      PRIVATE
      Seastar::seastar
      ${SQLITE3_LIBRARIES}
      yaml-cpp::yaml-cpp
      rapidjson_interface
      tokenizers_cpp
      absl::flat_hash_map
      absl::flat_hash_set
      absl::inlined_vector
      OpenSSL::SSL
      OpenSSL::Crypto
      ${OTEL_LIBS}
      GTest::gtest_main
    )
    gtest_discover_tests(application_test)

    # TokenizationCache Unit Tests (Requires Seastar types for header includes)
    # Tests LRU cache for tokenization results (Rule #4: bounded container)
    add_executable(tokenization_cache_test
      tests/unit/tokenization_cache_test.cpp
      src/tokenizer_service.cpp
      src/tokenizer_thread_pool.cpp
    )
    target_include_directories(tokenization_cache_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tokenization_cache_test
      PRIVATE
      Seastar::seastar
      tokenizers_cpp
      absl::flat_hash_map
      GTest::gtest_main
    )
    gtest_discover_tests(tokenization_cache_test)

    # TokenizerThreadPool Unit Tests (Requires Seastar types for header includes)
    # Tests dedicated thread pool for non-blocking FFI offload (Rule #4, #11)
    add_executable(tokenizer_thread_pool_test
      tests/unit/tokenizer_thread_pool_test.cpp
      src/tokenizer_thread_pool.cpp
    )
    target_include_directories(tokenizer_thread_pool_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tokenizer_thread_pool_test
      PRIVATE
      Seastar::seastar
      tokenizers_cpp
      GTest::gtest_main
    )
    gtest_discover_tests(tokenizer_thread_pool_test)

    # TokenizerService Unit Tests (Requires Seastar types for semaphore, metrics)
    # Tests semaphore-gated local tokenization fallback (Backlog 21.2),
    # counter bookkeeping, configuration, and Seastar API contract assumptions.
    add_executable(tokenizer_service_test
      tests/unit/tokenizer_service_test.cpp
      src/tokenizer_service.cpp
      src/tokenizer_thread_pool.cpp
    )
    target_include_directories(tokenizer_service_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tokenizer_service_test
      PRIVATE
      Seastar::seastar
      tokenizers_cpp
      absl::flat_hash_map
      GTest::gtest_main
    )
    gtest_discover_tests(tokenizer_service_test)

    # RouterService Integration Tests (Seastar-dependent)
    # Tests the core routing brain: route learning, TTL, backend lifecycle, draining mode.
    # Links against multiple src/*.cpp files for full RouterService + GossipService compilation.
    add_executable(router_service_test
      tests/unit/router_service_test.cpp
      src/router_service.cpp
      src/node_slab.cpp
      src/gossip_service.cpp
      src/gossip_protocol.cpp
      src/gossip_consensus.cpp
      src/gossip_transport.cpp
      src/crypto_offloader.cpp
      src/dtls_context.cpp
    )
    target_include_directories(router_service_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(router_service_test
      PRIVATE
      Seastar::seastar
      absl::flat_hash_map
      absl::flat_hash_set
      absl::inlined_vector
      OpenSSL::SSL
      OpenSSL::Crypto
      GTest::gtest_main
    )
    gtest_discover_tests(router_service_test)

    # AsyncPersistence Concurrency Stress Tests (Requires Seastar types)
    # Tests concurrent enqueue from multiple threads, backpressure enforcement,
    # queue depth accounting, and ClearAll under contention
    add_executable(async_persistence_concurrency_test
      tests/unit/async_persistence_concurrency_test.cpp
      src/async_persistence.cpp
      src/sqlite_persistence.cpp
    )
    target_include_directories(async_persistence_concurrency_test PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/unit
      ${SQLITE3_INCLUDE_DIRS}
    )
    target_link_libraries(async_persistence_concurrency_test
      PRIVATE
      Seastar::seastar
      ${SQLITE3_LIBRARIES}
      GTest::gtest_main
      GTest::gmock
    )
    gtest_discover_tests(async_persistence_concurrency_test)

    # TokenizerThreadPool Concurrency Stress Tests (Requires Seastar types)
    # Tests concurrent SPSC queue submit and stat reads, config accessor thread safety,
    # and queue boundary behavior under contention
    add_executable(tokenizer_thread_pool_concurrency_test
      tests/unit/tokenizer_thread_pool_concurrency_test.cpp
      src/tokenizer_thread_pool.cpp
    )
    target_include_directories(tokenizer_thread_pool_concurrency_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tokenizer_thread_pool_concurrency_test
      PRIVATE
      Seastar::seastar
      tokenizers_cpp
      GTest::gtest_main
    )
    gtest_discover_tests(tokenizer_thread_pool_concurrency_test)
endif()
