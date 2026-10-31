/**
 * Order book microbenchmarks. Each benchmark is a template over the book type and is
 * registered twice: the _V3 instance runs the current me::OrderBook, the _V1 instance the
 * book from before the optimizations (me_v1::OrderBookV1, order_book_v1.h). Both go
 * through the same code and call add_order / cancel_order.
 *
 * Most benchmarks rebuild their book every kBatchSize iterations inside PauseTiming, so
 * every timed call meets the same book state. A counter checks that each iteration did
 * the work it claims, and the benchmark reports an error otherwise.
 *
 * Run through scripts/record/bench_record.py; see docs/BENCHMARKING.md.
 */

#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "order_book.h"
#include "order_book_v1.h"
#include "feed_simulator.h"

using namespace me;
using me_v1::OrderBookV1;

// ── Helpers ─────────────────────────────────────────────────────────────────

// The timestamp is preset because the book stamps an order whose timestamp is 0, while
// the pipeline's producer always stamps first. Without it an order object's first timed
// call would read the TSC and its later reuses would not.
static Order make_order(uint64_t id, Side side, int64_t price, int64_t qty) {
    Order o{};
    o.order_id     = id;
    o.side         = side;
    o.price        = price;
    o.quantity     = qty;
    o.type         = OrderType::LIMIT;
    o.timestamp_ns = 1;
    std::strncpy(o.symbol, "BTCUSD", 7);
    return o;
}

// Returns an order to its state before it entered a book, for reuse in a new book.
static void reset(Order& o) {
    o.filled_qty = 0;
    o.status     = OrderStatus::NEW;
}

