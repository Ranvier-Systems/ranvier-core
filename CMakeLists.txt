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
      src/health_service.cpp
      src/http_controller.cpp
      src/router_service.cpp
      src/stream_parser.cpp
      src/tokenizer_service.cpp
      src/sqlite_persistence.cpp
    )

    target_include_directories(ranvier_server PRIVATE ${SQLITE3_INCLUDE_DIRS})
    target_link_libraries(ranvier_server
      PRIVATE
      Seastar::seastar
      tokenizers_cpp
      ${SQLITE3_LIBRARIES}
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
endif()
