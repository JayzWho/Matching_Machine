/**
 * order_book_v1_deque.cpp
 *
 * 【优化前快照】OrderBook 实现 — deque 版本
 *
 * 对应 git tag: v0.2-before-opt
 * 详细对比见：docs/optimization_log.md
 */
#include "order_book.h"
#include <chrono>

namespace me {

// 【原始版本】构造函数：无 order_index_ 预分配
OrderBook::OrderBook(std::string_view symbol)
    : symbol_(symbol)
{}

OrderBook::~OrderBook() = default;

std::vector<Trade> OrderBook::add_order(Order* order) {
    if (!order) return {};

    std::lock_guard<std::mutex> lock(mtx_);

    if (order->timestamp_ns == 0) {
        order->timestamp_ns = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    order_index_[order->order_id] = order;

    auto trades = match(order);

    if (!order->is_filled() && order->type == OrderType::LIMIT) {
        if (order->side == Side::BUY) {
            bid_levels_[order->price].push_back(order);
        } else {
            ask_levels_[order->price].push_back(order);
        }
    }

    for (const auto& t : trades) {
        if (trade_cb_) trade_cb_(t);
    }

    return trades;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    Order* order = it->second;
    if (order->status == OrderStatus::FILLED ||
        order->status == OrderStatus::CANCELLED) {
        return false;
    }

    order->status = OrderStatus::CANCELLED;

    // 【原始版本】线性扫描 deque 并用 erase 直接删除元素（非惰性）
    auto remove_from_level = [&](auto& levels) {
        auto level_it = levels.find(order->price);
        if (level_it == levels.end()) return;
        auto& dq = level_it->second;
        for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
            if ((*dit)->order_id == order_id) {
                dq.erase(dit);  // O(n) 移动后续元素
                break;
            }
        }
        if (dq.empty()) levels.erase(level_it);
    };

    if (order->side == Side::BUY) {
        remove_from_level(bid_levels_);
    } else {
        remove_from_level(ask_levels_);
    }

    order_index_.erase(it);
    return true;
}

int64_t OrderBook::best_bid() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (bid_levels_.empty()) return 0;
    return bid_levels_.begin()->first;
}

int64_t OrderBook::best_ask() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (ask_levels_.empty()) return 0;
    return ask_levels_.begin()->first;
}

size_t OrderBook::order_count() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    return order_index_.size();
}

// ── 核心撮合逻辑（价格-时间优先） ─────────────────────────────────────────────
//
// 【原始版本】trades 无预分配 reserve，每次都从空 vector 开始，
//            首次 push_back 时必然触发堆分配。
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;  // 无 reserve，首次 emplace_back 触发堆分配

    if (incoming->side == Side::BUY) {
        while (!incoming->is_filled() && !ask_levels_.empty()) {
            auto& [ask_price, ask_dq] = *ask_levels_.begin();

            if (incoming->price < ask_price) break;

            while (!incoming->is_filled() && !ask_dq.empty()) {
                Order* resting = ask_dq.front();

                int64_t match_qty = std::min(
                    incoming->remaining_qty(),
                    resting->remaining_qty()
                );

                incoming->filled_qty += match_qty;
                resting->filled_qty  += match_qty;

                Trade& t = trades.emplace_back();
                t.trade_id      = ++trade_id_counter_;
                t.timestamp_ns  = incoming->timestamp_ns;
                t.buy_order_id  = incoming->order_id;
                t.sell_order_id = resting->order_id;
                t.price         = ask_price;
                t.quantity      = match_qty;
                std::memcpy(t.symbol, incoming->symbol, 8);

                if (resting->is_filled()) {
                    resting->status = OrderStatus::FILLED;
                    order_index_.erase(resting->order_id);
                    ask_dq.pop_front();  // deque::pop_front()
                } else {
                    resting->status = OrderStatus::PARTIAL;
                }
            }

            if (ask_dq.empty()) ask_levels_.erase(ask_levels_.begin());
        }

        if (incoming->is_filled()) {
            incoming->status = OrderStatus::FILLED;
            order_index_.erase(incoming->order_id);
        } else if (incoming->filled_qty > 0) {
            incoming->status = OrderStatus::PARTIAL;
        }

    } else { // SELL
        while (!incoming->is_filled() && !bid_levels_.empty()) {
            auto& [bid_price, bid_dq] = *bid_levels_.begin();

            if (incoming->price > bid_price) break;

            while (!incoming->is_filled() && !bid_dq.empty()) {
                Order* resting = bid_dq.front();

                int64_t match_qty = std::min(
                    incoming->remaining_qty(),
                    resting->remaining_qty()
                );

                incoming->filled_qty += match_qty;
                resting->filled_qty  += match_qty;

                Trade& t = trades.emplace_back();
                t.trade_id      = ++trade_id_counter_;
                t.timestamp_ns  = incoming->timestamp_ns;
                t.buy_order_id  = resting->order_id;
                t.sell_order_id = incoming->order_id;
                t.price         = bid_price;
                t.quantity      = match_qty;
                std::memcpy(t.symbol, incoming->symbol, 8);

                if (resting->is_filled()) {
                    resting->status = OrderStatus::FILLED;
                    order_index_.erase(resting->order_id);
                    bid_dq.pop_front();
                } else {
                    resting->status = OrderStatus::PARTIAL;
                }
            }

            if (bid_dq.empty()) bid_levels_.erase(bid_levels_.begin());
        }

        if (incoming->is_filled()) {
            incoming->status = OrderStatus::FILLED;
            order_index_.erase(incoming->order_id);
        } else if (incoming->filled_qty > 0) {
            incoming->status = OrderStatus::PARTIAL;
        }
    }

    return trades;
}

} // namespace me
