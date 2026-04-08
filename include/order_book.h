#pragma once

#include "order.h"
#include <map>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <cstdint>

namespace me {

/**
 * @brief 订单簿（Order Book）— mutex 初版
 *
 * 本版本使用 std::mutex 保证线程安全，作为 lock-free 版本的功能基线。
 * 后续 Week 4-5 会用 SPSC ring buffer 将接收线程与处理线程解耦，
 * 届时 OrderBook 可以运行在单线程上，mutex 可以移除。
 *
 * 数据结构选择：
 *   bid_levels_: std::map<int64_t, std::deque<Order*>, std::greater<>>
 *     - map 保证价格从高到低有序（买方最优价在最前）
 *     - deque 保证同价格内按时间先后排列（FIFO = 时间优先）
 *   ask_levels_: std::map<int64_t, std::deque<Order*>>
 *     - 价格从低到高有序（卖方最优价在最前）
 *   order_index_: unordered_map<order_id, Order*>
 *     - O(1) 查找，用于快速撤单
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

private:
    /// 内部撮合逻辑（在已持有锁的情况下调用）
    std::vector<Trade> match(Order* incoming);

    std::string symbol_;
    mutable std::mutex mtx_;

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
                if (head > orders.size() / 2 && head > 16) {
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
    // reserve(65536) 避免高频插入触发 rehash
    std::unordered_map<uint64_t, Order*> order_index_;

    TradeCallback trade_cb_;
    uint64_t      trade_id_counter_ = 0;
};

} // namespace me
