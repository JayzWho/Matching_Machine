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
 */

#include "matching_engine.h"
#include "trade_ring_buffer.h"
#include "order_book.h"
#include "order.h"
#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>

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
    // 运行 1000 笔订单，20% 撤单比例
    engine.start("BTCUSD", 100'000'000, /*order_count=*/1000, /*cancel_ratio=*/0.2);
    engine.stop();

    // 消费者应处理了全部订单
    EXPECT_EQ(engine.consumed_count(), 1000u);

    // 延迟记录器应有对应数量的采样
    EXPECT_EQ(engine.latency_recorder().count(), 1000u);
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
    // 1 万笔订单，验证不会挂起或崩溃（在 2 核虚拟机上约 1~3 秒）
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, 10'000, 0.2);

    // 设置超时等待：最多 30 秒
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    engine.stop();  // join 会阻塞直到完成

    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
        << "Pipeline took too long (> 30 seconds)";

    EXPECT_EQ(engine.consumed_count(), 10'000u);
}

// ─── 4. add_order_noalloc 单元测试（直接测 OrderBook 接口） ─────────────────

TEST(OrderBookNoallocTest, NoMatch_NoTradeInBuffer) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    Order buy = make_order(1, Side::BUY, 99'000'000, 10);
    book.add_order_noalloc(&buy, buf);

    EXPECT_TRUE(buf.empty());   // 无对手单，无成交
    EXPECT_EQ(buy.filled_qty, 0);
}

TEST(OrderBookNoallocTest, FullMatch_TradeInBuffer) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf);
    EXPECT_TRUE(buf.empty());   // 卖单先挂，无对手

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf);

    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0].quantity, 10);
    EXPECT_EQ(buf[0].price, 100'000'000);
    EXPECT_EQ(buf[0].buy_order_id, 2u);
    EXPECT_EQ(buf[0].sell_order_id, 1u);
}

TEST(OrderBookNoallocTest, DeallocateCallback_CalledForRestingOrder) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    // 注册归还回调，记录被回调的 Order*
    std::vector<Order*> deallocated;
    book.set_deallocate_cb([&](Order* o) { deallocated.push_back(o); });

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf);
    EXPECT_TRUE(deallocated.empty());  // 卖单挂单，未被成交，不触发回调

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf);

    // 挂单方（sell）被完全成交 → 应触发归还回调
    ASSERT_EQ(deallocated.size(), 1u);
    EXPECT_EQ(deallocated[0]->order_id, 1u);  // sell 是挂单方
}

TEST(OrderBookNoallocTest, PartialMatch_DeallocateNotCalledForPartialResting) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    std::vector<Order*> deallocated;
    book.set_deallocate_cb([&](Order* o) { deallocated.push_back(o); });

    // 挂一个大卖单（qty=20）
    Order sell = make_order(1, Side::SELL, 100'000'000, 20);
    book.add_order_noalloc(&sell, buf);

    // 买单只买 10：挂单方部分成交，不应被归还
    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf);

    EXPECT_EQ(buf.size(), 1u);         // 1 笔成交
    EXPECT_TRUE(deallocated.empty());  // 卖单仍有剩余，不归还
    EXPECT_EQ(sell.filled_qty, 10);
    EXPECT_EQ(sell.status, OrderStatus::PARTIAL);
}
