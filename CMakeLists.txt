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
# Debug：ASan + UBSan
#
# UBSan 的 null 与 function 两个子检查必须排除。GCC 15 与 abseil 的 SwissTable
# 实现在这两个检查下不兼容：它们会破坏 hash_policy_traits::get_hash_slot_fn()
# 中"函数指针 == nullptr"的常量表达式求值，使 absl::flat_hash_map 无法编译：
#
#   error: '(TypeErasedApplyToSlotFn<...> == 0)' is not a constant expression
#
# 已验证这不是发行版打包问题：系统包 20260107 与上游 20260817.0 表现一致；
# 也不是优化级别问题：-O0/-O1/-Og/-O2 不带 sanitizer 时均可编译。
#
# 采用显式列举而非 "-fsanitize=undefined -fno-sanitize=null,function"，
# 因为 GCC 不支持从 undefined 这个检查组中撤销单个子检查（实测无效）。
#
# 覆盖面损失（可接受且有替代）：
#   null     —— 空指针解引用仍由 ASan 捕获（零页不可映射）
#   function —— 检查间接调用的类型不匹配；热路径自 d941a21 起已由
#               std::function 改为模板静态多态，几乎不存在间接调用
set(ME_UBSAN_CHECKS
    signed-integer-overflow bounds alignment shift return vla-bound
    float-divide-by-zero integer-divide-by-zero unreachable bool enum
    vptr object-size pointer-overflow builtin)
list(JOIN ME_UBSAN_CHECKS "," ME_UBSAN_CHECKS_CSV)

set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,${ME_UBSAN_CHECKS_CSV}")

# 构建类型名的大写形式，用于索引 CMAKE_CXX_FLAGS_<CONFIG>。
# 必须在 CMAKE_BUILD_TYPE 默认值和各 CONFIG 的 flags 都确定之后求值。
string(TOUPPER "${CMAKE_BUILD_TYPE}" ME_BUILD_TYPE_UPPER)

# ── 第三方依赖 ────────────────────────────────────────────────────────────────
include(FetchContent)

# 第三方头文件以 SYSTEM include 方式引入，避免其 GNU 扩展（如 abseil 的
# __int128）在本项目的 -Wpedantic 下产生大量与自身代码无关的警告。
# FetchContent 的 SYSTEM 关键字需要 CMake 3.25+；更低版本降级为普通 include。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
  set(ME_FC_SYSTEM SYSTEM)
else()
  set(ME_FC_SYSTEM "")
endif()

# Abseil（absl::flat_hash_map）
#   优先使用系统安装（Debian/Ubuntu: sudo apt install libabsl-dev），
#   未安装时回退到源码构建。两条路径锁定同一版本，行为一致。
#   注意：源码构建首次配置耗时数分钟，且会编译整个 abseil。
#   源码构建锁定的版本（abseil 的 CMake 包版本号格式不稳定，
#   故不对系统安装做精确版本匹配，仅在源码路径上锁定）
set(ME_ABSL_FALLBACK_VERSION 20260107.0)

# sanitizer 构建必须从源码编译 abseil，不能用系统预编译库。
#
# 原因是 ABI 不匹配：abseil 的 SwissTable 会根据 ABSL_HAVE_ADDRESS_SANITIZER
# 改变 small-object-optimization 的布局。系统 libabsl.so 未开 sanitizer，
# 而由头文件在本项目中实例化的模板代码开了，两者对 SOO capacity 的认知不一致，
# 运行时在 raw_hash_set.cc 触发 abseil 内部断言：
#
#   Assertion `(kMode != ResizeNonSooMode::kGuaranteedEmpty ||
#               old_capacity == policy.soo_capacity())' failed.
#
# 该断言在 OrderBook 构造（order_index_.reserve）时即触发，Debug 下全部
# OrderBook/MatchingEngine 测试无法运行。Release（无 sanitizer）不受影响。
#
# 这符合 abseil 的官方要求：使用方必须与 abseil 本身用相同的编译选项构建。
set(ME_ABSL_FROM_SOURCE OFF)
if(CMAKE_CXX_FLAGS_${ME_BUILD_TYPE_UPPER} MATCHES "-fsanitize=")
  set(ME_ABSL_FROM_SOURCE ON)
endif()

if(NOT ME_ABSL_FROM_SOURCE)
  find_package(absl QUIET)
else()
  message(STATUS "Abseil: sanitizer build detected -> building from source "
                 "(system libabsl is ABI-incompatible under sanitizers)")
endif()

if(absl_FOUND)
  message(STATUS "Abseil: using system installation ${absl_VERSION} (${absl_DIR})")
else()
  message(STATUS "Abseil: not found on system -> building ${ME_ABSL_FALLBACK_VERSION} from source.")
  message(STATUS "        Install 'libabsl-dev' to skip this and speed up configuration.")
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    abseil-cpp
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG        ${ME_ABSL_FALLBACK_VERSION}
    GIT_SHALLOW    TRUE
    ${ME_FC_SYSTEM}
  )
  FetchContent_MakeAvailable(abseil-cpp)
endif()

# Google Test
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
  ${ME_FC_SYSTEM}
)
# Google Benchmark
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3
  GIT_SHALLOW    TRUE
  ${ME_FC_SYSTEM}
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

# ── 线程库（多线程流水线依赖） ────────────────────────────────────────────────
find_package(Threads REQUIRED)

target_link_libraries(matching_engine_lib PUBLIC
  absl::flat_hash_map
  Threads::Threads
)

# ── 主程序可执行文件 ──────────────────────────────────────────────────────────
add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_lib)

# ── 测试套件 ──────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_order_book       tests/test_order_book.cpp)
add_executable(test_spsc_ring_buffer tests/test_spsc_ring_buffer.cpp)
add_executable(test_memory_pool      tests/test_memory_pool.cpp)
add_executable(test_matching_engine  tests/test_matching_engine.cpp)

foreach(test_target
    test_order_book
    test_spsc_ring_buffer
    test_memory_pool
    test_matching_engine)
  target_link_libraries(${test_target} PRIVATE
    matching_engine_lib
    GTest::gtest_main
  )
  add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()

# ── Benchmark 套件 ────────────────────────────────────────────────────────────
add_executable(bench_order_book       benchmarks/bench_order_book.cpp)
add_executable(bench_spsc_ring_buffer benchmarks/bench_spsc_ring_buffer.cpp)
add_executable(bench_matching_engine  benchmarks/bench_matching_engine.cpp)
add_executable(bench_baseline_mutex   benchmarks/bench_baseline_mutex.cpp)

foreach(bench_target
    bench_order_book
    bench_spsc_ring_buffer
    bench_matching_engine
    bench_baseline_mutex)
  target_link_libraries(${bench_target} PRIVATE
    matching_engine_lib
    benchmark::benchmark_main
  )
endforeach()
