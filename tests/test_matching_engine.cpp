/**
 * MatchingEngine: the single-threaded interface, TradeRingBuffer, the add_order_noalloc
 * path, and the pipeline's lifecycle, thread pinning and return of every order to the pool.
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

// ─── Helpers ─────────────────────────────────────────────────────────────────

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

// ─── Single-threaded interface ───────────────────────────────────────────────

TEST(MatchingEngineTest, SingleThreadSubmit_NoMatch) {
    MatchingEngine engine;
    engine.add_symbol("BTCUSD", 100'000'000);

    Order o = make_order(1, Side::BUY, 99'000'000, 10);
    auto trades = engine.submit(&o);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(engine.total_trades(), 0u);
}

TEST(MatchingEngineTest, SingleThreadSubmit_FullMatch) {
    MatchingEngine engine;
    engine.add_symbol("BTCUSD", 100'000'000);

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    auto t1 = engine.submit(&sell);
    EXPECT_TRUE(t1.empty());

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    auto t2 = engine.submit(&buy);
    ASSERT_EQ(t2.size(), 1u);
    EXPECT_EQ(t2[0].quantity, 10);
    EXPECT_EQ(t2[0].price, 100'000'000);
    EXPECT_EQ(engine.total_trades(), 1u);
}

// ─── TradeRingBuffer ─────────────────────────────────────────────────────────

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
    // Six writes into a capacity of four overwrite the two oldest.
    TradeRingBuffer<4> buf;
    for (uint64_t i = 1; i <= 6; ++i) {
        Trade t{};
        t.trade_id = i;
        buf.push_trade(t);
    }

    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf.total_written(), 6u);

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

// ─── Pipeline ────────────────────────────────────────────────────────────────

TEST(MatchingEnginePipelineTest, StartStop_SmallBatch) {
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, /*order_count=*/1000, /*cancel_ratio=*/0.2, {}, /*record_latency=*/true);
    engine.stop();

    EXPECT_EQ(engine.consumed_count(), 1000u);

    // One queue-time and one service-time sample per order.
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
    engine.start("BTCUSD", 100'000'000, 2000, 0.0);
    engine.stop();

    // 2000 orders trade fewer times than kTradeBufCap, so no trade is overwritten.
    EXPECT_GT(engine.trade_buffer().total_written(), 0u);
}

TEST(MatchingEnginePipelineTest, LargeBatch_NoHang) {
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, 100'000, 0.2);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    engine.stop();

    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
        << "Pipeline took too long (> 30 seconds)";

    EXPECT_EQ(engine.consumed_count(), 100'000u);
}

// Replays the producer's order stream (same default seed) on one thread and returns
// the number of orders left resting.
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

    // Filled and cancelled orders have come back, and every run starts from a full pool,
    // so each run ends with the same count.
    MatchingEngine engine(kCapacity);
    for (int run = 0; run < 3; ++run) {
        engine.start("BTCUSD", 100'000'000, kOrders, 0.2);
        engine.stop();
        EXPECT_EQ(engine.pool_available(), kCapacity - resting) << "run " << run;
    }
}

// ─── Thread pinning ──────────────────────────────────────────────────────────

TEST(MatchingEnginePipelineTest, PinnedThreads_ProcessAllOrders) {
    cpu_set_t set;
    CPU_ZERO(&set);
    ASSERT_EQ(sched_getaffinity(0, sizeof(set), &set), 0);
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);

    // Two CPUs when available, otherwise both threads on one.
    MatchingEngine engine;
    engine.start("BTCUSD", 100'000'000, 1000, 0.2, ThreadCpus{cpus.front(), cpus.back()});
    engine.stop();
    EXPECT_EQ(engine.consumed_count(), 1000u);
    EXPECT_FALSE(engine.running());
}