// ── Add without a match ─────────────────────────────────────────────────────
// Each iteration rests a buy below 100 ask levels. The book is rebuilt every batch, so
// every add meets the same depth. v3's add_order reserves room for trades before
// matching, so its timed region holds one malloc/free pair although nothing trades;
// v1 allocates only when a trade occurs.
template <class Book>
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    constexpr int kBgDepth   = 100;
    constexpr int kBatchSize = 1000;

    std::vector<Order> bg_sells(kBgDepth);
    for (int i = 0; i < kBgDepth; ++i) {
        bg_sells[i] = make_order(static_cast<uint64_t>(10000 + i), Side::SELL,
                                 101'000'000 + i * 100'000, 10);
    }

    // Bids at 99 never reach the asks from 101.
    std::vector<Order> buy_orders(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        buy_orders[i] = make_order(static_cast<uint64_t>(20000 + i),
                                   Side::BUY, 99'000'000, 1);
    }

    // Held by unique_ptr because OrderBookV1 holds a std::mutex and cannot be move-assigned.
    auto rebuild_book = [&]() {
        auto b = std::make_unique<Book>("BTCUSD");
        for (int i = 0; i < kBgDepth; ++i) b->add_order(&bg_sells[i]);
        return b;
    };
    auto book = rebuild_book();

    int idx = 0;
    for (auto _ : state) {
        if (idx == kBatchSize) {
            state.PauseTiming();
            book = rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        auto trades = book->add_order(&buy_orders[idx]);
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBook)->Name("BM_AddOrder_NoMatch_V3");
BENCHMARK_TEMPLATE(BM_AddOrder_NoMatch, OrderBookV1)->Name("BM_AddOrder_NoMatch_V1");

// ── Add that fills one order ────────────────────────────────────────────────
// Each iteration's buy fills the only sell of the best level, which then disappears.
// A batch rests kBatchSize sells one per rising level; buy i is priced at level i,
// whose lower levels earlier buys have taken.
template <class Book>
static void BM_AddOrder_FullMatch(benchmark::State& state) {
    constexpr int kBatchSize = 1000;

    std::vector<Order> sell_orders(kBatchSize);
    std::vector<Order> buy_orders(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        const int64_t price = 100'000'000 + i * 100'000;
        sell_orders[i] = make_order(static_cast<uint64_t>(30000 + i), Side::SELL, price, 10);
        buy_orders[i]  = make_order(static_cast<uint64_t>(40000 + i), Side::BUY,  price, 10);
    }

    // The old book is destroyed here, inside PauseTiming.
    std::unique_ptr<Book> book;
    auto rebuild_book = [&]() {
        book = std::make_unique<Book>("BTCUSD");
        for (int i = 0; i < kBatchSize; ++i) {
            reset(sell_orders[i]);
            reset(buy_orders[i]);
            book->add_order(&sell_orders[i]);
        }
    };
    rebuild_book();

    int idx = 0;
    int64_t matched = 0;
    for (auto _ : state) {
        if (idx == kBatchSize) {
            state.PauseTiming();
            rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        auto trades = book->add_order(&buy_orders[idx]);
        matched += static_cast<int64_t>(trades.size());
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.counters["trades_per_iteration"] =
        benchmark::Counter(static_cast<double>(matched), benchmark::Counter::kAvgIterations);
    if (matched != state.iterations()) state.SkipWithError("an iteration did not match exactly one order");
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_AddOrder_FullMatch, OrderBook)->Name("BM_AddOrder_FullMatch_V3");
BENCHMARK_TEMPLATE(BM_AddOrder_FullMatch, OrderBookV1)->Name("BM_AddOrder_FullMatch_V1");

// ── Add that sweeps L levels ────────────────────────────────────────────────
// L = state.range(0). Each iteration's buy of quantity L takes L levels of one sell
// each. A batch rests about 1000 sells one per rising level; buy s is priced at the top
// of the s-th group of L levels.
template <class Book>
static void BM_AddOrder_SweepLevels(benchmark::State& state) {
    const int levels = static_cast<int>(state.range(0));
    const int sweeps = 1000 / levels;  // iterations per batch

    std::vector<Order> sell_orders(static_cast<size_t>(sweeps * levels));
    std::vector<Order> buy_orders(static_cast<size_t>(sweeps));
    uint64_t next_id = 1;
    for (int s = 0; s < sweeps; ++s) {
        for (int i = 0; i < levels; ++i) {
            const int k = s * levels + i;
            sell_orders[k] = make_order(next_id++, Side::SELL, 100'000'000 + k * 100'000, 1);
        }
        buy_orders[s] = make_order(next_id++, Side::BUY,
                                   100'000'000 + (s * levels + levels - 1) * 100'000, levels);
    }

    // The old book is destroyed here, inside PauseTiming.
    std::unique_ptr<Book> book;
    auto rebuild_book = [&]() {
        book = std::make_unique<Book>("BTCUSD");
        for (auto& o : sell_orders) {
            reset(o);
            book->add_order(&o);
        }
        for (auto& o : buy_orders) reset(o);
    };
    rebuild_book();

    int idx = 0;
    int64_t matched = 0;
    for (auto _ : state) {
        if (idx == sweeps) {
            state.PauseTiming();
            rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        auto trades = book->add_order(&buy_orders[idx]);
        matched += static_cast<int64_t>(trades.size());
        benchmark::DoNotOptimize(trades);
        ++idx;
    }

    state.counters["trades_per_iteration"] =
        benchmark::Counter(static_cast<double>(matched), benchmark::Counter::kAvgIterations);
    if (matched != state.iterations() * levels) state.SkipWithError("an iteration did not sweep every level");
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_AddOrder_SweepLevels, OrderBook)->Name("BM_AddOrder_SweepLevels_V3")->Arg(1)->Arg(5)->Arg(10)->Arg(20);
BENCHMARK_TEMPLATE(BM_AddOrder_SweepLevels, OrderBookV1)->Name("BM_AddOrder_SweepLevels_V1")->Arg(1)->Arg(5)->Arg(10)->Arg(20);

// ── Cancel ──────────────────────────────────────────────────────────────────
// Each iteration cancels a resting buy that is alone at its level. A batch rests
// kBatchSize targets interleaved with kBatchSize background buys that stay, so the book
// keeps at least kBatchSize levels; targets are cancelled in a fixed shuffled order so
// that cancels do not always hit one end of the book.
template <class Book>
static void BM_CancelOrder(benchmark::State& state) {
    constexpr int kBatchSize = 1000;

    std::vector<Order> targets(kBatchSize);
    std::vector<Order> background(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        background[i] = make_order(static_cast<uint64_t>(2 * i + 1), Side::BUY,
                                   99'000'000 - (2 * i) * 1000, 1);
        targets[i]    = make_order(static_cast<uint64_t>(2 * i + 2), Side::BUY,
                                   99'000'000 - (2 * i + 1) * 1000, 1);
    }
    std::vector<uint64_t> cancel_ids(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) cancel_ids[i] = targets[i].order_id;
    std::shuffle(cancel_ids.begin(), cancel_ids.end(), std::mt19937_64(42));

    std::unique_ptr<Book> book;
    auto rebuild_book = [&]() {
        book = std::make_unique<Book>("BTCUSD");
        for (int i = 0; i < kBatchSize; ++i) {
            reset(targets[i]);
            book->add_order(&background[i]);
            book->add_order(&targets[i]);
        }
    };
    rebuild_book();

    int idx = 0;
    int64_t cancelled = 0;
    for (auto _ : state) {
        if (idx == kBatchSize) {
            state.PauseTiming();
            rebuild_book();
            state.ResumeTiming();
            idx = 0;
        }

        cancelled += book->cancel_order(cancel_ids[idx]) ? 1 : 0;
        ++idx;
    }

    state.counters["cancels_per_iteration"] =
        benchmark::Counter(static_cast<double>(cancelled), benchmark::Counter::kAvgIterations);
    if (cancelled != state.iterations()) state.SkipWithError("a cancel did not find its order");
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_CancelOrder, OrderBook)->Name("BM_CancelOrder_V3");
BENCHMARK_TEMPLATE(BM_CancelOrder, OrderBookV1)->Name("BM_CancelOrder_V1");

// ── Mixed workload ──────────────────────────────────────────────────────────
// Replays 1,000,000 FeedSimulator orders (10% cancels) into an empty book. Each
// iteration resets the orders and builds a new book inside PauseTiming, so every
// iteration does the same work; differing trade counts are reported as an error.
template <class Book>
static void BM_MixedWorkload(benchmark::State& state) {
    FeedSimulator sim("BTCUSD", 100'000'000, /*cancel_ratio=*/0.1, /*seed=*/42);
    auto orders = sim.generate_random(1000'000);
    for (auto& o : orders) o.timestamp_ns = 1;  // see make_order

    std::unique_ptr<Book> book;
    int64_t first_trades = -1;
    bool same_every_iteration = true;
    for (auto _ : state) {
        state.PauseTiming();
        for (auto& o : orders) reset(o);
        book = std::make_unique<Book>("BTCUSD");
        state.ResumeTiming();

        int64_t total_trades = 0;
        for (auto& o : orders) {
            if (o.type == OrderType::CANCEL) {
                book->cancel_order(o.order_id);
            } else {
                auto trades = book->add_order(&o);
                total_trades += static_cast<int64_t>(trades.size());
            }
        }

        if (first_trades < 0) first_trades = total_trades;
        same_every_iteration = same_every_iteration && total_trades == first_trades;
    }

    state.counters["trades_per_iteration"] = static_cast<double>(first_trades);
    if (!same_every_iteration) state.SkipWithError("iterations produced different numbers of trades");
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) *
        static_cast<int64_t>(orders.size()));
}
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBook)->Name("BM_MixedWorkload_V3")->Iterations(5);
BENCHMARK_TEMPLATE(BM_MixedWorkload, OrderBookV1)->Name("BM_MixedWorkload_V1")->Iterations(5);
