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
#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "order_book.h"
#include "feed_simulator.h"

using namespace me;

// ── 辅助工具 ────────────────────────────────────────────────────────────────

// 构造一个简单限价单（不依赖 FeedSimulator）
//
// 时间戳预先写好（非零即可）：订单簿遇到时间戳为 0 的订单会自己读一次 TSC 补上，
// 而流水线里生产者总会先写时间戳。不写的话，每个订单对象第一次进入计时区时多读一次 TSC，
// 之后复用时又不读，各次迭代做的事不一样。
static Order make_order(uint64_t id, Side side, int64_t price, int64_t qty) {
    Order o{};
    o.order_id     = id;
    o.side         = side;
    o.price        = price;
    o.quantity     = qty;
    o.type         = OrderType::LIMIT;
    o.timestamp_ns = 1;
    std::strncpy(o.symbol, "BTCUSD", 7);
    return o;
}

// 让订单回到送入订单簿之前的状态，同一批订单对象可以再送入一个新的订单簿
static void reset(Order& o) {
    o.filled_qty = 0;
    o.status     = OrderStatus::NEW;
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
// 每次迭代：一张买单与一张数量相同的卖单完全成交。卖单是它所在价格档位的唯一订单，
// 成交后这一档被删除。这是最典型的热路径场景。
//
// 设计说明：
//   - 与 NoMatch 相同的批量模式：每 kBatchSize 次迭代暂停计时一次，在暂停内重置这一批订单的
//     状态、新建订单簿、挂好 kBatchSize 张卖单（价格逐档升高，一档一张）。第 i 次迭代的买单
//     以第 i 档的价格下单，更低的档位已被前面的买单吃掉，它正好与第 i 档那张卖单成交。
//   - 计数器 trades_per_iteration 是每次迭代的成交笔数，应恰为 1；不是 1 时报错。
//   - 不固定 Iterations，由框架自动决定。
static void BM_AddOrder_FullMatch(benchmark::State& state) {
    constexpr int kBatchSize = 1000;

    // 预生成卖单和买单（id 区间错开，互不冲突）
    std::vector<Order> sell_orders(kBatchSize);
    std::vector<Order> buy_orders(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        const int64_t price = 100'000'000 + i * 100'000;
        sell_orders[i] = make_order(static_cast<uint64_t>(30000 + i), Side::SELL, price, 10);
        buy_orders[i]  = make_order(static_cast<uint64_t>(40000 + i), Side::BUY,  price, 10);
    }

    // 重置订单、新建订单簿并挂好卖单；旧订单簿在这里析构，都在暂停计时内
    std::unique_ptr<OrderBook> book;
    auto rebuild_book = [&]() {
        book = std::make_unique<OrderBook>("BTCUSD");
        for (int i = 0; i < kBatchSize; ++i) {
            reset(sell_orders[i]);
            reset(buy_orders[i]);
            book->add_order(&sell_orders[i]);
        }
    };
    rebuild_book();

    int idx = 0;
    int64_t matched = 0;
    for (auto _ : state) {
        if (idx == kBatchSize) {
            state.PauseTiming();
            rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        // 计时范围：买单触发撮合
        auto trades = book->add_order(&buy_orders[idx]);
        matched += static_cast<int64_t>(trades.size());
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.counters["trades_per_iteration"] =
        benchmark::Counter(static_cast<double>(matched), benchmark::Counter::kAvgIterations);
    if (matched != state.iterations()) state.SkipWithError("an iteration did not match exactly one order");
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_AddOrder_FullMatch);

// ── 3. 大单扫穿多档（最坏情况） ──────────────────────────────────────────────
// 测试参数化：state.range(0) 控制被扫穿的价格档位数 L。
// 每次迭代：一张数量为 L 的买单依次吃掉 L 个价格档位，每档一张数量为 1 的卖单。
//
// 设计说明：
//   - 与 NoMatch 相同的批量模式：每批暂停计时一次，在暂停内重置这一批订单的状态、
//     新建订单簿、挂好约 1000 张卖单（价格逐档升高，一档一张）。一批 1000 / L 次迭代，
//     第 s 次迭代的买单以第 s 组 L 档里最高的价格下单，更低的档位已被前面的买单吃掉，
//     它正好吃完这一组 L 档后全部成交。
//   - 计数器 trades_per_iteration 应恰为 L；不是 L 时报错。
static void BM_AddOrder_SweepLevels(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const int sweeps = 1000 / levels;  // 每批的迭代次数

    std::vector<Order> sell_orders(static_cast<size_t>(sweeps * levels));
    std::vector<Order> buy_orders(static_cast<size_t>(sweeps));
    uint64_t next_id = 1;
    for (int s = 0; s < sweeps; ++s) {
        for (int i = 0; i < levels; ++i) {
            const int k = s * levels + i;
            sell_orders[k] = make_order(next_id++, Side::SELL, 100'000'000 + k * 100'000, 1);
        }
        buy_orders[s] = make_order(next_id++, Side::BUY,
                                   100'000'000 + (s * levels + levels - 1) * 100'000, levels);
    }

    // 重置订单、新建订单簿并挂好卖单；旧订单簿在这里析构，都在暂停计时内
    std::unique_ptr<OrderBook> book;
    auto rebuild_book = [&]() {
        book = std::make_unique<OrderBook>("BTCUSD");
        for (auto& o : sell_orders) {
            reset(o);
            book->add_order(&o);
        }
        for (auto& o : buy_orders) reset(o);
    };
    rebuild_book();

    int idx = 0;
    int64_t matched = 0;
    for (auto _ : state) {
        if (idx == sweeps) {
            state.PauseTiming();
            rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        // 计时范围：仅含扫穿撮合逻辑
        auto trades = book->add_order(&buy_orders[idx]);
        matched += static_cast<int64_t>(trades.size());
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.counters["trades_per_iteration"] =
        benchmark::Counter(static_cast<double>(matched), benchmark::Counter::kAvgIterations);
    if (matched != state.iterations() * levels) state.SkipWithError("an iteration did not sweep every level");
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
// 分别测试扫穿 1、5、10、20 个价格档位
BENCHMARK(BM_AddOrder_SweepLevels)->Arg(1)->Arg(5)->Arg(10)->Arg(20);

// ── 4. 撤单操作延迟 ──────────────────────────────────────────────────────────
// 每次迭代撤掉一张挂着的买单。它是所在价格档位的唯一订单，撤掉后这一档被删除。
//
// 设计说明：
//   - 与 NoMatch 相同的批量模式：每 kBatchSize 次迭代暂停计时一次，在暂停内重置待撤订单的
//     状态、新建订单簿（旧订单簿在这里析构）、挂好 kBatchSize 张待撤买单和 kBatchSize 张
//     背景买单，两者价格交错、一档一张。背景买单不撤，让一批里订单簿始终有 kBatchSize 档以上。
//   - 待撤订单按一个固定的乱序撤，撤单不总落在最优价或最差价一端。
//   - 计数器 cancels_per_iteration 应恰为 1；有撤单失败时报错。
static void BM_CancelOrder(benchmark::State& state) {
    constexpr int kBatchSize = 1000;

    std::vector<Order> targets(kBatchSize);
    std::vector<Order> background(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        background[i] = make_order(static_cast<uint64_t>(2 * i + 1), Side::BUY,
                                   99'000'000 - (2 * i) * 1000, 1);
        targets[i]    = make_order(static_cast<uint64_t>(2 * i + 2), Side::BUY,
                                   99'000'000 - (2 * i + 1) * 1000, 1);
    }
    std::vector<uint64_t> cancel_ids(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) cancel_ids[i] = targets[i].order_id;
    std::shuffle(cancel_ids.begin(), cancel_ids.end(), std::mt19937_64(42));

    std::unique_ptr<OrderBook> book;
    auto rebuild_book = [&]() {
        book = std::make_unique<OrderBook>("BTCUSD");
        for (int i = 0; i < kBatchSize; ++i) {
            reset(targets[i]);
            book->add_order(&background[i]);
            book->add_order(&targets[i]);
        }
    };
    rebuild_book();

    int idx = 0;
    int64_t cancelled = 0;
    for (auto _ : state) {
        if (idx == kBatchSize) {
            state.PauseTiming();
            rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        // 计时范围：撤单
        cancelled += book->cancel_order(cancel_ids[idx]) ? 1 : 0;
        ++idx;
    }

    state.counters["cancels_per_iteration"] =
        benchmark::Counter(static_cast<double>(cancelled), benchmark::Counter::kAvgIterations);
    if (cancelled != state.iterations()) state.SkipWithError("a cancel did not find its order");
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
    for (auto& o : orders) o.timestamp_ns = 1;  // 生成的时间戳为 0，原因见 make_order

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