TEST(MatchingEnginePipelineTest, UnavailableCpu_ThrowsAndStops) {
    const int outside = CPU_SETSIZE - 1;  // no such CPU on this machine
    MatchingEngine engine;
    // The producer cannot start: the consumer exits, the engine is stopped and can start again.
    EXPECT_THROW(engine.start("BTCUSD", 100'000'000, 1000, 0.2, ThreadCpus{outside, -1}), std::system_error);
    EXPECT_FALSE(engine.running());
    EXPECT_THROW(engine.start("BTCUSD", 100'000'000, 1000, 0.2, ThreadCpus{-1, outside}), std::system_error);
    EXPECT_FALSE(engine.running());

    engine.start("BTCUSD", 100'000'000, 1000, 0.2);
    engine.stop();
    EXPECT_EQ(engine.consumed_count(), 1000u);
}

// ─── OrderBook::add_order_noalloc ────────────────────────────────────────────

TEST(OrderBookNoallocTest, NoMatch_NoTradeInBuffer) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    Order buy = make_order(1, Side::BUY, 99'000'000, 10);
    book.add_order_noalloc(&buy, buf, [](Order*) {});

    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buy.filled_qty, 0);
}

TEST(OrderBookNoallocTest, FullMatch_TradeInBuffer) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf, [](Order*) {});
    EXPECT_TRUE(buf.empty());

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

    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    Order sell = make_order(1, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf, deallocator);
    EXPECT_TRUE(deallocated.empty());

    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf, deallocator);

    // Both orders are done: the filled resting sell first, from match_noalloc, then the
    // filled incoming buy.
    ASSERT_EQ(deallocated.size(), 2u);
    EXPECT_EQ(deallocated[0]->order_id, 1u);
    EXPECT_EQ(deallocated[1]->order_id, 2u);
}

TEST(OrderBookNoallocTest, PartialMatch_DeallocateNotCalledForPartialResting) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");

    std::vector<Order*> deallocated;
    auto deallocator = [&](Order* o) { deallocated.push_back(o); };

    Order sell = make_order(1, Side::SELL, 100'000'000, 20);
    book.add_order_noalloc(&sell, buf, deallocator);

    // The buy fills; the partly filled sell keeps resting and is not returned.
    Order buy = make_order(2, Side::BUY, 100'000'000, 10);
    book.add_order_noalloc(&buy, buf, deallocator);

    EXPECT_EQ(buf.size(), 1u);
    ASSERT_EQ(deallocated.size(), 1u);
    EXPECT_EQ(deallocated[0]->order_id, 2u);  // buy：incoming FILLED
    EXPECT_EQ(sell.filled_qty, 10);
    EXPECT_EQ(sell.status, OrderStatus::PARTIAL);
}

// A cancel request carries the ID of the order to cancel.
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

    // The cancelled order comes back first, then the request.
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
    book.add_order_noalloc(&buy, buf, deallocator);
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

    // The target has already filled, so only the request comes back.
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

TEST(OrderBookNoallocTest, FillRemovesLevelLeftWithOnlyCancelledOrders) {
    TradeRingBuffer<64> buf;
    OrderBook book("BTCUSD");
    auto noop = [](Order*) {};

    // Level 100 holds `first` and a cancelled order. Once the sell takes `first`, only a
    // hole is left, so the level must go and the best bid move to the next level.
    Order first     = make_order(1, Side::BUY, 100'000'000, 10);
    Order cancelled = make_order(2, Side::BUY, 100'000'000, 10);
    Order next      = make_order(3, Side::BUY,  99'000'000, 10);
    book.add_order_noalloc(&first, buf, noop);
    book.add_order_noalloc(&cancelled, buf, noop);
    book.add_order_noalloc(&next, buf, noop);
    Order cancel = make_cancel(2);
    book.add_order_noalloc(&cancel, buf, noop);

    Order sell = make_order(4, Side::SELL, 100'000'000, 10);
    book.add_order_noalloc(&sell, buf, noop);

    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0].buy_order_id, 1u);
    EXPECT_EQ(book.best_bid(), 99'000'000);
}
