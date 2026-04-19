#include "feed_simulator.h"
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace me {

FeedSimulator::FeedSimulator(std::string_view symbol,
                             int64_t          base_price,
                             double           cancel_ratio,
                             uint64_t         seed)
    : symbol_(symbol)
    , base_price_(base_price)
    , rng_(seed)
    , price_dist_(0.0, static_cast<double>(base_price) * 0.005)
    , qty_dist_(1, 100)
    , side_dist_(0.5)
    , cancel_dist_(cancel_ratio)
{}

std::vector<Order> FeedSimulator::generate_random(size_t count) {
    std::vector<Order> orders;
    orders.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        bool is_cancel = cancel_dist_(rng_) && (next_order_id_ > 1);
        orders.push_back(make_random_order(is_cancel));
    }
    return orders;
}

std::vector<Order> FeedSimulator::load_csv(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }

    std::vector<Order> orders;
    std::string line;

    // 跳过表头
    std::getline(file, line);

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string token;

        Order o{};
        // 格式：order_id,side,price,qty,type
        std::getline(ss, token, ','); o.order_id  = std::stoull(token);
        std::getline(ss, token, ','); o.side      = (token == "0") ? Side::BUY : Side::SELL;
        std::getline(ss, token, ','); o.price     = std::stoll(token);
        std::getline(ss, token, ','); o.quantity  = std::stoll(token);
        std::getline(ss, token, ','); o.type      = (token == "0") ? OrderType::LIMIT : OrderType::CANCEL;

        std::strncpy(o.symbol, symbol_.c_str(), 7);
        orders.push_back(o);
    }
    return orders;
}

void FeedSimulator::generate_into(Order* slot) {
    bool is_cancel = cancel_dist_(rng_) && (next_order_id_ > 1);
    *slot = make_random_order(is_cancel);
}

Order FeedSimulator::make_random_order(bool is_cancel) {
    Order o{};
    o.order_id     = next_order_id_++;
    o.timestamp_ns = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    o.side         = side_dist_(rng_) ? Side::BUY : Side::SELL;
    o.quantity     = qty_dist_(rng_);
    std::strncpy(o.symbol, symbol_.c_str(), 7);

    if (is_cancel) {
        o.type  = OrderType::CANCEL;
        // 撤销一个已存在的订单（随机选择一个更早的 ID）
        std::uniform_int_distribution<uint64_t> id_dist(1, next_order_id_ - 2);
        o.order_id = id_dist(rng_);
    } else {
        o.type  = OrderType::LIMIT;
        // 价格 = 基准价 + 随机偏移，取整到 100（模拟最小变动单位）
        int64_t offset = static_cast<int64_t>(price_dist_(rng_));
        o.price = base_price_ + (offset / 100) * 100;
        if (o.price <= 0) o.price = base_price_;
    }

    return o;
}

} // namespace me
