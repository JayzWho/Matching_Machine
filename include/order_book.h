/**
 * order_book_v1_deque.h
 *
 * 【优化前快照】OrderBook 头文件 — deque 版本
 *
 * 这是 Week 3 完成时的原始版本，使用 std::deque<Order*> 作为 PriceLevel 的
 * 队列实现。保留此文件用于 before/after 对比（对应 git tag: v0.2-before-opt）。
 *
 * 与优化版本（order_book.h）的差异：
 *   - bid_levels_ / ask_levels_ 的值类型为 std::deque<Order*>，
 *     而非自定义 PriceLevel（vector + head 游标）
 *   - 构造函数无 order_index_.reserve(65536)
 *   - match() 中 trades 无 reserve(4)
 *   - cancel_order 直接调用 deque::erase，无惰性删除
 *
 * 详细对比见：docs/optimization_log.md
 */
#pragma once

#include "order.h"
#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <functional>
#include <cstdint>

namespace me {

class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(std::string_view symbol);
    ~OrderBook();

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    std::vector<Trade> add_order(Order* order);
    bool cancel_order(uint64_t order_id);

    [[nodiscard]] int64_t best_bid() const noexcept;
    [[nodiscard]] int64_t best_ask() const noexcept;

    void set_trade_callback(TradeCallback cb) { trade_cb_ = std::move(cb); }

    [[nodiscard]] size_t order_count() const noexcept;

private:
    std::vector<Trade> match(Order* incoming);

    std::string symbol_;
    mutable std::mutex mtx_;

    // 【原始版本】使用 std::deque<Order*> 作为 PriceLevel 队列
    // 缺点：
    //   1. deque 内存布局为"分段数组"（chunk 链），顺序遍历需跨 chunk 跳转，
    //      对 CPU 预取不友好，每个 chunk 指针跳转都可能产生 cache miss。
    //   2. deque::pop_front() 虽然是 O(1)，但释放 chunk 时有额外内存管理开销。
    //   3. 无法通过 reserve 预分配容量。
    std::map<int64_t, std::deque<Order*>, std::greater<int64_t>> bid_levels_;
    std::map<int64_t, std::deque<Order*>>                        ask_levels_;

    // 【原始版本】无 reserve，高频插入时触发 rehash（O(n) 重分配）
    std::unordered_map<uint64_t, Order*> order_index_;

    TradeCallback trade_cb_;
    uint64_t      trade_id_counter_ = 0;
};

} // namespace me
