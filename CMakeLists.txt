cmake_minimum_required(VERSION 3.24)
project(ranvier)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# FetchContent for external dependencies
include(FetchContent)

# Google Test Configuration (Always available for unit tests)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest
  GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# yaml-cpp Configuration (for config file parsing)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp
  GIT_TAG        0.8.0
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# Abseil Configuration (High-performance containers)
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp
  GIT_TAG        20240116.1
)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(abseil-cpp)

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
target_include_directories(rapidjson_interface INTERFACE ${rapidjson_SOURCE_DIR}/include)

# Option to disable OpenTelemetry (avoids static initialization that can block)
option(WITH_TELEMETRY "Enable OpenTelemetry distributed tracing" ON)

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
    FetchContent_Declare(
      tokenizers_cpp
      GIT_REPOSITORY https://github.com/mlc-ai/tokenizers-cpp
      GIT_TAG        main
    )
    FetchContent_MakeAvailable(tokenizers_cpp)

    # Limit the Noise
    add_compile_options(-fmax-errors=3)

    # Define the Main Server Executable
    add_executable(ranvier_server
      src/main.cpp
      src/application.cpp
      src/async_persistence.cpp
      src/dtls_context.cpp
      src/gossip_service.cpp
      src/health_service.cpp
      src/http_controller.cpp
      src/k8s_discovery_service.cpp
      src/router_service.cpp
      src/stream_parser.cpp
      src/tokenizer_service.cpp
      src/sqlite_persistence.cpp
      src/tracing_service.cpp
    )

    target_include_directories(ranvier_server PRIVATE ${SQLITE3_INCLUDE_DIRS})

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
)
target_include_directories(radix_tree_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(radix_tree_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(radix_tree_test)

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
)
target_include_directories(config_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(config_test
  PRIVATE
  GTest::gtest_main
  yaml-cpp::yaml-cpp
)
gtest_discover_tests(config_test)

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

# Quorum / Split-Brain Detection Unit Tests (Pure C++, no Seastar dependency)
# Tests for quorum calculation and state transitions
add_executable(quorum_test
  tests/unit/quorum_test.cpp
)
target_include_directories(quorum_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(quorum_test
  PRIVATE
  GTest::gtest_main
)
gtest_discover_tests(quorum_test)

# StreamParser Unit Tests (Requires Seastar types)
if(Seastar_FOUND)
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
    add_executable(async_persistence_test
      tests/unit/async_persistence_test.cpp
      src/async_persistence.cpp
      src/sqlite_persistence.cpp
    )
    target_include_directories(async_persistence_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${SQLITE3_INCLUDE_DIRS})
    target_link_libraries(async_persistence_test
      PRIVATE
      Seastar::seastar
      ${SQLITE3_LIBRARIES}
      GTest::gtest_main
    )
    gtest_discover_tests(async_persistence_test)
endif()
