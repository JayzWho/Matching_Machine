/**
 * test_matching_engine.cpp
 *
 * MatchingEngine 多线程流水线集成测试。
 *
 * 覆盖场景：
 *   1. 单线程接口向后兼容（submit/add_symbol）
 *   2. 流水线启动与停止（start/stop 生命周期）
 *   3. 消费者订单计数与内存池无泄漏
 *   4. TradeRingBuffer 成交记录写入正确性
 *   5. 并发压力：大批量订单无崩溃、无挂起
 *   6. 线程钉核：钉在指定 CPU 上照常处理全部订单；CPU 不可用时 start() 抛出且引擎停下
 *   7. 内存池归还：每轮结束时内存池里只有仍挂在簿上的订单，撤单归还被撤的挂单
 */

#include "matching_engine.h"
#include "trade_ring_buffer.h"
#include "order_book.h"
#include "order.h"
#include "feed_simulator.h"
#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <system_error>

using namespace me;

// ─── 辅助工具 ────────────────────────────────────────────────────────────────

static Order make_order(uint64_t id, Side side, int64_t price, int64_t qty,
                        const char* symbol = "BTCUSD") {
    Order o{};
    o.order_id  = id;
    o.side      = side;
    o.price     = price;
    o.quantity  = qty;
    o.type      = OrderType::LIMIT;
    std::strncpy(o.symbol, symbol, 7);
    return o;
}

// ─── 1. 单线程接口向后兼容 ───────────────────────────────────────────────────

TEST(MatchingEngineTest, SingleThreadSubmit_NoMatch) {
    MatchingEngine engine;
    engine.add_symbol("BTCUSD", 100'000'000);

    Order o = make_order(1, Side::BUY, 99'000'000, 10);
    auto trades = engine.submit(&o);

    EXPECT_TRUE(trades.empty());  // 无对手单，不成交
    EXPECT_EQ(engine.total_trades(), 0u);
}

TEST(MatchingEngineTest, SingleThreadSubmit_FullMatch) {
    MatchingEngine engine;
    engine.add_symbol("BTCUSD", 100'000'000);

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    auto t1 = engine.submit(&sell);
    EXPECT_TRUE(t1.empty());  // 先挂卖单，无对手

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    auto t2 = engine.submit(&buy);
    ASSERT_EQ(t2.size(), 1u);
    EXPECT_EQ(t2[0].quantity, 10);
    EXPECT_EQ(t2[0].price, 100'000'000);
    EXPECT_EQ(engine.total_trades(), 1u);
}

// ─── 2. TradeRingBuffer 单元测试 ─────────────────────────────────────────────

TEST(TradeRingBufferTest, PushAndDrain) {
    TradeRingBuffer<8> buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);

    Trade t{};
    t.trade_id = 42;
    t.quantity = 5;
    buf.push_trade(t);

    EXPECT_EQ(buf.size(), 1u);
    EXPECT_FALSE(buf.empty());

    size_t count = 0;
    buf.drain([&](const Trade& tr) {
        EXPECT_EQ(tr.trade_id, 42u);
        EXPECT_EQ(tr.quantity, 5);
        ++count;
    });

    EXPECT_EQ(count, 1u);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.total_written(), 1u);
}

TEST(TradeRingBufferTest, WrapAround) {
    // 容量为 4，写入 6 个，验证环形覆盖
    TradeRingBuffer<4> buf;
    for (uint64_t i = 1; i <= 6; ++i) {
        Trade t{};
        t.trade_id = i;
        buf.push_trade(t);
    }

    EXPECT_EQ(buf.size(), 4u);  // 最多保留 4 个（容量）
    EXPECT_EQ(buf.total_written(), 6u);

    // drain 应返回最新的 4 个（trade_id: 3,4,5,6）
    std::vector<uint64_t> ids;
    buf.drain([&](const Trade& t) { ids.push_back(t.trade_id); });
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(ids[0], 3u);
    EXPECT_EQ(ids[3], 6u);
}

TEST(TradeRingBufferTest, DrainEmptyIsSafe) {
    TradeRingBuffer<8> buf;
    size_t count = 0;
    buf.drain([&](const Trade&) { ++count; });
    EXPECT_EQ(count, 0u);
}

// ─── 3. 流水线启停与基本正确性 ───────────────────────────────────────────────

TEST(MatchingEnginePipelineTest, StartStop_SmallBatch) {
    MatchingEngine engine;
    // 运行 1000 笔订单，20% 撤单比例，记录延迟
    engine.start("BTCUSD", 100'000'000, /*order_count=*/1000, /*cancel_ratio=*/0.2, {}, /*record_latency=*/true);
    engine.stop();

    // 消费者应处理了全部订单
    EXPECT_EQ(engine.consumed_count(), 1000u);

    // 每笔订单的排队时间、服务时间各有一个采样
    EXPECT_EQ(engine.queue_latency().count(), 1000u);
    EXPECT_EQ(engine.service_latency().count(), 1000u);
}

