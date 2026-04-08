#include "matching_engine.h"
#include <stdexcept>
#include <cstring>

namespace me {

void MatchingEngine::add_symbol(std::string_view symbol, int64_t /*base_price*/) {
    std::string key(symbol);
    if (books_.count(key)) return;  // 已注册，跳过
    books_[key] = std::make_unique<OrderBook>(symbol);
}

std::vector<Trade> MatchingEngine::submit(Order* order) {
    std::string key(order->symbol, strnlen(order->symbol, 8));
    auto it = books_.find(key);
    if (it == books_.end()) {
        // 品种未注册时自动创建（方便测试）
        books_[key] = std::make_unique<OrderBook>(key);
        it = books_.find(key);
    }

    auto trades = it->second->add_order(order);
    total_trades_ += trades.size();
    return trades;
}

OrderBook* MatchingEngine::get_order_book(std::string_view symbol) {
    std::string key(symbol);
    auto it = books_.find(key);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

} // namespace me
