/**
 * order_book_v1.h
 *
 * 优化前的订单簿（v1），只供基准对比用：
 *   - bench_baseline_mutex 的 mutex 流水线用它撮合；
 *   - bench_order_book 的单线程基准把它和当前订单簿（me::OrderBook）放进同一段基准代码，各跑一遍。
 */

#pragma once

#include "order.h"            // Order, Trade, Side, OrderType, OrderStatus
#include "latency_recorder.h" // LatencyRecorder::now()

#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <string>
#include <string_view>
#include <algorithm>
#include <cstring>

// ─── OrderBookV1：v1 deque 版本（me_v1 命名空间隔离） ─────────────────────────
//
// 逻辑与 docs/snapshots/order_book_v1_deque.{h,cpp}（见 tag archive/pre-cleanup-2026-10）完全相同，
// 保留所有"未优化"特征：
//   - std::deque<Order*> 作为 PriceLevel 队列（非 cache-friendly）
//   - std::unordered_map 无 reserve（高频插入触发 rehash）
//   - cancel_order 用 deque::erase（O(n) 移动）
//   - match() 返回 std::vector<Trade>（每次堆分配）
//   - add_order / cancel_order / best_bid / best_ask 均持 std::mutex

namespace me_v1 {

class OrderBookV1 {
public:
    explicit OrderBookV1(std::string_view symbol)
        : symbol_(symbol) {}

    ~OrderBookV1() = default;

    OrderBookV1(const OrderBookV1&) = delete;
    OrderBookV1& operator=(const OrderBookV1&) = delete;

    // ── 核心接口 ──────────────────────────────────────────────────────────────

    std::vector<me::Trade> add_order(me::Order* order) {
        if (!order) return {};

        std::lock_guard<std::mutex> lock(mtx_);

        // 兜底时间戳（若生产者已写入 RDTSC，则此处跳过）
        if (order->timestamp_ns == 0) {
            order->timestamp_ns = me::LatencyRecorder::now();
        }

        order_index_[order->order_id] = order;
        auto trades = match(order);

        if (!order->is_filled() && order->type == me::OrderType::LIMIT) {
            if (order->side == me::Side::BUY) {
                bid_levels_[order->price].push_back(order);
            } else {
                ask_levels_[order->price].push_back(order);
            }
        }

        return trades;
    }

    bool cancel_order(uint64_t order_id) {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = order_index_.find(order_id);
        if (it == order_index_.end()) return false;

        me::Order* order = it->second;
        if (order->status == me::OrderStatus::FILLED ||
            order->status == me::OrderStatus::CANCELLED) {
            return false;
        }

        order->status = me::OrderStatus::CANCELLED;

        // 【原始版本】线性扫描 deque，用 erase 直接删除（O(n)）
        auto remove_from_level = [&](auto& levels) {
            auto level_it = levels.find(order->price);
            if (level_it == levels.end()) return;
            auto& dq = level_it->second;
            for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
                if ((*dit)->order_id == order_id) {
                    dq.erase(dit);
                    break;
                }
            }
            if (dq.empty()) levels.erase(level_it);
        };

        if (order->side == me::Side::BUY) {
            remove_from_level(bid_levels_);
        } else {
            remove_from_level(ask_levels_);
        }

        order_index_.erase(it);
        return true;
    }

    [[nodiscard]] int64_t best_bid() const noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return bid_levels_.empty() ? 0 : bid_levels_.begin()->first;
    }

    [[nodiscard]] int64_t best_ask() const noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return ask_levels_.empty() ? 0 : ask_levels_.begin()->first;
    }

    [[nodiscard]] size_t order_count() const noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return order_index_.size();
    }

    /// 清空订单簿（benchmark 迭代间复用）
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        bid_levels_.clear();
        ask_levels_.clear();
        order_index_.clear();
        trade_id_counter_ = 0;
    }