TEST(MatchingEnginePipelineTest, LatencyNotRecordedByDefault) {
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, /*order_count=*/1000);
    engine.stop();

    EXPECT_EQ(engine.consumed_count(), 1000u);
    EXPECT_EQ(engine.queue_latency().count(), 0u);
    EXPECT_EQ(engine.service_latency().count(), 0u);
}

TEST(MatchingEnginePipelineTest, StartStop_NotRunningAfterStop) {
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, 100);
    engine.stop();
    EXPECT_FALSE(engine.running());
}

TEST(MatchingEnginePipelineTest, TradeBuffer_HasTrades) {
    MatchingEngine engine;
    // 生成 2000 笔订单，0% 撤单：应产生大量成交
    engine.start("BTCUSD", 100'000'000, 2000, 0.0);
    engine.stop();

    // TradeRingBuffer 有容量限制（kTradeBufCap = 4096），
    // 2000 笔订单不会超出，应能拿到所有成交
    EXPECT_GT(engine.trade_buffer().total_written(), 0u);
}

TEST(MatchingEnginePipelineTest, LargeBatch_NoHang) {
    // 10 万笔订单，验证不会挂起或崩溃
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, 100'000, 0.2);

    // 设置超时等待：最多 30 秒
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    engine.stop();  // join 会阻塞直到完成

    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
        << "Pipeline took too long (> 30 seconds)";

    EXPECT_EQ(engine.consumed_count(), 100'000u);
}

// 在单线程上重放流水线生产者生成的同一串订单（FeedSimulator 默认种子相同），
// 返回最后仍挂在订单簿上的订单数
static size_t resting_after(size_t order_count, double cancel_ratio) {
    FeedSimulator sim("BTCUSD", 100'000'000, cancel_ratio);
    std::vector<Order> orders = sim.generate_random(order_count);
    TradeRingBuffer<kTradeBufCap> trades;
    OrderBook book("BTCUSD");
    for (auto& o : orders) book.add_order_noalloc(&o, trades, [](Order*) {});
    return book.order_count();
}

TEST(MatchingEnginePipelineTest, Pool_HoldsOnlyRestingOrdersAfterEachRun) {
    constexpr size_t kCapacity = 16384, kOrders = 5000;
    const size_t resting = resting_after(kOrders, 0.2);
    ASSERT_GT(resting, 0u);

    // 成交和被撤的订单都已归还；每轮从满的内存池开始，所以每轮结束时都相同
    MatchingEngine engine(kCapacity);
    for (int run = 0; run < 3; ++run) {
        engine.start("BTCUSD", 100'000'000, kOrders, 0.2);
        engine.stop();
        EXPECT_EQ(engine.pool_available(), kCapacity - resting) << "run " << run;
    }
}

// ─── 3b. 线程钉核 ─────────────────────────────────────────────────────────────

TEST(MatchingEnginePipelineTest, PinnedThreads_ProcessAllOrders) {
    cpu_set_t set;
    CPU_ZERO(&set);
    ASSERT_EQ(sched_getaffinity(0, sizeof(set), &set), 0);
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);

    // 能用两个 CPU 时分开钉，只有一个时两个线程钉在同一个上
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, 1000, 0.2, ThreadCpus{cpus.front(), cpus.back()});
    engine.stop();
    EXPECT_EQ(engine.consumed_count(), 1000u);
    EXPECT_FALSE(engine.running());
}

TEST(MatchingEnginePipelineTest, UnavailableCpu_ThrowsAndStops) {
    const int outside = CPU_SETSIZE - 1;   // 不在本机上的 CPU 编号
    MatchingEngine engine;
    // 生产者起不来：已起来的消费者要退出，引擎回到停止状态，之后还能正常启动
    EXPECT_THROW(engine.start("BTCUSD", 100'000'000, 1000, 0.2, ThreadCpus{outside, -1}), std::system_error);
    EXPECT_FALSE(engine.running());
    // 消费者起不来
    EXPECT_THROW(engine.start("BTCUSD", 100'000'000, 1000, 0.2, ThreadCpus{-1, outside}), std::system_error);
    EXPECT_FALSE(engine.running());

    engine.start("BTCUSD", 100'000'000, 1000, 0.2);
    engine.stop();
    EXPECT_EQ(engine.consumed_count(), 1000u);
}

// ─── 4. add_order_noalloc 单元测试（直接测 OrderBook 接口） ─────────────────

