#include "matching_engine.h"
#include "feed_simulator.h"
#include <iostream>
#include <iomanip>

int main() {
    using namespace me;

    std::cout << "=== Low-Latency Matching Engine Demo ===\n\n";

    // Prices are in units of 1e-6: 100'000'000 is 100.000000.
    MatchingEngine engine;
    engine.add_symbol("BTCUSD", 100'000'000);

    FeedSimulator feed("BTCUSD", 100'000'000, /*cancel_ratio=*/0.2, /*seed=*/42);
    auto orders = feed.generate_random(1000);

    size_t total_trades = 0;

    for (auto& order : orders) {
        if (order.type == OrderType::CANCEL) {
            auto* book = engine.get_order_book("BTCUSD");
            if (book) book->cancel_order(order.order_id);
        } else {
            auto trades = engine.submit(&order);
            total_trades += trades.size();
        }
    }

    auto* book = engine.get_order_book("BTCUSD");
    std::cout << "Orders processed : " << orders.size() << "\n";
    std::cout << "Trades generated : " << total_trades << "\n";
    if (book) {
        int64_t bid = book->best_bid();
        int64_t ask = book->best_ask();
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "Best bid         : " << (bid / 1e6) << "\n";
        std::cout << "Best ask         : " << (ask / 1e6) << "\n";
        std::cout << "Spread           : " << ((ask - bid) / 1e6) << "\n";
    }

    std::cout << "\nDone.\n";
    return 0;
}
