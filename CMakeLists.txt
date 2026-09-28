cmake_minimum_required(VERSION 3.20)
project(RateLimiter VERSION 1.0.0 LANGUAGES CXX)

# Enforce strict C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Discover POSIX threads support (-pthread)
find_package(Threads REQUIRED)

# Primary Target: ratelimiter (Header-only library)
add_library(ratelimiter INTERFACE)

# Define include directories for build-tree vs installed consumption
target_include_directories(ratelimiter INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Propagate threading dependency to all consumers
target_link_libraries(ratelimiter INTERFACE
    Threads::Threads
)

# Interface Target: Strict compiler diagnostics and sanitizers
add_library(compiler_flags INTERFACE)

target_compile_options(compiler_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wnon-virtual-dtor
)

# Option to detect concurrent data races via ThreadSanitizer
option(ENABLE_TSAN "Enable ThreadSanitizer for detecting data races" OFF)
if(ENABLE_TSAN)
    target_compile_options(compiler_flags INTERFACE -fsanitize=thread -g -O1)
    target_link_options(compiler_flags INTERFACE -fsanitize=thread)
endif()

# Executables: Unit Tests, Benchmarks, and Examples
enable_testing()

# 1. Unit & Concurrency Test Suite
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_limiter.cpp")
    add_executable(test_limiter tests/test_limiter.cpp)
    target_link_libraries(test_limiter PRIVATE ratelimiter compiler_flags)
    add_test(NAME RateLimiterTests COMMAND test_limiter)
endif()

# 2. High-Throughput Benchmark Suite
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/throughput_bench.cpp")
    add_executable(throughput_bench benchmarks/throughput_bench.cpp)
    target_link_libraries(throughput_bench PRIVATE ratelimiter compiler_flags)

    # Enable aggressive hardware optimizations when TSan is disabled
    if(NOT ENABLE_TSAN)
        target_compile_options(throughput_bench PRIVATE -O3 -march=native)
    endif()
endif()

# 3. Server Stub / Integration Example
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/server_stub.cpp")
    add_executable(server_stub examples/server_stub.cpp)
    target_link_libraries(server_stub PRIVATE ratelimiter compiler_flags)
endif()