TEST(OrderBookNoallocTest, NoMatch_NoTradeInBuffer) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    Order buy = make_order(1, Side::BUY, 99'000'000, 10);
    book.add_order_noalloc(&buy, buf, [](Order*) {});

    EXPECT_TRUE(buf.empty());   // 无对手单，无成交
    EXPECT_EQ(buy.filled_qty, 0);
}

TEST(OrderBookNoallocTest, FullMatch_TradeInBuffer) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf, [](Order*) {});
    EXPECT_TRUE(buf.empty());   // 卖单先挂，无对手

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf, [](Order*) {});

    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0].quantity, 10);
    EXPECT_EQ(buf[0].price, 100'000'000);
    EXPECT_EQ(buf[0].buy_order_id, 2u);
    EXPECT_EQ(buf[0].sell_order_id, 1u);
}

TEST(OrderBookNoallocTest, DeallocateCallback_CalledForRestingOrder) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    // 记录被归还的 Order*
    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf, deallocator);
    EXPECT_TRUE(deallocated.empty());  // 卖单挂单，未被成交，不触发回调

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf, deallocator);

    // deallocator 语义：归还所有"处理完毕"的 Order
    // sell（挂单方）被完全成交 → 触发归还；buy（incoming FILLED）→ 同样触发归还
    ASSERT_EQ(deallocated.size(), 2u);
    // 顺序：match_noalloc 先归还 resting(sell)，add_order_noalloc 末尾再归还 incoming(buy)
    EXPECT_EQ(deallocated[0]->order_id, 1u);  // sell：挂单方，由 match_noalloc 归还
    EXPECT_EQ(deallocated[1]->order_id, 2u);  // buy：incoming FILLED，由 add_order_noalloc 归还
}

TEST(OrderBookNoallocTest, PartialMatch_DeallocateNotCalledForPartialResting) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    // 挂一个大卖单（qty=20）
    Order sell = make_order(1, Side::SELL, 100'000'000, 20);
    book.add_order_noalloc(&sell, buf, deallocator);

    // 买单只买 10：buy FILLED，sell 部分成交仍留在 PriceLevel
    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf, deallocator);

    EXPECT_EQ(buf.size(), 1u);          // 1 笔成交
    // buy 完全成交应被归还；sell 仍有剩余不归还
    ASSERT_EQ(deallocated.size(), 1u);
    EXPECT_EQ(deallocated[0]->order_id, 2u);  // buy：incoming FILLED
    EXPECT_EQ(sell.filled_qty, 10);
    EXPECT_EQ(sell.status, OrderStatus::PARTIAL);
}

// 撤单请求：order_id 是要撤的订单
static Order make_cancel(uint64_t target) {
    Order o = make_order(target, Side::BUY, 0, 0);
    o.type = OrderType::CANCEL;
    return o;
}

TEST(OrderBookNoallocTest, Cancel_ReturnsCancelledOrderAndRequest) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");
    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    Order resting = make_order(1, Side::BUY, 99'000'000, 10);
    book.add_order_noalloc(&resting, buf, deallocator);
    ASSERT_TRUE(deallocated.empty());

    Order cancel = make_cancel(1);
    book.add_order_noalloc(&cancel, buf, deallocator);

    // 被撤的挂单先归还，撤单请求随后
    ASSERT_EQ(deallocated.size(), 2u);
    EXPECT_EQ(deallocated[0], &resting);
    EXPECT_EQ(deallocated[1], &cancel);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_bid(), 0);
}

TEST(OrderBookNoallocTest, Cancel_ReturnsPartiallyFilledOrder) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");
    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    Order sell = make_order(1, Side::SELL, 100'000'000, 20);
    book.add_order_noalloc(&sell, buf, deallocator);
    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf, deallocator);   // buy 完全成交并归还，sell 剩 10
    ASSERT_EQ(deallocated.size(), 1u);

    Order cancel = make_cancel(1);
    book.add_order_noalloc(&cancel, buf, deallocator);
    ASSERT_EQ(deallocated.size(), 3u);
    EXPECT_EQ(deallocated[1], &sell);
    EXPECT_EQ(deallocated[2], &cancel);
    EXPECT_EQ(book.best_ask(), 0);
}

TEST(OrderBookNoallocTest, Cancel_UnknownOrderReturnsOnlyRequest) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");
    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    // 要撤的订单已成交：不在簿上，只归还撤单请求
    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf, deallocator);
    book.add_order_noalloc(&buy, buf, deallocator);
    deallocated.clear();

    Order cancel_filled = make_cancel(1);
    Order cancel_unknown = make_cancel(99);
    book.add_order_noalloc(&cancel_filled, buf, deallocator);
    book.add_order_noalloc(&cancel_unknown, buf, deallocator);
    ASSERT_EQ(deallocated.size(), 2u);
    EXPECT_EQ(deallocated[0], &cancel_filled);
    EXPECT_EQ(deallocated[1], &cancel_unknown);
}
