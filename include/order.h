#pragma once

#include <cstdint>
#include <cstring>

namespace me {

/// 订单方向
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1,
};

/// 订单类型
enum class OrderType : uint8_t {
    LIMIT  = 0,   ///< 限价单
    CANCEL = 1,   ///< 撤单请求
};

/// 订单状态
enum class OrderStatus : uint8_t {
    NEW       = 0,
    PARTIAL   = 1,   ///< 部分成交
    FILLED    = 2,   ///< 完全成交
    CANCELLED = 3,
};

/**
 * @brief 核心订单结构体
 *
 * alignas(64)：强制按 cache line 对齐，避免两个 Order 对象跨越同一 cache line
 * 导致 false sharing（多核并发时的性能杀手）。
 *
 * 整数定价：price 存储为实际价格 × 1,000,000（6位小数精度）。
 * 例：价格 100.123456 → price = 100123456
 * 理由：浮点数比较不精确（0.1 + 0.2 ≠ 0.3），且整数运算更快。
 */
struct alignas(64) Order {
    uint64_t    order_id      = 0;
    uint64_t    timestamp_ns  = 0;   ///< 纳秒时间戳，用于价格-时间优先排序
    int64_t     price         = 0;   ///< 整数定价，单位：原始价格 × 1e6
    int64_t     quantity      = 0;   ///< 订单数量（手数）
    int64_t     filled_qty    = 0;   ///< 已成交数量
    char        symbol[8]     = {};  ///< 品种代码，如 "BTCUSD\0\0"
    Side        side          = Side::BUY;
    OrderType   type          = OrderType::LIMIT;
    OrderStatus status        = OrderStatus::NEW;

    // padding 保证结构体恰好 64 字节（一个 cache line）
    // 当前字段：8+8+8+8+8+8+1+1+1 = 51 字节，需要 13 字节 padding
    uint8_t     _pad[13]      = {};

    /// 剩余待成交数量
    [[nodiscard]] int64_t remaining_qty() const noexcept {
        return quantity - filled_qty;
    }

    /// 是否完全成交
    [[nodiscard]] bool is_filled() const noexcept {
        return filled_qty >= quantity;
    }
};

// 编译期断言：确保 Order 恰好是 64 字节
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes (one cache line)");

/// 成交记录
struct Trade {
    uint64_t trade_id      = 0;
    uint64_t timestamp_ns  = 0;
    uint64_t buy_order_id  = 0;
    uint64_t sell_order_id = 0;
    int64_t  price         = 0;   ///< 成交价（整数定价）
    int64_t  quantity      = 0;   ///< 成交数量
    char     symbol[8]     = {};
};

} // namespace me
