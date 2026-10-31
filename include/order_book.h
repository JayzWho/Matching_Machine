#pragma once

#include "order.h"
#include "trade_ring_buffer.h"
#include <map>
#include <vector>
#include <cstdint>
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include "absl/container/flat_hash_map.h"
#include "latency_recorder.h"

namespace me {

inline constexpr size_t kTradeBufCap = 4096;

/**
 * Price-time priority order book for one symbol.
 *
 * Not thread-safe: in the pipeline only the consumer thread touches it. Each side is a
 * `std::map` from price to a FIFO `PriceLevel`, ordered so that `begin()` is the best
 * price. `order_index_` maps the ID of every resting order to the order, for cancels.
 *
 * Preconditions: live order IDs are unique and quantities are positive. The book does
 * not check them. The caller owns the `Order` objects.
 */
class OrderBook {
public:

    explicit OrderBook(std::string_view symbol);
    ~OrderBook();

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    /// Matches a limit order and rests what is left. Accepts limit orders only; cancels
    /// go through `cancel_order()`. Returns the trades, which costs one allocation per call.
    std::vector<Trade> add_order(Order* order);

    /**
     * The pipeline's entry point. Handles both limit orders and cancel requests, writes
     * trades into `trade_buf` instead of returning a vector, and calls `deallocator(Order*)`
     * for every order the book is done with: a filled order, a cancel request and the
     * order it cancelled. Orders and trades are not allocated here, but opening a price
     * level or growing a level's vector still allocates.
     */
    template<size_t Cap, typename Deallocator>
    void add_order_noalloc(Order* order, TradeRingBuffer<Cap>& trade_buf,
                           const Deallocator& deallocator);

    /// Returns false when no resting order has this ID.
    bool cancel_order(uint64_t order_id);

    [[nodiscard]] int64_t best_bid() const noexcept;

    [[nodiscard]] int64_t best_ask() const noexcept;

    /// Number of resting orders.
    [[nodiscard]] size_t order_count() const noexcept;

    [[nodiscard]] size_t index_capacity() const noexcept;

    /// Empties the book but keeps the index's slots, so refilling it does not rehash.
    /// Does not free the orders.
    void clear() noexcept;

private:
    /// Removes a resting order and returns it, or nullptr when there is none.
    Order* cancel_resting(uint64_t order_id);

    std::vector<Trade> match(Order* incoming);

    template<size_t Cap, typename Deallocator>
    void match_noalloc(Order* incoming, TradeRingBuffer<Cap>& trade_buf,
                       const Deallocator& deallocator);

    std::string symbol_;

    /**
     * FIFO queue of one price level: a contiguous vector with a head cursor instead of a
     * deque. Popping advances the cursor; the dead prefix is erased once it is more than
     * half the vector. Cancelled orders leave nullptr holes.
     */
    struct PriceLevel {
        std::vector<Order*> orders;
        size_t head = 0;

        void push(Order* o) { orders.push_back(o); }

        Order* front() const {
            return (head < orders.size()) ? orders[head] : nullptr;
        }

        void pop_front() {
            if (head < orders.size()) {
                ++head;
                // Skip holes so that front() is a live order and a level holding only
                // holes is empty(); matching then erases it.
                while (head < orders.size() && orders[head] == nullptr) ++head;
                if (head > 16 && head > orders.size() / 2) {
                    orders.erase(orders.begin(),
                                 orders.begin() + static_cast<ptrdiff_t>(head));
                    head = 0;
                }
            }
        }

        bool empty() const { return head >= orders.size(); }
        size_t size() const { return orders.size() - head; }
    };

    std::map<int64_t, PriceLevel, std::greater<int64_t>> bid_levels_;
    std::map<int64_t, PriceLevel>                        ask_levels_;

    // Resting orders by ID. A cancel finds the order here, then scans its price level.
    absl::flat_hash_map<uint64_t, Order*> order_index_;

    uint64_t           trade_id_counter_ = 0;
};

template<size_t Cap, typename Deallocator>
void OrderBook::add_order_noalloc(Order* order, TradeRingBuffer<Cap>& trade_buf,
                                  const Deallocator& deallocator) {
    if (!order) return;

    if (order->timestamp_ns == 0) {
        order->timestamp_ns = LatencyRecorder::now();
    }

    if (order->type == OrderType::CANCEL) {
        if (Order* cancelled = cancel_resting(order->order_id)) deallocator(cancelled);
        deallocator(order);
        return;
    }

    order_index_[order->order_id] = order;
    match_noalloc(order, trade_buf, deallocator);

    if (!order->is_filled() && order->type == OrderType::LIMIT) {
        if (order->side == Side::BUY) {
            bid_levels_[order->price].push(order);
        } else {
            ask_levels_[order->price].push(order);
        }
    }

    // A resting order is returned by match_noalloc once a later order fills it.
    if (order->is_filled()) {
        deallocator(order);
    }
}

template<size_t Cap, typename Deallocator>
void OrderBook::match_noalloc(Order* incoming, TradeRingBuffer<Cap>& trade_buf,
                               const Deallocator& deallocator) {
    if (incoming->side == Side::BUY) {
        while (!incoming->is_filled() && !ask_levels_.empty()) {
            auto& [ask_price, ask_level] = *ask_levels_.begin();
            if (incoming->price < ask_price) break;

            while (!ask_level.empty() && ask_level.front() == nullptr)
                ask_level.pop_front();
            if (ask_level.empty()) { ask_levels_.erase(ask_levels_.begin()); continue; }

            while (!incoming->is_filled() && !ask_level.empty()) {
                Order* resting = ask_level.front();
                if (resting == nullptr) { ask_level.pop_front(); continue; }

                int64_t match_qty = std::min(incoming->remaining_qty(),
                                             resting->remaining_qty());
                incoming->filled_qty += match_qty;
                resting->filled_qty  += match_qty;

                Trade& t = trade_buf.alloc_slot();
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
                    deallocator(resting);
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

            while (!bid_level.empty() && bid_level.front() == nullptr)
                bid_level.pop_front();
            if (bid_level.empty()) { bid_levels_.erase(bid_levels_.begin()); continue; }

            while (!incoming->is_filled() && !bid_level.empty()) {
                Order* resting = bid_level.front();
                if (resting == nullptr) { bid_level.pop_front(); continue; }

                int64_t match_qty = std::min(incoming->remaining_qty(),
                                             resting->remaining_qty());
                incoming->filled_qty += match_qty;
                resting->filled_qty  += match_qty;

                Trade& t = trade_buf.alloc_slot();
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
                    deallocator(resting);
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
}

} // namespace me