private:
    // ── 核心撮合逻辑（价格-时间优先）────────────────────────────────────────
    // 【原始版本】trades 无预分配 reserve，首次 emplace_back 必然触发堆分配
    std::vector<me::Trade> match(me::Order* incoming) {
        std::vector<me::Trade> trades;  // 无 reserve

        if (incoming->side == me::Side::BUY) {
            while (!incoming->is_filled() && !ask_levels_.empty()) {
                auto& [ask_price, ask_dq] = *ask_levels_.begin();
                if (incoming->price < ask_price) break;

                while (!incoming->is_filled() && !ask_dq.empty()) {
                    me::Order* resting = ask_dq.front();

                    int64_t match_qty = std::min(incoming->remaining_qty(),
                                                 resting->remaining_qty());
                    incoming->filled_qty += match_qty;
                    resting->filled_qty  += match_qty;

                    me::Trade& t = trades.emplace_back();
                    t.trade_id      = ++trade_id_counter_;
                    t.timestamp_ns  = incoming->timestamp_ns;
                    t.buy_order_id  = incoming->order_id;
                    t.sell_order_id = resting->order_id;
                    t.price         = ask_price;
                    t.quantity      = match_qty;
                    std::memcpy(t.symbol, incoming->symbol, 8);

                    if (resting->is_filled()) {
                        resting->status = me::OrderStatus::FILLED;
                        order_index_.erase(resting->order_id);
                        ask_dq.pop_front();
                    } else {
                        resting->status = me::OrderStatus::PARTIAL;
                    }
                }
                if (ask_dq.empty()) ask_levels_.erase(ask_levels_.begin());
            }

            if (incoming->is_filled()) {
                incoming->status = me::OrderStatus::FILLED;
                order_index_.erase(incoming->order_id);
            } else if (incoming->filled_qty > 0) {
                incoming->status = me::OrderStatus::PARTIAL;
            }

        } else { // SELL
            while (!incoming->is_filled() && !bid_levels_.empty()) {
                auto& [bid_price, bid_dq] = *bid_levels_.begin();
                if (incoming->price > bid_price) break;

                while (!incoming->is_filled() && !bid_dq.empty()) {
                    me::Order* resting = bid_dq.front();

                    int64_t match_qty = std::min(incoming->remaining_qty(),
                                                 resting->remaining_qty());
                    incoming->filled_qty += match_qty;
                    resting->filled_qty  += match_qty;

                    me::Trade& t = trades.emplace_back();
                    t.trade_id      = ++trade_id_counter_;
                    t.timestamp_ns  = incoming->timestamp_ns;
                    t.buy_order_id  = resting->order_id;
                    t.sell_order_id = incoming->order_id;
                    t.price         = bid_price;
                    t.quantity      = match_qty;
                    std::memcpy(t.symbol, incoming->symbol, 8);

                    if (resting->is_filled()) {
                        resting->status = me::OrderStatus::FILLED;
                        order_index_.erase(resting->order_id);
                        bid_dq.pop_front();
                    } else {
                        resting->status = me::OrderStatus::PARTIAL;
                    }
                }
                if (bid_dq.empty()) bid_levels_.erase(bid_levels_.begin());
            }

            if (incoming->is_filled()) {
                incoming->status = me::OrderStatus::FILLED;
                order_index_.erase(incoming->order_id);
            } else if (incoming->filled_qty > 0) {
                incoming->status = me::OrderStatus::PARTIAL;
            }
        }

        return trades;
    }

    std::string symbol_;
    mutable std::mutex mtx_;

    // 【原始版本】std::deque<Order*>（非 cache-friendly 分段数组布局）
    std::map<int64_t, std::deque<me::Order*>, std::greater<int64_t>> bid_levels_;
    std::map<int64_t, std::deque<me::Order*>>                        ask_levels_;

    // 【原始版本】无 reserve，高频插入触发 rehash
    std::unordered_map<uint64_t, me::Order*> order_index_;

    uint64_t trade_id_counter_ = 0;
};

} // namespace me_v1
