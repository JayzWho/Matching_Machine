#pragma once

#include "order_book.h"
#include "order.h"
#include <vector>
#include <string_view>
#include <string>
#include <unordered_map>
#include <memory>

namespace me {

/**
 * @brief 撮合引擎门面类
 *
 * 当前阶段（Week 1-3）的 MatchingEngine 是对 OrderBook 的轻量封装，
 * 负责：
 *   1. 管理多个品种的 OrderBook
 *   2. 统一接收订单并路由到对应 OrderBook
 *   3. 汇总成交记录（供测试和日志使用）
 *
 * Week 7-8 升级后，这里将接入 SPSC ring buffer，
 * 变成真正的生产者-消费者架构。
 */
class MatchingEngine {
public:
    MatchingEngine() = default;

    /// 注册一个品种的订单簿
    void add_symbol(std::string_view symbol, int64_t base_price);

    /// 提交订单（单线程版本，Week 7 后升级为 lock-free 多线程）
    std::vector<Trade> submit(Order* order);

    /// 获取指定品种的订单簿（用于测试查询）
    OrderBook* get_order_book(std::string_view symbol);

    /// 总成交笔数（监控指标）
    [[nodiscard]] size_t total_trades() const noexcept { return total_trades_; }

private:
    // symbol → OrderBook 映射
    // 用 string key + unique_ptr 避免 map rehash 时 OrderBook 被移动
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    size_t total_trades_ = 0;
};

} // namespace me
