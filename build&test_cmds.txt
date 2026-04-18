# ── 首次配置（只需执行一次） ──────────────────────────────────────────────────

# Debug 版本（ASan + UBSan，-g -O0）
cmake -S . -B build/debug  -DCMAKE_BUILD_TYPE=Debug

# Release 版本（-O2 -DNDEBUG）
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release

# ── 构建 ──────────────────────────────────────────────────────────────────────

cmake --build build/debug   -j$(nproc)
cmake --build build/release -j$(nproc)

# ── 跑测试（Debug） ───────────────────────────────────────────────────────────

cmake --build build/debug -j$(nproc) && cd build/debug && ctest --output-on-failure

# ── 跑 Benchmark（Release，绑核减少噪声） ────────────────────────────────────

# 第十章：OrderBook 单线程基线
cmake --build build/release -j$(nproc) && taskset -c 0 ./build/release/bench_order_book

# 第十一章：SPSC 多线程流水线（需要至少 2 个 CPU 核心）
cmake --build build/release -j$(nproc) && taskset -c 0,1 ./build/release/bench_matching_engine

# 输出 JSON（用于对比）
taskset -c 0,1 ./build/release/bench_matching_engine \
    --benchmark_format=json > results/bench_pipeline_$(date +%Y-%m-%d_%H-%M-%S).json

# ── 单独跑流水线测试 ──────────────────────────────────────────────────────────

# 仅跑快速测试（跳过 LargeBatch）
./build/release/test_matching_engine --gtest_filter="-*LargeBatch*"

# 全量测试（含 10 万笔订单压力测试，约 30~60 秒）
./build/release/test_matching_engine
