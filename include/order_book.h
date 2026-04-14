#pragma once

#include "order.h"
#include <map>
#include <vector>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <cstddef>
#include "absl/container/flat_hash_map.h"

namespace me {

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
    /// 内部撮合逻辑
    std::vector<Trade> match(Order* incoming);

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
    // absl::flat_hash_map：open-addressing (Swiss Table)，桶连续排列，
    // 零额外堆分配，cache 局部性显著优于 std::unordered_map 的链地址法
    // reserve(65536) 避免高频插入触发 rehash
    absl::flat_hash_map<uint64_t, Order*> order_index_;

    TradeCallback trade_cb_;
    uint64_t      trade_id_counter_ = 0;
};

} // namespace me
