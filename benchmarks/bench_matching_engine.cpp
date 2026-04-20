/**
 * bench_matching_engine.cpp
 *
 * 端到端撮合流水线 Benchmark。
 *
 * 测量场景：
 *   1. BM_Pipeline_Throughput    — 完整 SPSC 流水线的订单吞吐量（orders/s）
 *   2. BM_SingleThread_Baseline  — 单线程 add_order_noalloc 基线（对比用）
 *   3. BM_Pipeline_LatencyReport — 端到端延迟分布报告（P50/P99，RDTSC 时钟周期）
 *
 * 运行方式（必须用 Release 模式，绑核以减少调度抖动）：
 *   cmake --build build/release -j$(nproc)
 *   taskset -c 0,1 ./build/release/bench_matching_engine
 *   taskset -c 0,1 ./build/release/bench_matching_engine \
 *       --benchmark_format=json > results/bench_pipeline.json
 *
 * 注意：
 *   - 多线程 benchmark 需要至少 2 个 CPU 核心
 *   - 在腾讯云虚拟机上，硬件 PMU 不可用；延迟以 RDTSC 时钟周期报告
 *   - BM_Pipeline_LatencyReport 通过 MatchingEngine 内部 LatencyRecorder 手动输出，
 *     Google Benchmark 框架无法直接测量跨线程延迟分布
 */

#include <benchmark/benchmark.h>
#include <cstring>
#include "matching_engine.h"
#include "order_book.h"
#include "feed_simulator.h"
#include "trade_ring_buffer.h"
#include "latency_recorder.h"

using namespace me;

// ─── 辅助工具 ────────────────────────────────────────────────────────────────

static Order make_order(uint64_t id, Side side, int64_t price, int64_t qty) {
    Order o{};
    o.order_id  = id;
    o.side      = side;
    o.price     = price;
    o.quantity  = qty;
    o.type      = OrderType::LIMIT;
    std::strncpy(o.symbol, "BTCUSD", 7);
    return o;
}

// ─── 1. 多线程流水线吞吐量 ───────────────────────────────────────────────────
//
// 每次 benchmark 迭代运行完整的 start→stop 生命周期，
// 统计每秒处理的订单数（throughput）。
//
// state.SetItemsProcessed() 告知框架本次迭代处理了多少"逻辑单元"，
// 框架自动计算并输出 items/s（即 orders/s）。

static void BM_Pipeline_Throughput(benchmark::State& state) {
    const size_t order_count = static_cast<size_t>(state.range(0));
    constexpr double cancel_ratio = 0.2;

    MatchingEngine engine;
    for (auto _ : state) {
        engine.start("BTCUSD", 100'000'000LL, order_count, cancel_ratio);
        engine.stop();
    }

    // 每次迭代处理 order_count 笔订单
    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(order_count)
    );
    state.SetLabel("orders=" + std::to_string(order_count));
}

// 参数：订单数量（10K / 50K / 100K）
BENCHMARK(BM_Pipeline_Throughput)
    ->Arg(10'000)
    ->Arg(50'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);   // 减少迭代次数（每次都要启停线程）

// ─── 3. 单线程 add_order_noalloc 基线（无线程切换开销）────────────────────
//
// 直接在当前线程循环调用 add_order_noalloc，测量撮合核心路径本身的吞吐量。
// 与 BM_Pipeline_Throughput 对比，可量化 SPSC 队列和线程同步的额外开销。
//
// 数据来源：FeedSimulator 预生成，避免生成逻辑混入计时。

static void BM_SingleThread_Baseline(benchmark::State& state) {
    constexpr size_t kBatchSize  = 10'000;
    constexpr double kCancelRatio = 0.2;

    FeedSimulator sim("BTCUSD", 100'000'000LL, /*cancel_ratio=*/kCancelRatio);
    auto orders = sim.generate_random(kBatchSize);

    // 将值语义的 Order 放入内存池（模拟 MemoryPool 路径）
    // 此处为简化，直接用 vector<Order> 的地址
    TradeRingBuffer<kTradeBufCap> trade_buf;
    OrderBook book("BTCUSD");

    for (auto _ : state) {
        state.PauseTiming();
        book.clear();
        // 重置订单状态（每次迭代重用同一批数据）
        for (auto& o : orders) {
            o.filled_qty = 0;
            o.status     = OrderStatus::NEW;
        }
        state.ResumeTiming();

        for (auto& o : orders) {
            book.add_order_noalloc(&o, trade_buf, [](Order*) {});
        }

        benchmark::DoNotOptimize(trade_buf.size());
        state.PauseTiming();
        trade_buf.drain([](const Trade&) {});  // 清空 trade_buf 以便下次迭代
        state.ResumeTiming();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(kBatchSize)
    );
    state.SetLabel("single-thread noalloc");
}

BENCHMARK(BM_SingleThread_Baseline)
    ->Unit(benchmark::kMicrosecond);

// ─── 4. 延迟报告：benchmark 结束后打印 P50/P99 ──────────────────────────────
//
// Google Benchmark 不支持直接在 fixture 析构时输出，
// 因此用一个"汇报型" benchmark 运行一次并手动 report。

static void BM_Pipeline_LatencyReport(benchmark::State& state) {
    MatchingEngine engine;
    for (auto _ : state) {
        state.PauseTiming();
        engine.start("BTCUSD", 100'000'000LL, 100'000, 0.2);
        engine.stop();
        state.ResumeTiming();

        // 手动输出延迟百分位（输出到 stdout，不影响 benchmark JSON 结果）
        std::printf("\n--- Pipeline Latency Report (100K orders) ---\n");
        engine.latency_recorder().report(/*cpu_ghz=*/2.494);
        std::printf("  (Trades produced: %zu)\n\n",
                    engine.trade_buffer().total_written());
    }
}

BENCHMARK(BM_Pipeline_LatencyReport)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);
