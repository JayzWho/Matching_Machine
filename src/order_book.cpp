#include "order_book.h"
#include <algorithm>

namespace me {

OrderBook::OrderBook(std::string_view symbol)
    : symbol_(symbol)
{
    // Reserve so that inserts do not rehash on the hot path.
    order_index_.reserve(65536);
}

OrderBook::~OrderBook() = default;

std::vector<Trade> OrderBook::add_order(Order* order) {
    if (!order) return {};

    if (order->timestamp_ns == 0) {
        order->timestamp_ns = LatencyRecorder::now();
    }

    order_index_[order->order_id] = order;

    auto trades = match(order);

    if (!order->is_filled() && order->type == OrderType::LIMIT) {
        if (order->side == Side::BUY) {
            bid_levels_[order->price].push(order);
        } else {
            ask_levels_[order->price].push(order);
        }
    }

    return trades;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    return cancel_resting(order_id) != nullptr;
}

Order* OrderBook::cancel_resting(uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return nullptr;

    Order* order = it->second;
    if (order->status == OrderStatus::FILLED ||
        order->status == OrderStatus::CANCELLED) {
        return nullptr;
    }

    order->status = OrderStatus::CANCELLED;

    // Linear scan of the order's price level.
    auto remove_from_level = [&](auto& levels) {
        auto level_it = levels.find(order->price);
        if (level_it == levels.end()) return;
        auto& level = level_it->second;
        auto& vec = level.orders;
        for (size_t i = level.head; i < vec.size(); ++i) {
            if (vec[i] && vec[i]->order_id == order_id) {
                vec[i] = nullptr;  // a hole; matching skips it
                break;
            }
        }
        // Advance past leading holes and erase the level if nothing live remains; empty()
        // looks only at the cursor.
        while (level.head < vec.size() && vec[level.head] == nullptr)
            ++level.head;
        if (level.head >= vec.size()) levels.erase(level_it);
    };

    if (order->side == Side::BUY) {
        remove_from_level(bid_levels_);
    } else {
        remove_from_level(ask_levels_);
    }

    order_index_.erase(it);
    return order;
}

int64_t OrderBook::best_bid() const noexcept {
    if (bid_levels_.empty()) return 0;
    return bid_levels_.begin()->first;
}

int64_t OrderBook::best_ask() const noexcept {
    if (ask_levels_.empty()) return 0;
    return ask_levels_.begin()->first;
}

size_t OrderBook::order_count() const noexcept {
    return order_index_.size();
}

size_t OrderBook::index_capacity() const noexcept {
    return order_index_.capacity();
}

void OrderBook::clear() noexcept {
    bid_levels_.clear();
    ask_levels_.clear();
    // Not order_index_.clear(): from 128 slots up, absl's clear() frees the table and
    // later inserts grow it again from one slot. Erasing the whole range keeps the slots.
    order_index_.erase(order_index_.begin(), order_index_.end());
    trade_id_counter_ = 0;
}

std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;
    trades.reserve(4);  // one allocation per call, even when nothing trades

    if (incoming->side == Side::BUY) {
        while (!incoming->is_filled() && !ask_levels_.empty()) {
            auto& [ask_price, ask_level] = *ask_levels_.begin();

            if (incoming->price < ask_price) break;

            while (!ask_level.empty() && ask_level.front() == nullptr) {
                ask_level.pop_front();
            }
            if (ask_level.empty()) {
                ask_levels_.erase(ask_levels_.begin());
                continue;
            }

            while (!incoming->is_filled() && !ask_level.empty()) {
                Order* resting = ask_level.front();
                if (resting == nullptr) {
                    ask_level.pop_front();
                    continue;
                }

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
                    ask_level.pop_front();
                } else {
                    resting->status = OrderStatus::PARTIAL;
                }
            }

            if (ask_level.empty()) ask_levels_.erase(ask_levels_.begin());
        }

        if (incoming->is_filled()) {
            incoming->status = OrderStatus::FILLED;
            order_index_.erase(incoming->order_id);
        } else if (incoming->filled_qty > 0) {
            incoming->status = OrderStatus::PARTIAL;
        }

    } else { // SELL
        while (!incoming->is_filled() && !bid_levels_.empty()) {
            auto& [bid_price, bid_level] = *bid_levels_.begin();

            if (incoming->price > bid_price) break;

            while (!bid_level.empty() && bid_level.front() == nullptr) {
                bid_level.pop_front();
            }
            if (bid_level.empty()) {
                bid_levels_.erase(bid_levels_.begin());
                continue;
            }

            while (!incoming->is_filled() && !bid_level.empty()) {
                Order* resting = bid_level.front();
                if (resting == nullptr) {
                    bid_level.pop_front();
                    continue;
                }

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
                    bid_level.pop_front();
                } else {
                    resting->status = OrderStatus::PARTIAL;
                }
            }

            if (bid_level.empty()) bid_levels_.erase(bid_levels_.begin());
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
