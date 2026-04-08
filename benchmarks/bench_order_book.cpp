/**
 * bench_order_book.cpp
 *
 * Google Benchmark 套件：测量 OrderBook 的撮合延迟与吞吐量。
 *
 * 测量场景：
 *   1. BM_AddOrder_NoMatch       — 纯挂单，无撮合（测基础插入开销）
 *   2. BM_AddOrder_FullMatch     — 一对一精确成交（最常见的热路径）
 *   3. BM_AddOrder_SweepLevels   — 大单扫穿多个价格档位（最坏情况）
 *   4. BM_CancelOrder            — 撤单操作延迟
 *   5. BM_MixedWorkload          — 混合流量（70% 挂单 + 20% 成交 + 10% 撤单）
 *
 * 运行方式（必须用 Release 模式，Debug 数据无参考价值）：
 *   cmake --build build/release -j$(nproc)
 *   taskset -c 0 ./build/release/bench_order_book
 *   taskset -c 0 ./build/release/bench_order_book --benchmark_format=json > results.json
 */

#include <benchmark/benchmark.h>
#include "order_book.h"
#include "feed_simulator.h"
#include "memory_pool.h"

using namespace me;

// ── 辅助工具 ────────────────────────────────────────────────────────────────

// 构造一个简单限价单（不依赖 FeedSimulator）
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

// ── 1. 纯挂单，无撮合 ────────────────────────────────────────────────────────
// 测量在无法成交时（买价始终低于卖价），add_order 的纯挂单开销。
// 这是 OrderBook 操作的下界延迟。
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    OrderBook book("BTCUSD");

    // 预先挂一些卖单作为背景深度（ask 从 101 开始）
    for (int i = 0; i < 100; ++i) {
        static std::vector<Order> bg_orders;
        bg_orders.push_back(make_order(10000 + i, Side::SELL,
                                       101'000'000 + i * 100'000, 10));
        book.add_order(&bg_orders.back());
    }

    uint64_t id = 1;
    for (auto _ : state) {
        // 买价 99，卖价最低 101，永远不成交
        Order o = make_order(id++, Side::BUY, 99'000'000, 1);
        auto trades = book.add_order(&o);
        benchmark::DoNotOptimize(trades);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_AddOrder_NoMatch)->Iterations(100'000);

// ── 2. 一对一精确成交 ────────────────────────────────────────────────────────
// 每次迭代：先挂一个卖单，再来一个完全匹配的买单，触发精确成交。
// 这是最典型的热路径场景。
static void BM_AddOrder_FullMatch(benchmark::State& state) {
    OrderBook book("BTCUSD");

    uint64_t id = 1;
    for (auto _ : state) {
        // 挂卖单
        Order sell = make_order(id++, Side::SELL, 100'000'000, 10);
        book.add_order(&sell);

        // 买单触发撮合
        Order buy = make_order(id++, Side::BUY, 100'000'000, 10);
        auto trades = book.add_order(&buy);
        benchmark::DoNotOptimize(trades);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_AddOrder_FullMatch)->Iterations(100'000);

// ── 3. 大单扫穿多档（最坏情况） ──────────────────────────────────────────────
// 测试参数化：state.range(0) 控制被扫穿的价格档位数。
// 每次迭代之前重新建立对应深度的盘口，然后用一个大单全部扫穿。
static void BM_AddOrder_SweepLevels(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));

    uint64_t id = 1;
    for (auto _ : state) {
        state.PauseTiming();  // 暂停计时：建立盘口不计入 benchmark
        OrderBook book("BTCUSD");
        // 在 100.000000 ~ 100.000000+levels 的价格档位各挂 1 笔卖单
        for (int i = 0; i < levels; ++i) {
            Order sell = make_order(id++, Side::SELL,
                                    100'000'000 + i * 100'000, 1);
            book.add_order(&sell);
        }
        state.ResumeTiming();  // 恢复计时：下面才是被测代码

        // 买入大单，扫穿所有价格档位
        Order buy = make_order(id++, Side::BUY,
                               100'000'000 + (levels + 1) * 100'000,
                               levels);  // qty = levels，刚好全部成交
        auto trades = book.add_order(&buy);
        benchmark::DoNotOptimize(trades);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
// 分别测试扫穿 1、5、10、20 个价格档位
BENCHMARK(BM_AddOrder_SweepLevels)->Arg(1)->Arg(5)->Arg(10)->Arg(20);

// ── 4. 撤单操作延迟 ──────────────────────────────────────────────────────────
static void BM_CancelOrder(benchmark::State& state) {
    // 预分配订单存储，避免 benchmark 循环中 vector 重分配
    constexpr size_t kBatchSize = 10'000;
    std::vector<Order> orders(kBatchSize);

    uint64_t id = 1;
    size_t idx = 0;

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book("BTCUSD");
        // 先批量挂单
        for (size_t i = 0; i < kBatchSize; ++i) {
            orders[i] = make_order(id++, Side::BUY,
                                   99'000'000 - static_cast<int64_t>(i) * 1000, 1);
            book.add_order(&orders[i]);
        }
        state.ResumeTiming();

        // 测量撤单
        bool ok = book.cancel_order(orders[idx % kBatchSize].order_id);
        benchmark::DoNotOptimize(ok);
        ++idx;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_CancelOrder)->Iterations(50'000);

// ── 5. 混合流量（模拟真实场景） ───────────────────────────────────────────────
// 使用 FeedSimulator 生成真实分布的订单序列（限价单 + 撤单混合）
// 并批量送入 OrderBook，测量平均每笔订单的处理吞吐量。
static void BM_MixedWorkload(benchmark::State& state) {
    // 预生成订单（不计入 benchmark 时间）
    FeedSimulator sim("BTCUSD", 100'000'000, /*seed=*/42);
    auto orders = sim.generate_random(100'000, /*cancel_ratio=*/0.1);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book("BTCUSD");
        state.ResumeTiming();

        int64_t total_trades = 0;
        for (auto& o : orders) {
            if (o.type == OrderType::CANCEL) {
                book.cancel_order(o.order_id);
            } else {
                auto trades = book.add_order(&o);
                total_trades += static_cast<int64_t>(trades.size());
            }
        }
        benchmark::DoNotOptimize(total_trades);
    }

    // 报告每秒处理的订单数
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(orders.size()));
}
BENCHMARK(BM_MixedWorkload)->Iterations(20);
