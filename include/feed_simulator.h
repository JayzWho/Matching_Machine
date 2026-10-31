#pragma once

#include "order.h"
#include <string_view>
#include <vector>
#include <random>
#include <fstream>
#include <cstdint>

namespace me {

/**
 * Synthetic, seeded order flow. Limit prices are drawn from a normal distribution around
 * a fixed base price and rounded to a tick of 100; a cancel targets an earlier order ID
 * chosen uniformly, which often no longer rests. Also reads orders from a CSV file.
 */
class FeedSimulator {
public:
    /// `base_price` is in units of 1e-6, like Order::price.
    FeedSimulator(std::string_view symbol,
                  int64_t          base_price,
                  double           cancel_ratio = 0.2,
                  uint64_t         seed         = 42);

    std::vector<Order> generate_random(size_t count);

    /// Writes the next order into a slot the caller has allocated.
    void generate_into(Order* slot);

    /// Columns: order_id,side,price,qty,type with side 0 = BUY, 1 = SELL and
    /// type 0 = LIMIT, 1 = CANCEL.
    std::vector<Order> load_csv(const std::string& filepath);

private:
    Order make_random_order(bool is_cancel = false);

    std::string symbol_;
    int64_t     base_price_;
    uint64_t    next_order_id_ = 1;

    std::mt19937_64                    rng_;
    std::normal_distribution<double>   price_dist_;   // offset from base_price
    std::uniform_int_distribution<int> qty_dist_;
    std::bernoulli_distribution        side_dist_;
    std::bernoulli_distribution        cancel_dist_;
};

} // namespace me
