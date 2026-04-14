cmake_minimum_required(VERSION 3.20)
project(MatchingEngine VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 生成 compile_commands.json，供 clangd 语言服务器使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项：Release 模式下开启 O2 优化
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

# ── Abseil（系统安装：apt install libabsl-dev） ───────────────────────────────
find_package(absl REQUIRED)

# ── 第三方依赖（FetchContent 自动下载） ──────────────────────────────────────
include(FetchContent)

# Google Test
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# Google Benchmark
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest benchmark)

# ── 核心静态库 ────────────────────────────────────────────────────────────────
# 所有可复用逻辑编译为静态库，main / tests / benchmarks 统一链接
add_library(matching_engine_lib STATIC
  src/order_book.cpp
  src/matching_engine.cpp
  src/feed_simulator.cpp
)

target_include_directories(matching_engine_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(matching_engine_lib PRIVATE
  -Wall -Wextra -Wpedantic
)

target_link_libraries(matching_engine_lib PUBLIC
  absl::flat_hash_map
)

# ── 主程序可执行文件 ──────────────────────────────────────────────────────────
add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_lib)

# ── 测试套件 ──────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_order_book       tests/test_order_book.cpp)
add_executable(test_spsc_ring_buffer tests/test_spsc_ring_buffer.cpp)
add_executable(test_memory_pool      tests/test_memory_pool.cpp)

foreach(test_target
    test_order_book
    test_spsc_ring_buffer
    test_memory_pool)
  target_link_libraries(${test_target} PRIVATE
    matching_engine_lib
    GTest::gtest_main
  )
  add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()

# ── Benchmark 套件 ────────────────────────────────────────────────────────────
add_executable(bench_order_book       benchmarks/bench_order_book.cpp)
add_executable(bench_spsc_ring_buffer benchmarks/bench_spsc_ring_buffer.cpp)

foreach(bench_target
    bench_order_book
    bench_spsc_ring_buffer)
  target_link_libraries(${bench_target} PRIVATE
    matching_engine_lib
    benchmark::benchmark_main
  )
endforeach()
