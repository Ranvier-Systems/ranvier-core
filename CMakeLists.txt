cmake_minimum_required(VERSION 3.24)
project(ranvier)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Seastar Configuration
find_package(Seastar REQUIRED)

# 2. Tokenizers-CPP Configuration (Downloads it automatically)
include(FetchContent)
FetchContent_Declare(
  tokenizers_cpp
  GIT_REPOSITORY https://github.com/mlc-ai/tokenizers-cpp
  GIT_TAG        main
)
FetchContent_MakeAvailable(tokenizers_cpp)

# 3. Define the Executable
add_executable(ranvier_server src/main.cpp)

# 4. Link Libraries
target_link_libraries(ranvier_server
  PRIVATE
  Seastar::seastar
  tokenizers_cpp
)
