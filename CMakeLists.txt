cmake_minimum_required(VERSION 3.24)
project(ranvier)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Seastar Configuration
find_package(Seastar REQUIRED)

# Tokenizers-CPP Configuration (Downloads it automatically)
include(FetchContent)
FetchContent_Declare(
  tokenizers_cpp
  GIT_REPOSITORY https://github.com/mlc-ai/tokenizers-cpp
  GIT_TAG        main
)
FetchContent_MakeAvailable(tokenizers_cpp)

# Limit the Noise: Stop the compiler from printing 1,000 lines of "Candidate function not found" when it fails on the first one
add_compile_options(-fmax-errors=3)

# Define the Executable
add_executable(ranvier_server
  src/main.cpp
  src/http_controller.cpp
  src/router_service.cpp
  src/tokenizer_service.cpp
)

# Link Libraries
target_link_libraries(ranvier_server
  PRIVATE
  Seastar::seastar
  tokenizers_cpp
)

# Copy the assets folder to the build directory automatically
file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})

