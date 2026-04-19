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
#include <memory>
#include "order_book.h"
#include "feed_simulator.h"

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
//
// 设计说明：
//   - 预生成订单列表，避免循环内 make_order 构造开销混入计时。
//   - 每 kBatchSize 次迭代在 PauseTiming 内重建 OrderBook（重挂固定背景卖单
//     并清空累积的买单），保证每次 add_order 面对的树深度恒定在 kBgDepth，
//     测出的是"稳态固定深度下的插入延迟"。
//   - 不固定 Iterations，让框架自动决定（误差 < 0.5% 才停），结果更可信。
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    constexpr int kBgDepth   = 100;   // 背景卖单档位数（固定盘口深度）
    constexpr int kBatchSize = 1000;  // 每批重建一次 OrderBook

    // 预生成背景卖单（ask 从 101 开始）
    std::vector<Order> bg_sells(kBgDepth);
    for (int i = 0; i < kBgDepth; ++i) {
        bg_sells[i] = make_order(static_cast<uint64_t>(10000 + i), Side::SELL,
                                 101'000'000 + i * 100'000, 10);
    }

    // 预生成被测买单（买价 99，永远不与卖单成交）
    // 使用不同的 order_id 区间（20000~20000+kBatchSize），避免与背景卖单冲突
    std::vector<Order> buy_orders(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        buy_orders[i] = make_order(static_cast<uint64_t>(20000 + i),
                                   Side::BUY, 99'000'000, 1);
    }

    // 用 unique_ptr 持有 OrderBook，以便在 PauseTiming 内重建
    // （OrderBook 含 std::mutex，不可直接移动赋值）
    auto rebuild_book = [&]() {
        auto b = std::make_unique<OrderBook>("BTCUSD");
        for (int i = 0; i < kBgDepth; ++i) b->add_order(&bg_sells[i]);
        return b;
    };
    auto book = rebuild_book();

    int idx = 0;
    for (auto _ : state) {
        // 每批用完后重建，维持固定深度（把构造/重置开销放入 PauseTiming）
        if (idx == kBatchSize) {
            state.PauseTiming();
            book = rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        // 直接用预生成的订单，零构造开销
        auto trades = book->add_order(&buy_orders[idx]);
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_AddOrder_NoMatch);

// ── 2. 一对一精确成交 ────────────────────────────────────────────────────────
// 每次迭代：先挂一个卖单，再来一个完全匹配的买单，触发精确成交。
// 这是最典型的热路径场景。
//
// 设计说明：
//   - 预生成 sell/buy 配对列表，消除循环内 make_order 的构造开销。
//   - 每次迭代在 PauseTiming 内挂卖单（准备成交对手方），
//     计时范围精确限定在买单触发撮合的 add_order 调用上。
//   - 不固定 Iterations，由框架自动决定。
static void BM_AddOrder_FullMatch(benchmark::State& state) {
    constexpr int kBatchSize = 10'000;

    // 预生成卖单和买单（id 区间错开，互不冲突）
    std::vector<Order> sell_orders(kBatchSize);
    std::vector<Order> buy_orders(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        sell_orders[i] = make_order(static_cast<uint64_t>(30000 + i),
                                    Side::SELL, 100'000'000, 10);
        buy_orders[i]  = make_order(static_cast<uint64_t>(40000 + i),
                                    Side::BUY,  100'000'000, 10);
    }

    OrderBook book("BTCUSD");
    int idx = 0;
    for (auto _ : state) {
        if (idx == kBatchSize) idx = 0;

        // PauseTiming：挂卖单（准备对手方），不计入撮合延迟
        state.PauseTiming();
        book.add_order(&sell_orders[idx]);
        state.ResumeTiming();

        // 计时范围：买单触发撮合
        auto trades = book.add_order(&buy_orders[idx]);
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_AddOrder_FullMatch);

// ── 3. 大单扫穿多档（最坏情况） ──────────────────────────────────────────────
// 测试参数化：state.range(0) 控制被扫穿的价格档位数。
//
// 改进设计 v2（解决 v1 中 kBatch 个 OrderBook 反复构造/析构哈希表的问题）：
//   v1 的 fill_batch 每批构造 kBatch 个 OrderBook，每个 OrderBook 在构造时
//   会 reserve(65536) 的 flat_hash_map，导致大量堆分配，严重拉长 PauseTiming
//   区间，同时干扰 perf 对核心撮合路径的采样。
//
//   v2 方案：
//   - 整个 benchmark 生命周期只构造 1 个 OrderBook（循环外），避免重复构造。
//   - Slot 结构仅存储数据（sells + buy），不再持有 OrderBook。
//   - fill_batch 只生成订单数据（无堆分配），然后调用 book.clear() + 重新挂单，
//     book 内部的 flat_hash_map 桶内存被复用（clear 不释放已分配的容量）。
//   - PauseTiming 频率仍为 1 次/kBatch，噪声摊薄逻辑不变。
static void BM_AddOrder_SweepLevels(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));

    // kBatch：每批预建的盘口数量。
    // levels 越小，每次迭代耗时越短，需要更多批量来摊薄重建开销。
    // 这里统一取 500，对所有 levels 均足够。
    constexpr int kBatch = 500;

    // Slot 仅持有订单数据，不再包含 OrderBook
    // sells 持久化存储，确保 OrderBook 内部保存的指针在整批次期间始终有效
    struct Slot {
        std::vector<Order> sells;
        Order buy;
    };
    std::vector<Slot> slots(kBatch);
    for (auto& s : slots) s.sells.resize(levels);

    // 全局递增 id，确保跨批次的 order_id 不重复
    uint64_t next_id = 1;

    // 整个 benchmark 只构造一次 OrderBook，避免反复构造/析构哈希表
    OrderBook book("BTCUSD");

    // 填充一批槽位的订单数据，并重建盘口（在 PauseTiming 内调用）
    // 注意：此函数会先 clear() 再重新挂单，book 的内存容量被复用
    auto fill_batch = [&]() {
        book.clear();
        for (int s = 0; s < kBatch; ++s) {
            // 生成卖单数据
            for (int i = 0; i < levels; ++i) {
                slots[s].sells[i] = make_order(next_id++, Side::SELL,
                                               100'000'000 + i * 100'000, 1);
            }
            // 预构造扫穿买单（价格高于所有卖档，qty=levels 恰好全部成交）
            slots[s].buy = make_order(next_id++, Side::BUY,
                                      100'000'000 + (levels + 1) * 100'000,
                                      levels);
        }
        // 将第 0 批槽位的卖单挂入盘口，作为第一次迭代的初始状态
        // 后续每次迭代结束后，在 PauseTiming 内换入下一个 slot 的卖单
        for (int i = 0; i < levels; ++i) {
            book.add_order(&slots[0].sells[i]);
        }
    };

    // 初始填充（benchmark 开始前，不在计时区间内）
    fill_batch();

    int idx = 0;
    for (auto _ : state) {
        // 计时范围：仅含扫穿撮合逻辑
        auto trades = book.add_order(&slots[idx].buy);
        benchmark::DoNotOptimize(trades);
        ++idx;

        // 每次迭代结束后，在 PauseTiming 内为下一次迭代准备盘口
        // （clear + 挂下一个 slot 的卖单，或批量重建）
        if (idx == kBatch) {
            state.PauseTiming();
            fill_batch();
            state.ResumeTiming();
            idx = 0;
        } else {
            state.PauseTiming();
            book.clear();
            for (int i = 0; i < levels; ++i) {
                book.add_order(&slots[idx].sells[i]);
            }
            state.ResumeTiming();
        }
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
// 分别测试扫穿 1、5、10、20 个价格档位
BENCHMARK(BM_AddOrder_SweepLevels)->Arg(1)->Arg(5)->Arg(10)->Arg(20);

// ── 4. 撤单操作延迟 ──────────────────────────────────────────────────────────
static void BM_CancelOrder(benchmark::State& state) {
    // 预分配订单存储，避免 benchmark 循环中 vector 重分配
    constexpr size_t kBatchSize = 10'00;
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
BENCHMARK(BM_CancelOrder);

// ── 5. 混合流量（模拟真实场景） ───────────────────────────────────────────────
// 使用 FeedSimulator 生成真实分布的订单序列（限价单 + 撤单混合）
// 并批量送入 OrderBook，测量平均每笔订单的处理吞吐量。
static void BM_MixedWorkload(benchmark::State& state) {
    // 预生成订单（不计入 benchmark 时间）
    FeedSimulator sim("BTCUSD", 100'000'000, /*cancel_ratio=*/0.1, /*seed=*/42);
    auto orders = sim.generate_random(1000'000);

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
BENCHMARK(BM_MixedWorkload)->Iterations(50);
