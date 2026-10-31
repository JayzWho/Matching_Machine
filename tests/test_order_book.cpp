#include "order_book.h"
#include "matching_engine.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace me;

// ── Helpers ──────────────────────────────────────────────────────────────────

static uint64_t g_order_id = 1;

Order make_order(Side side, int64_t price, int64_t qty,
                 OrderType type = OrderType::LIMIT) {
    Order o{};
    o.order_id    = g_order_id++;
    o.side        = side;
    o.price       = price;
    o.quantity    = qty;
    o.type        = type;
    o.timestamp_ns = static_cast<uint64_t>(g_order_id);
    std::strncpy(o.symbol, "TEST", 7);
    return o;
}

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_order_id = 1;
        book = std::make_unique<OrderBook>("TEST");
    }
    std::unique_ptr<OrderBook> book;
};

// ── Basics ───────────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, EmptyBookNoBestPrice) {
    EXPECT_EQ(book->best_bid(), 0);
    EXPECT_EQ(book->best_ask(), 0);
}

TEST_F(OrderBookTest, AddBuyOrderNoCrossing) {
    auto o = make_order(Side::BUY, 99'000'000, 10);
    auto trades = book->add_order(&o);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book->best_bid(), 99'000'000);
    EXPECT_EQ(book->order_count(), 1u);
}

TEST_F(OrderBookTest, AddSellOrderNoCrossing) {
    auto o = make_order(Side::SELL, 101'000'000, 10);
    auto trades = book->add_order(&o);

    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book->best_ask(), 101'000'000);
}

// ── Matching ─────────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, ExactMatchFullFill) {
    auto buy  = make_order(Side::BUY,  100'000'000, 10);
    auto sell = make_order(Side::SELL, 100'000'000, 10);

    book->add_order(&buy);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[0].price, 100'000'000);
    EXPECT_EQ(trades[0].buy_order_id, buy.order_id);
    EXPECT_EQ(trades[0].sell_order_id, sell.order_id);

    EXPECT_EQ(book->best_bid(), 0);
    EXPECT_EQ(book->best_ask(), 0);
    EXPECT_EQ(book->order_count(), 0u);
}

TEST_F(OrderBookTest, PartialFillBuyLarger) {
    auto buy  = make_order(Side::BUY,  100'000'000, 20);
    auto sell = make_order(Side::SELL, 100'000'000, 10);

    book->add_order(&buy);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10);

    EXPECT_EQ(buy.filled_qty,  10);
    EXPECT_EQ(buy.status,      OrderStatus::PARTIAL);
    EXPECT_EQ(sell.status,     OrderStatus::FILLED);

    EXPECT_EQ(book->best_bid(), 100'000'000);
    EXPECT_EQ(book->order_count(), 1u);
}

TEST_F(OrderBookTest, PricePriorityBuyHigherPriceFirst) {
    // Price priority: the higher bid trades first.
    auto buy_low  = make_order(Side::BUY, 99'000'000, 10);
    auto buy_high = make_order(Side::BUY, 101'000'000, 10);
    auto sell     = make_order(Side::SELL, 99'000'000, 10);

    book->add_order(&buy_low);
    book->add_order(&buy_high);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_order_id, buy_high.order_id);
    EXPECT_EQ(trades[0].price, 101'000'000);
}

TEST_F(OrderBookTest, TimePriorityFIFO) {
    // Time priority: at one price, first in, first out.
    auto buy1 = make_order(Side::BUY, 100'000'000, 10);
    auto buy2 = make_order(Side::BUY, 100'000'000, 10);
    auto sell = make_order(Side::SELL, 100'000'000, 10);

    book->add_order(&buy1);
    book->add_order(&buy2);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_order_id, buy1.order_id);
}

TEST_F(OrderBookTest, MultiLevelMatch) {
    // One sell sweeps several bid levels.
    auto buy1 = make_order(Side::BUY, 102'000'000, 5);
    auto buy2 = make_order(Side::BUY, 101'000'000, 5);
    auto buy3 = make_order(Side::BUY, 100'000'000, 5);
    auto sell = make_order(Side::SELL, 100'000'000, 12);

    book->add_order(&buy1);
    book->add_order(&buy2);
    book->add_order(&buy3);
    auto trades = book->add_order(&sell);

    EXPECT_EQ(trades.size(), 3u);
    int64_t total_qty = 0;
    for (const auto& t : trades) total_qty += t.quantity;
    EXPECT_EQ(total_qty, 12);

    EXPECT_EQ(buy1.status, OrderStatus::FILLED);
    EXPECT_EQ(buy2.status, OrderStatus::FILLED);
    EXPECT_EQ(buy3.filled_qty, 2);
}

// ── Cancels ──────────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, CancelOrderSuccess) {
    auto buy = make_order(Side::BUY, 100'000'000, 10);
    book->add_order(&buy);
    EXPECT_EQ(book->order_count(), 1u);

    bool result = book->cancel_order(buy.order_id);
    EXPECT_TRUE(result);
    EXPECT_EQ(book->order_count(), 0u);
    EXPECT_EQ(book->best_bid(), 0);
}

TEST_F(OrderBookTest, CancelNonExistentOrder) {
    bool result = book->cancel_order(99999);
    EXPECT_FALSE(result);
}

TEST_F(OrderBookTest, CancelAlreadyFilledOrder) {
    auto buy  = make_order(Side::BUY,  100'000'000, 10);
    auto sell = make_order(Side::SELL, 100'000'000, 10);
    book->add_order(&buy);
    book->add_order(&sell);

    // A filled order cannot be cancelled.
    bool result = book->cancel_order(buy.order_id);
    EXPECT_FALSE(result);
}

TEST_F(OrderBookTest, FillRemovesLevelLeftWithOnlyCancelledOrders) {
    // Level 100 holds `first` and a cancelled order. Once the buy takes `first`, only a
    // hole is left, so the level must go and the best ask move to the next level.
    auto first     = make_order(Side::SELL, 100'000'000, 10);
    auto cancelled = make_order(Side::SELL, 100'000'000, 10);
    auto next      = make_order(Side::SELL, 101'000'000, 10);
    book->add_order(&first);
    book->add_order(&cancelled);
    book->add_order(&next);
    ASSERT_TRUE(book->cancel_order(cancelled.order_id));

    auto buy = make_order(Side::BUY, 100'000'000, 10);
    auto trades = book->add_order(&buy);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_order_id, first.order_id);
    EXPECT_EQ(book->best_ask(), 101'000'000);
}

// ── clear() ──────────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, ClearKeepsIndexCapacity) {
    // After clear() the index keeps the slots of a new book, so refilling does not grow it.
    const size_t fresh = book->index_capacity();
    std::vector<Order> orders;
    orders.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        orders.push_back(make_order(Side::BUY, 99'000'000 - i * 100, 1));
        book->add_order(&orders.back());
    }

    book->clear();

    EXPECT_EQ(book->order_count(), 0u);
    EXPECT_EQ(book->best_bid(), 0);
    EXPECT_EQ(book->index_capacity(), fresh);
}

// ── Trades ───────────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, TradeRecordedInRingBuffer) {
    TradeRingBuffer<64> trade_buf;

    auto buy  = make_order(Side::BUY,  100'000'000, 10);
    auto sell = make_order(Side::SELL, 100'000'000, 10);
    auto noop = [](Order*) {};
    book->add_order_noalloc(&buy,  trade_buf, noop);
    book->add_order_noalloc(&sell, trade_buf, noop);

    EXPECT_EQ(trade_buf.size(), 1u);
    EXPECT_EQ(trade_buf[0].quantity, 10);
}
