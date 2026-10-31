#pragma once

#include <cstdint>
#include <cstring>

namespace me {

enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1,
};

enum class OrderType : uint8_t {
    LIMIT  = 0,
    CANCEL = 1,   ///< Cancels the resting order whose ID is `order_id`.
};

enum class OrderStatus : uint8_t {
    NEW       = 0,
    PARTIAL   = 1,
    FILLED    = 2,
    CANCELLED = 3,
};

/**
 * One order, exactly one cache line. Prices are integers in units of 1e-6 so that
 * comparisons are exact.
 */
struct alignas(64) Order {
    uint64_t    order_id      = 0;
    uint64_t    timestamp_ns  = 0;   ///< TSC ticks at submission, despite the name. FIFO order comes from the queue position, not from this field.
    int64_t     price         = 0;   ///< Price × 1e6.
    int64_t     quantity      = 0;
    int64_t     filled_qty    = 0;
    char        symbol[8]     = {};  ///< NUL-padded, e.g. "BTCUSD\0\0".
    Side        side          = Side::BUY;
    OrderType   type          = OrderType::LIMIT;
    OrderStatus status        = OrderStatus::NEW;

    // 51 bytes of fields; pad to 64.
    uint8_t     _pad[13]      = {};

    [[nodiscard]] int64_t remaining_qty() const noexcept {
        return quantity - filled_qty;
    }

    [[nodiscard]] bool is_filled() const noexcept {
        return filled_qty >= quantity;
    }
};

static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes (one cache line)");

struct Trade {
    uint64_t trade_id      = 0;
    uint64_t timestamp_ns  = 0;   ///< The incoming order's `timestamp_ns`, in TSC ticks.
    uint64_t buy_order_id  = 0;
    uint64_t sell_order_id = 0;
    int64_t  price         = 0;   ///< Price × 1e6.
    int64_t  quantity      = 0;
    char     symbol[8]     = {};
};

} // namespace me
