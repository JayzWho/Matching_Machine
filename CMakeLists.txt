cmake_minimum_required(VERSION 3.20)
project(MatchingEngine VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
# Debug: ASan plus an explicit list of UBSan checks.
#
# UBSan's null and function checks are left out. Under GCC 15 they break the constant
# evaluation of a function-pointer comparison in Abseil's hash_policy_traits, and
# absl::flat_hash_map no longer compiles:
#
#   error: '(TypeErasedApplyToSlotFn<...> == 0)' is not a constant expression
#
# The system package 20260107 and upstream 20260817.0 behave the same, and every
# optimization level compiles without sanitizers. GCC cannot remove single checks from
# -fsanitize=undefined, hence the list. ASan still catches null dereferences, and the
# hot path has used templates instead of std::function since f2abe06, so the function
# check would have little to cover.
set(ME_UBSAN_CHECKS
    signed-integer-overflow bounds alignment shift return vla-bound
    float-divide-by-zero integer-divide-by-zero unreachable bool enum
    vptr object-size pointer-overflow builtin)
list(JOIN ME_UBSAN_CHECKS "," ME_UBSAN_CHECKS_CSV)

set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,${ME_UBSAN_CHECKS_CSV}")

# Indexes CMAKE_CXX_FLAGS_<CONFIG>; evaluated once the build type and flags are set.
string(TOUPPER "${CMAKE_BUILD_TYPE}" ME_BUILD_TYPE_UPPER)

# ── Dependencies ─────────────────────────────────────────────────────────────
include(FetchContent)

# Third-party headers are SYSTEM includes so that their GNU extensions, such as Abseil's
# __int128, do not warn under -Wpedantic. FetchContent takes SYSTEM from CMake 3.25.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
  set(ME_FC_SYSTEM SYSTEM)
else()
  set(ME_FC_SYSTEM "")
endif()

# Abseil: the system package (libabsl-dev) when present, otherwise this version built
# from source, which takes several minutes on the first configure. Only the source
# path pins a version, since Abseil's package version format is not stable.
set(ME_ABSL_FALLBACK_VERSION 20260107.0)

# Sanitizer builds compile Abseil from source. Its SwissTable changes its small-object
# layout under ABSL_HAVE_ADDRESS_SANITIZER, so templates instantiated here with ASan
# disagree with an uninstrumented system libabsl.so, and the first reserve() in
# OrderBook aborts in raw_hash_set.cc:
#
#   Assertion `(kMode != ResizeNonSooMode::kGuaranteedEmpty ||
#               old_capacity == policy.soo_capacity())' failed.
#
# Abseil requires its users to build with the same options as Abseil itself.
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

# ── Engine library, linked by the demo, the tests and the benchmarks ─────────
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

# ── Threads ──────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

target_link_libraries(matching_engine_lib PUBLIC
  absl::flat_hash_map
  Threads::Threads
)

# ── Demo ─────────────────────────────────────────────────────────────────────
add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_lib)

# ── Tests ────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_order_book       tests/test_order_book.cpp)
add_executable(test_spsc_ring_buffer tests/test_spsc_ring_buffer.cpp)
add_executable(test_memory_pool      tests/test_memory_pool.cpp)
add_executable(test_matching_engine  tests/test_matching_engine.cpp)
add_executable(test_thread_placement tests/test_thread_placement.cpp)

foreach(test_target
    test_order_book
    test_spsc_ring_buffer
    test_memory_pool
    test_matching_engine
    test_thread_placement)
  target_link_libraries(${test_target} PRIVATE
    matching_engine_lib
    GTest::gtest_main
  )
  add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()

# The measurement scripts' tests need no build and no measurement mode. `ctest -L scripts`
# runs only them, `ctest -LE scripts` everything else.
foreach(script_test
    scripts/env/test_bench_env.sh
    scripts/env/test_install_bench_env.sh)
  get_filename_component(script_test_name ${script_test} NAME_WE)
  add_test(NAME ${script_test_name}
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/${script_test}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(${script_test_name} PROPERTIES LABELS scripts)
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  foreach(script_test
      scripts/record/test_bench_diagnose.py
      scripts/report/test_bench_report.py
      scripts/report/test_diagnose_report.py
      scripts/diagnostics/test_thread_affinity.py)
    get_filename_component(script_test_name ${script_test} NAME_WE)
    # -B: no __pycache__ in the source tree
    add_test(NAME ${script_test_name}
             COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/${script_test}
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(${script_test_name} PROPERTIES LABELS scripts)
  endforeach()
else()
  message(STATUS "Python 3 not found: the Python script tests are not registered")
endif()

# ── Benchmarks ───────────────────────────────────────────────────────────────
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

# The single-threaded benchmarks check that each iteration did its work and report an
# error otherwise ("ERROR OCCURRED"); each runs briefly here and an error fails the test.
# MixedWorkload replays a million orders per iteration and takes over a minute under
# sanitizers, so it runs only in builds without them.
set(bench_check_args --benchmark_min_time=0.01s)
if(CMAKE_CXX_FLAGS_${ME_BUILD_TYPE_UPPER} MATCHES "-fsanitize=")
  list(APPEND bench_check_args --benchmark_filter=-MixedWorkload)
endif()
add_test(NAME bench_order_book_checks COMMAND bench_order_book ${bench_check_args})
add_test(NAME bench_single_thread_checks
         COMMAND bench_matching_engine --benchmark_filter=SingleThread --benchmark_min_time=0.01s)
set_tests_properties(bench_order_book_checks bench_single_thread_checks PROPERTIES
  FAIL_REGULAR_EXPRESSION "ERROR OCCURRED")

# Calibrates the TSC against CLOCK_MONOTONIC for scripts/env/capture_env.sh, which stores
# the result with every recording. The test checks only that it runs and prints a frequency.
add_executable(tsc_calibrate scripts/env/tsc_calibrate.cpp)
add_test(NAME tsc_calibrate COMMAND tsc_calibrate)
set_tests_properties(tsc_calibrate PROPERTIES
  PASS_REGULAR_EXPRESSION "^\\{\"ghz\": [0-9]+\\.[0-9]+, \"ghz_min\": [0-9]+\\.[0-9]+, \"ghz_max\": [0-9]+\\.[0-9]+,")
