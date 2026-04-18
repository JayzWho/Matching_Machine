#pragma once

#include "order.h"
#include "trade_ring_buffer.h"
#include <map>
#include <vector>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>
#include <chrono>
#include <cstring>
#include "absl/container/flat_hash_map.h"

namespace me {

/// 内存池归还回调：由 MatchingEngine 注入，用于通知外部归还已成交的挂单对象
/// 消费者线程独占调用（通过 OrderBook 的成交逻辑触发），无需线程安全
using DeallocateCallback = std::function<void(Order*)>;

/// TradeRingBuffer 容量常量（供 OrderBook 接口模板参数使用）
inline constexpr size_t kTradeBufCap = 4096;

/**
 * @brief 订单簿（Order Book）— 单线程无锁版本（v3，进一步优化）
 *
 * 本版本针对 SPSC 架构下 Consumer 线程独占访问的场景，已移除 mutex。
 * OrderBook 不保证线程安全，调用方需确保单线程访问（由 SPSC 保证）。
 *
 * 数据结构选择：
 *   bid_levels_: std::map<int64_t, PriceLevel, std::greater<>>
 *     - map 保证价格从高到低有序（买方最优价在最前）
 *     - PriceLevel（vector + head 游标）保证同价格内按时间先后排列（FIFO）
 *   ask_levels_: std::map<int64_t, PriceLevel>
 *     - 价格从低到高有序（卖方最优价在最前）
 *   order_index_: absl::flat_hash_map<order_id, Order*>
 *     - open-addressing（Swiss Table）替代 std::unordered_map 链地址法
 *     - 桶连续排列，零额外堆分配，cache 局部性显著优于 std::unordered_map
 *     - reserve(65536) 避免 rehash；flat_hash_map 默认 load_factor ≤ 0.875
 *
 * 优化前快照（deque 版本）见：docs/snapshots/order_book_v1_deque.{h,cpp}
 */
class OrderBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(std::string_view symbol);
    ~OrderBook();

    // 禁止拷贝，允许移动
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    /**
     * @brief 新增限价单，并尝试立即撮合
     * @param order 订单指针，生命周期由调用方管理（后续由 MemoryPool 管理）
     * @return 产生的成交列表
     */
    std::vector<Trade> add_order(Order* order);

    /**
     * @brief 无堆分配版 add_order（供 MatchingEngine 消费者线程使用）
     *
     * 与 add_order 逻辑完全相同，区别在于：
     *   - 成交结果直接写入外部 TradeRingBuffer，不构造 std::vector<Trade>
     *   - 当挂单方订单被完全成交时，通过 deallocate_cb_ 通知外部归还内存池
     *
     * 热路径全程无堆分配。调用方（MatchingEngine）须在调用前通过
     * set_deallocate_cb() 注册归还回调，否则挂单方 Order 不会被归还。
     *
     * @tparam Cap  TradeRingBuffer 的容量（模板参数，允许测试使用小容量）
     * @param order     新到达订单的指针
     * @param trade_buf 成交结果写入的环形缓冲区（消费者线程独占）
     */
    template<size_t Cap>
    void add_order_noalloc(Order* order, TradeRingBuffer<Cap>& trade_buf);

    /**
     * @brief 注册 Order 归还回调
     *
     * 每当 match_noalloc 消耗掉一个挂单方 Order（完全成交），就调用此回调。
     * 回调通常是将 Order* 推入归还队列，由生产者线程负责调用 pool.deallocate()。
     *
     * @param cb  callable，签名：void(Order*)
     */
    void set_deallocate_cb(DeallocateCallback cb) { deallocate_cb_ = std::move(cb); }

    /**
     * @brief 撤销订单
     * @return true 撤单成功，false 订单不存在或已成交
     */
    bool cancel_order(uint64_t order_id);

    /// 查询当前最优买价（bid）
    [[nodiscard]] int64_t best_bid() const noexcept;

    /// 查询当前最优卖价（ask）
    [[nodiscard]] int64_t best_ask() const noexcept;

    /// 注册成交回调（异步通知，生产中常用于风控和日志）
    void set_trade_callback(TradeCallback cb) { trade_cb_ = std::move(cb); }

    /// 当前订单总数（用于测试验证）
    [[nodiscard]] size_t order_count() const noexcept;

    /**
     * @brief 清空订单簿，保留已分配的内存（用于 benchmark 内批量复用）
     *
     * 清除所有价格档位和订单索引，重置成交计数器。
     * 注意：调用方负责管理 Order 对象的生命周期，clear() 不会释放 Order 指针。
     */
    void clear() noexcept;

private:
    /// 内部撮合逻辑（返回 vector 版本，供旧接口使用）
    std::vector<Trade> match(Order* incoming);

