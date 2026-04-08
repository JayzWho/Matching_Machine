#include "order_book.h"
#include "matching_engine.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace me;

// ── 测试工具函数 ──────────────────────────────────────────────────────────────

static uint64_t g_order_id = 1;

Order make_order(Side side, int64_t price, int64_t qty,
                 OrderType type = OrderType::LIMIT) {
    Order o{};
    o.order_id    = g_order_id++;
    o.side        = side;
    o.price       = price;
    o.quantity    = qty;
    o.type        = type;
    o.timestamp_ns = static_cast<uint64_t>(g_order_id);  // 简单递增保证时间序
    std::strncpy(o.symbol, "TEST", 7);
    return o;
}

// 每个 TEST 前重置 order_id 计数器
class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_order_id = 1;
        book = std::make_unique<OrderBook>("TEST");
    }
    std::unique_ptr<OrderBook> book;
};

// ── 基础功能测试 ──────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, EmptyBookNoBestPrice) {
    EXPECT_EQ(book->best_bid(), 0);
    EXPECT_EQ(book->best_ask(), 0);
}

TEST_F(OrderBookTest, AddBuyOrderNoCrossing) {
    // 买单价格低于卖方（无对手），应挂单
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

// ── 撮合正确性测试 ────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, ExactMatchFullFill) {
    // 买卖价格相等，数量相等 → 完全成交，订单簿清空
    auto buy  = make_order(Side::BUY,  100'000'000, 10);
    auto sell = make_order(Side::SELL, 100'000'000, 10);

    book->add_order(&buy);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[0].price, 100'000'000);
    EXPECT_EQ(trades[0].buy_order_id, buy.order_id);
    EXPECT_EQ(trades[0].sell_order_id, sell.order_id);

    // 成交后订单簿应为空
    EXPECT_EQ(book->best_bid(), 0);
    EXPECT_EQ(book->best_ask(), 0);
    EXPECT_EQ(book->order_count(), 0u);
}

TEST_F(OrderBookTest, PartialFillBuyLarger) {
    // 买 20，卖 10 → 买单部分成交，剩 10 挂单
    auto buy  = make_order(Side::BUY,  100'000'000, 20);
    auto sell = make_order(Side::SELL, 100'000'000, 10);

    book->add_order(&buy);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10);

    EXPECT_EQ(buy.filled_qty,  10);
    EXPECT_EQ(buy.status,      OrderStatus::PARTIAL);
    EXPECT_EQ(sell.status,     OrderStatus::FILLED);

    // 买单剩余 10 手仍在簿
    EXPECT_EQ(book->best_bid(), 100'000'000);
    EXPECT_EQ(book->order_count(), 1u);
}

TEST_F(OrderBookTest, PricePriorityBuyHigherPriceFirst) {
    // 价格优先：高买价先成交
    auto buy_low  = make_order(Side::BUY, 99'000'000, 10);
    auto buy_high = make_order(Side::BUY, 101'000'000, 10);
    auto sell     = make_order(Side::SELL, 99'000'000, 10);

    book->add_order(&buy_low);
    book->add_order(&buy_high);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    // 应与 buy_high（出价更高）成交
    EXPECT_EQ(trades[0].buy_order_id, buy_high.order_id);
    EXPECT_EQ(trades[0].price, 101'000'000);
}

TEST_F(OrderBookTest, TimePriorityFIFO) {
    // 时间优先：同价格下，先进先出
    auto buy1 = make_order(Side::BUY, 100'000'000, 10);  // 先入
    auto buy2 = make_order(Side::BUY, 100'000'000, 10);  // 后入
    auto sell = make_order(Side::SELL, 100'000'000, 10);

    book->add_order(&buy1);
    book->add_order(&buy2);
    auto trades = book->add_order(&sell);

    ASSERT_EQ(trades.size(), 1u);
    // 应与 buy1（先到）成交
    EXPECT_EQ(trades[0].buy_order_id, buy1.order_id);
}

TEST_F(OrderBookTest, MultiLevelMatch) {
    // 多档位穿越：一个大卖单消耗多个买档位
    auto buy1 = make_order(Side::BUY, 102'000'000, 5);
    auto buy2 = make_order(Side::BUY, 101'000'000, 5);
    auto buy3 = make_order(Side::BUY, 100'000'000, 5);
    auto sell = make_order(Side::SELL, 100'000'000, 12);  // 吃掉两个档位

    book->add_order(&buy1);
    book->add_order(&buy2);
    book->add_order(&buy3);
    auto trades = book->add_order(&sell);

    EXPECT_EQ(trades.size(), 3u);
    // 总成交量 = 5 + 5 + 2 = 12
    int64_t total_qty = 0;
    for (const auto& t : trades) total_qty += t.quantity;
    EXPECT_EQ(total_qty, 12);

    // buy1 和 buy2 完全成交，buy3 剩 3 手
    EXPECT_EQ(buy1.status, OrderStatus::FILLED);
    EXPECT_EQ(buy2.status, OrderStatus::FILLED);
    EXPECT_EQ(buy3.filled_qty, 2);
}

// ── 撤单测试 ──────────────────────────────────────────────────────────────────

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
    book->add_order(&sell);  // buy 完全成交

    // 已成交的订单不能撤
    bool result = book->cancel_order(buy.order_id);
    EXPECT_FALSE(result);
}

// ── 回调测试 ──────────────────────────────────────────────────────────────────

TEST_F(OrderBookTest, TradeCallbackInvoked) {
    std::vector<Trade> received_trades;
    book->set_trade_callback([&](const Trade& t) {
        received_trades.push_back(t);
    });

    auto buy  = make_order(Side::BUY,  100'000'000, 10);
    auto sell = make_order(Side::SELL, 100'000'000, 10);
    book->add_order(&buy);
    book->add_order(&sell);

    EXPECT_EQ(received_trades.size(), 1u);
    EXPECT_EQ(received_trades[0].quantity, 10);
}
