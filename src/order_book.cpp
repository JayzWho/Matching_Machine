#include "order_book.h"
#include <chrono>

namespace me {

OrderBook::OrderBook(std::string_view symbol)
    : symbol_(symbol)
{
    // 预分配 order_index_ 容量，避免高频插入时触发 rehash
    // rehash 是 O(n) 且会导致所有桶重分配，在热路径上开销不可控
    order_index_.reserve(65536);
}

OrderBook::~OrderBook() = default;

std::vector<Trade> OrderBook::add_order(Order* order) {
    if (!order) return {};

    std::lock_guard<std::mutex> lock(mtx_);

    // 为订单打上入队时间戳（如果调用方没有设置）
    if (order->timestamp_ns == 0) {
        order->timestamp_ns = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    // 注册到订单索引
    order_index_[order->order_id] = order;

    // 尝试立即撮合
    auto trades = match(order);

    // 如果仍有剩余数量，挂入对应价格档位
    if (!order->is_filled() && order->type == OrderType::LIMIT) {
        if (order->side == Side::BUY) {
            bid_levels_[order->price].push(order);
        } else {
            ask_levels_[order->price].push(order);
        }
    }

    // 触发成交回调
    for (const auto& t : trades) {
        if (trade_cb_) trade_cb_(t);
    }

    return trades;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    Order* order = it->second;
    if (order->status == OrderStatus::FILLED ||
        order->status == OrderStatus::CANCELLED) {
        return false;
    }

    order->status = OrderStatus::CANCELLED;

    // 从对应价格档位中移除（线性扫描，撤单是低频操作，可接受）
    auto remove_from_level = [&](auto& levels) {
        auto level_it = levels.find(order->price);
        if (level_it == levels.end()) return;
        auto& level = level_it->second;
        auto& vec = level.orders;
        for (size_t i = level.head; i < vec.size(); ++i) {
            if (vec[i] && vec[i]->order_id == order_id) {
                vec[i] = nullptr;  // 标记为空洞（惰性删除）
                break;
            }
        }
        // 检查有效元素是否全部为空洞：从 head 扫描找第一个非 null
        bool all_null = true;
        for (size_t i = level.head; i < vec.size(); ++i) {
            if (vec[i] != nullptr) { all_null = false; break; }
        }
        if (all_null) levels.erase(level_it);
    };

    if (order->side == Side::BUY) {
        remove_from_level(bid_levels_);
    } else {
        remove_from_level(ask_levels_);
    }

    order_index_.erase(it);
    return true;
}

int64_t OrderBook::best_bid() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (bid_levels_.empty()) return 0;
    return bid_levels_.begin()->first;
}

int64_t OrderBook::best_ask() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (ask_levels_.empty()) return 0;
    return ask_levels_.begin()->first;
}

size_t OrderBook::order_count() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    return order_index_.size();
}

// ── 核心撮合逻辑（价格-时间优先） ─────────────────────────────────────────────
//
// 规则：
//   买单成交条件：买方出价 >= 卖方最优价
//   卖单成交条件：卖方要价 <= 买方最优价
//   同价格内：先进先出（FIFO），由 PriceLevel 的 push/pop_front 顺序保证
//
// 性能优化：
//   - trades 预分配：小容量的 reserve(4) 覆盖绝大多数正常成交，
//     避免常见情况下的堆重分配
//   - PriceLevel 用 vector + head 游标：连续内存，顺序遍历 cache 友好
//   - nullptr 跳过：cancel_order 用惰性删除，这里需要跳过空洞
//
// 注意：此函数在已持有 mtx_ 的情况下调用，不再加锁。
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;
    trades.reserve(4);  // 绝大多数成交只产生 1-4 笔，避免第一次扩容

    if (incoming->side == Side::BUY) {
        // 买单：与卖方最优价（ask_levels_ 升序，begin() 是最低卖价）撮合
        while (!incoming->is_filled() && !ask_levels_.empty()) {
            auto& [ask_price, ask_level] = *ask_levels_.begin();

            // 买价 < 最优卖价：无法成交，退出
            if (incoming->price < ask_price) break;

            // 跳过撤单留下的空洞（nullptr）
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

                // 更新双方成交数量
                incoming->filled_qty += match_qty;
                resting->filled_qty  += match_qty;

                // 成交价取挂单方价格（价格-时间优先的标准做法）
                Trade& t = trades.emplace_back();
                t.trade_id      = ++trade_id_counter_;
                t.timestamp_ns  = incoming->timestamp_ns;
                t.buy_order_id  = incoming->order_id;
                t.sell_order_id = resting->order_id;
                t.price         = ask_price;
                t.quantity      = match_qty;
                std::memcpy(t.symbol, incoming->symbol, 8);

                // 挂单完全成交 → 从队列移除；部分成交 → 更新状态
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
        // 卖单：与买方最优价（bid_levels_ 降序，begin() 是最高买价）撮合
        while (!incoming->is_filled() && !bid_levels_.empty()) {
            auto& [bid_price, bid_level] = *bid_levels_.begin();

            // 卖价 > 最优买价：无法成交，退出
            if (incoming->price > bid_price) break;

            // 跳过空洞
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