    /// 无堆分配撮合逻辑（模板版本，成交直接写入 trade_buf，挂单方成交后触发 deallocate_cb_）
    template<size_t Cap>
    void match_noalloc(Order* incoming, TradeRingBuffer<Cap>& trade_buf);

    std::string symbol_;

    /**
     * PriceLevel：同一价格档位下的订单队列。
     *
     * 优化：用 std::vector<Order*> + head_ 游标代替 std::deque<Order*>。
     *
     * 理由：
     *   - deque 的内存布局是"分段数组"，每次追踪指针需要额外的间接层，
     *     在高频撮合时造成不必要的 cache miss。
     *   - vector 所有元素连续存储，顺序遍历对 CPU 预取（prefetch）友好。
     *   - pop_front 用游标 head_++ 代替 erase(begin())，O(1) 且无内存移动。
     *   - 当 head_ 超过容量一半时执行一次 compact（erase dead前缀），
     *     防止 vector 无限增长。
     */
    struct PriceLevel {
        std::vector<Order*> orders;
        size_t head = 0;   ///< 有效元素起始索引（指向队列头部）

        void push(Order* o) { orders.push_back(o); }

        Order* front() const {
            return (head < orders.size()) ? orders[head] : nullptr;
        }

        void pop_front() {
            if (head < orders.size()) {
                ++head;
                // 超过一半是空洞时 compact，均摊 O(1)
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

    // 买方：价格越高越优先（降序）
    std::map<int64_t, PriceLevel, std::greater<int64_t>> bid_levels_;
    // 卖方：价格越低越优先（升序）
    std::map<int64_t, PriceLevel>                        ask_levels_;

    // 订单索引：order_id → Order*，用于 O(1) 撤单
    // absl::flat_hash_map：open-addressing (Swiss Table)，桶连续排列，
    // 零额外堆分配，cache 局部性显著优于 std::unordered_map 的链地址法
    // reserve(65536) 避免高频插入触发 rehash
    absl::flat_hash_map<uint64_t, Order*> order_index_;

    TradeCallback      trade_cb_;
    DeallocateCallback deallocate_cb_;   ///< 挂单方成交后的 Order 归还回调（可选）
    uint64_t           trade_id_counter_ = 0;
};

// ── 模板函数实现（必须在头文件内） ───────────────────────────────────────────

template<size_t Cap>
void OrderBook::add_order_noalloc(Order* order, TradeRingBuffer<Cap>& trade_buf) {
    if (!order) return;

    if (order->timestamp_ns == 0) {
        order->timestamp_ns = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    if (order->type == OrderType::CANCEL) {
        cancel_order(order->order_id);
        if (deallocate_cb_) deallocate_cb_(order);
        return;
    }

    order_index_[order->order_id] = order;
    match_noalloc(order, trade_buf);

    if (!order->is_filled() && order->type == OrderType::LIMIT) {
        if (order->side == Side::BUY) {
            bid_levels_[order->price].push(order);
        } else {
            ask_levels_[order->price].push(order);
        }
    }
}

template<size_t Cap>
void OrderBook::match_noalloc(Order* incoming, TradeRingBuffer<Cap>& trade_buf) {
    if (incoming->side == Side::BUY) {
        while (!incoming->is_filled() && !ask_levels_.empty()) {
            auto& [ask_price, ask_level] = *ask_levels_.begin();
            if (incoming->price < ask_price) break;     //若已无法撮合，跳出循环。

            // 清理空洞
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

                Trade t{};
                t.trade_id      = ++trade_id_counter_;
                t.timestamp_ns  = incoming->timestamp_ns;
                t.buy_order_id  = incoming->order_id;
                t.sell_order_id = resting->order_id;
                t.price         = ask_price;
                t.quantity      = match_qty;
                std::memcpy(t.symbol, incoming->symbol, 8);
                trade_buf.push_trade(t);
                if (trade_cb_) trade_cb_(t);

                if (resting->is_filled()) {
                    resting->status = OrderStatus::FILLED;
                    order_index_.erase(resting->order_id);
                    ask_level.pop_front();
                    if (deallocate_cb_) deallocate_cb_(resting);
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

                Trade t{};
                t.trade_id      = ++trade_id_counter_;
                t.timestamp_ns  = incoming->timestamp_ns;
                t.buy_order_id  = resting->order_id;
                t.sell_order_id = incoming->order_id;
                t.price         = bid_price;
                t.quantity      = match_qty;
                std::memcpy(t.symbol, incoming->symbol, 8);
                trade_buf.push_trade(t);
                if (trade_cb_) trade_cb_(t);

                if (resting->is_filled()) {
                    resting->status = OrderStatus::FILLED;
                    order_index_.erase(resting->order_id);
                    bid_level.pop_front();
                    if (deallocate_cb_) deallocate_cb_(resting);
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
