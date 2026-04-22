/**
 * bench_baseline_mutex.cpp
 *
 * Mutex 基线版本 Benchmark —— 对比用（不考虑优化，实现尽量简单）
 *
 * 本文件包含：
 *   1. OrderBookV1  — v1 deque 版本（std::mutex + std::deque + std::unordered_map），
 *                    逻辑与 docs/snapshots/order_book_v1_deque.{h,cpp} 完全一致，
 *                    内联于此文件以避免与主项目 order_book.h 的命名冲突。
 *   2. MutexMatchingEngine — 极简双线程实现：
 *                    Producer: 生成订单，写 RDTSC 时间戳，加锁 push + notify
 *                    Consumer: wait + pop，调用 OrderBookV1::add_order，记录延迟
 *                    共享队列：std::queue<Order*> + std::mutex + std::condition_variable
 *   3. BM_MutexPipeline_Throughput    — 10K / 50K / 100K 吞吐测试
 *   4. BM_MutexPipeline_LatencyReport — 100K 端到端延迟 P50/P99 报告
 *
 * 运行方式（Release 模式，绑核）：
 *   cmake --build build/release -j$(nproc)
 *   taskset -c 0,1 ./build/release/bench_baseline_mutex
 *
 * 对比目标（SPSC 优化版，来自 learning_notes/11_pipeline.md）：
 *   吞吐：10K=1.75M/s，50K=3.23M/s，100K=3.15M/s
 *   P50：~1.26ms，P99：~2.49ms（VM 环境）
 *
 * 注意：
 *   - 此文件完全独立，不修改 src/ 或 include/ 中的任何代码
 *   - 延迟单位与 SPSC 版本完全一致（RDTSC 时钟周期 + 2.494 GHz 换算）
 */

#include <benchmark/benchmark.h>

// 复用主项目中的基础类型和工具（只包含纯头文件，无符号冲突）
#include "order.h"           // Order, Trade, Side, OrderType, OrderStatus
#include "feed_simulator.h"  // FeedSimulator::generate_random
#include "latency_recorder.h" // LatencyRecorder::now(), report()

#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <queue>
#include <memory>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>

// ─── 1. OrderBookV1：v1 deque 版本（完整内联，me_v1 命名空间隔离） ───────────
//
// 逻辑与 docs/snapshots/order_book_v1_deque.{h,cpp} 完全相同，
// 保留所有"未优化"特征：
//   - std::deque<Order*> 作为 PriceLevel 队列（非 cache-friendly）
//   - std::unordered_map 无 reserve（高频插入触发 rehash）
//   - cancel_order 用 deque::erase（O(n) 移动）
//   - match() 返回 std::vector<Trade>（每次堆分配）
//   - add_order / cancel_order / best_bid / best_ask 均持 std::mutex

namespace me_v1 {

class OrderBookV1 {
public:
    explicit OrderBookV1(std::string_view symbol)
        : symbol_(symbol) {}

    ~OrderBookV1() = default;

    OrderBookV1(const OrderBookV1&) = delete;
    OrderBookV1& operator=(const OrderBookV1&) = delete;

    // ── 核心接口 ──────────────────────────────────────────────────────────────

    std::vector<me::Trade> add_order(me::Order* order) {
        if (!order) return {};

        std::lock_guard<std::mutex> lock(mtx_);

        // 兜底时间戳（若生产者已写入 RDTSC，则此处跳过）
        if (order->timestamp_ns == 0) {
            order->timestamp_ns = me::LatencyRecorder::now();
        }

        order_index_[order->order_id] = order;
        auto trades = match(order);

        if (!order->is_filled() && order->type == me::OrderType::LIMIT) {
            if (order->side == me::Side::BUY) {
                bid_levels_[order->price].push_back(order);
            } else {
                ask_levels_[order->price].push_back(order);
            }
        }

        return trades;
    }

    bool cancel_order(uint64_t order_id) {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = order_index_.find(order_id);
        if (it == order_index_.end()) return false;

        me::Order* order = it->second;
        if (order->status == me::OrderStatus::FILLED ||
            order->status == me::OrderStatus::CANCELLED) {
            return false;
        }

        order->status = me::OrderStatus::CANCELLED;

        // 【原始版本】线性扫描 deque，用 erase 直接删除（O(n)）
        auto remove_from_level = [&](auto& levels) {
            auto level_it = levels.find(order->price);
            if (level_it == levels.end()) return;
            auto& dq = level_it->second;
            for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
                if ((*dit)->order_id == order_id) {
                    dq.erase(dit);
                    break;
                }
            }
            if (dq.empty()) levels.erase(level_it);
        };

        if (order->side == me::Side::BUY) {
            remove_from_level(bid_levels_);
        } else {
            remove_from_level(ask_levels_);
        }

        order_index_.erase(it);
        return true;
    }

    [[nodiscard]] int64_t best_bid() const noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return bid_levels_.empty() ? 0 : bid_levels_.begin()->first;
    }

    [[nodiscard]] int64_t best_ask() const noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return ask_levels_.empty() ? 0 : ask_levels_.begin()->first;
    }

    [[nodiscard]] size_t order_count() const noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return order_index_.size();
    }

    /// 清空订单簿（benchmark 迭代间复用）
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        bid_levels_.clear();
        ask_levels_.clear();
        order_index_.clear();
        trade_id_counter_ = 0;
    }

private:
    // ── 核心撮合逻辑（价格-时间优先）────────────────────────────────────────
    // 【原始版本】trades 无预分配 reserve，首次 emplace_back 必然触发堆分配
    std::vector<me::Trade> match(me::Order* incoming) {
        std::vector<me::Trade> trades;  // 无 reserve

        if (incoming->side == me::Side::BUY) {
            while (!incoming->is_filled() && !ask_levels_.empty()) {
                auto& [ask_price, ask_dq] = *ask_levels_.begin();
                if (incoming->price < ask_price) break;

                while (!incoming->is_filled() && !ask_dq.empty()) {
                    me::Order* resting = ask_dq.front();

                    int64_t match_qty = std::min(incoming->remaining_qty(),
                                                 resting->remaining_qty());
                    incoming->filled_qty += match_qty;
                    resting->filled_qty  += match_qty;

                    me::Trade& t = trades.emplace_back();
                    t.trade_id      = ++trade_id_counter_;
                    t.timestamp_ns  = incoming->timestamp_ns;
                    t.buy_order_id  = incoming->order_id;
                    t.sell_order_id = resting->order_id;
                    t.price         = ask_price;
                    t.quantity      = match_qty;
                    std::memcpy(t.symbol, incoming->symbol, 8);

                    if (resting->is_filled()) {
                        resting->status = me::OrderStatus::FILLED;
                        order_index_.erase(resting->order_id);
                        ask_dq.pop_front();
                    } else {
                        resting->status = me::OrderStatus::PARTIAL;
                    }
                }
                if (ask_dq.empty()) ask_levels_.erase(ask_levels_.begin());
            }

            if (incoming->is_filled()) {
                incoming->status = me::OrderStatus::FILLED;
                order_index_.erase(incoming->order_id);
            } else if (incoming->filled_qty > 0) {
                incoming->status = me::OrderStatus::PARTIAL;
            }

        } else { // SELL
            while (!incoming->is_filled() && !bid_levels_.empty()) {
                auto& [bid_price, bid_dq] = *bid_levels_.begin();
                if (incoming->price > bid_price) break;

                while (!incoming->is_filled() && !bid_dq.empty()) {
                    me::Order* resting = bid_dq.front();

                    int64_t match_qty = std::min(incoming->remaining_qty(),
                                                 resting->remaining_qty());
                    incoming->filled_qty += match_qty;
                    resting->filled_qty  += match_qty;

                    me::Trade& t = trades.emplace_back();
                    t.trade_id      = ++trade_id_counter_;
                    t.timestamp_ns  = incoming->timestamp_ns;
                    t.buy_order_id  = resting->order_id;
                    t.sell_order_id = incoming->order_id;
                    t.price         = bid_price;
                    t.quantity      = match_qty;
                    std::memcpy(t.symbol, incoming->symbol, 8);

                    if (resting->is_filled()) {
                        resting->status = me::OrderStatus::FILLED;
                        order_index_.erase(resting->order_id);
                        bid_dq.pop_front();
                    } else {
                        resting->status = me::OrderStatus::PARTIAL;
                    }
                }
                if (bid_dq.empty()) bid_levels_.erase(bid_levels_.begin());
            }

            if (incoming->is_filled()) {
                incoming->status = me::OrderStatus::FILLED;
                order_index_.erase(incoming->order_id);
            } else if (incoming->filled_qty > 0) {
                incoming->status = me::OrderStatus::PARTIAL;
            }
        }

        return trades;
    }

    std::string symbol_;
    mutable std::mutex mtx_;

    // 【原始版本】std::deque<Order*>（非 cache-friendly 分段数组布局）
    std::map<int64_t, std::deque<me::Order*>, std::greater<int64_t>> bid_levels_;
    std::map<int64_t, std::deque<me::Order*>>                        ask_levels_;

    // 【原始版本】无 reserve，高频插入触发 rehash
    std::unordered_map<uint64_t, me::Order*> order_index_;

    uint64_t trade_id_counter_ = 0;
};

} // namespace me_v1


// ─── 2. MutexMatchingEngine：极简双线程实现 ──────────────────────────────────
//
// 设计原则：尽量简单，不考虑任何优化。
//   - 生产者：流式生成订单，每条用 make_shared 堆分配，push 入共享队列，notify 消费者
//   - 消费者：wait 直到有新订单或生产者完成，pop 后调用 OrderBookV1::add_order，
//             shared_ptr 引用计数归零后自动释放，无需手动 delete
//   - 共享队列：std::queue<shared_ptr<Order>> + std::mutex + std::condition_variable
//   - 订单内存：每条订单独立堆分配（make_shared），无 MemoryPool，无预分配
//   - 延迟测量：生产者 push 前写 RDTSC，消费者 pop 后做差

class MutexMatchingEngine {
public:
    MutexMatchingEngine() = default;

    ~MutexMatchingEngine() {
        if (running_.load()) stop();
    }

    // 禁止拷贝/移动（含有 mutex/cv，语义复杂）
    MutexMatchingEngine(const MutexMatchingEngine&) = delete;
    MutexMatchingEngine& operator=(const MutexMatchingEngine&) = delete;

    /// 启动双线程流水线，处理 order_count 笔订单
    void start(std::string_view symbol,
               int64_t         base_price,
               size_t          order_count,
               double          cancel_ratio = 0.2) {
        if (running_.load()) throw std::runtime_error("already running");

        // 重置状态
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            while (!shared_queue_.empty()) shared_queue_.pop();
        }
        producer_done_.store(false);
        latency_recorder_.reset();
        running_.store(true);

        // 启动消费者（先起，避免生产者 push 时消费者未 wait）
        consumer_thread_ = std::thread([this, sym = std::string(symbol)] {
            consumer_loop(sym);
        });

        // 启动生产者（传入 symbol/base_price/cancel_ratio，线程内流式生成）
        producer_thread_ = std::thread([this, sym = std::string(symbol),
                                        base_price, order_count, cancel_ratio] {
            producer_loop(sym, base_price, order_count, cancel_ratio);
        });
    }

    /// 等待两线程完成
    void stop() {
        if (producer_thread_.joinable()) producer_thread_.join();
        if (consumer_thread_.joinable()) consumer_thread_.join();
        running_.store(false);
    }

    const me::LatencyRecorder& latency_recorder() const { return latency_recorder_; }

private:
    void producer_loop(std::string_view symbol, int64_t base_price,
                       size_t order_count, double cancel_ratio) {
        me::FeedSimulator sim(symbol, base_price, cancel_ratio);

        for (size_t i = 0; i < order_count; ++i) {
            // 每条订单独立堆分配（make_shared），无内存池，无预分配
            auto order = std::make_shared<me::Order>();
            sim.generate_into(order.get());

            // 写入 RDTSC 时间戳（端到端延迟起点，紧贴 push 前）
            order->timestamp_ns = me::LatencyRecorder::now();

            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                shared_queue_.push(std::move(order));
            }
            cv_.notify_one();
        }

        // 通知消费者：生产者已完成
        producer_done_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    void consumer_loop(std::string_view symbol) {
        me_v1::OrderBookV1 book(symbol);

        // 持有所有 shared_ptr，确保 Order 生命周期覆盖整个 book 的使用期
        // （OrderBook 内部存裸指针，Order 必须在 book.clear() 之前保持存活）
        std::vector<std::shared_ptr<me::Order>> alive;

        while (true) {
            std::shared_ptr<me::Order> order;
            {
                std::unique_lock<std::mutex> lk(queue_mtx_);
                cv_.wait(lk, [this] {
                    return !shared_queue_.empty() ||
                           producer_done_.load(std::memory_order_acquire);
                });

                if (shared_queue_.empty()) {
                    break;
                }
                order = std::move(shared_queue_.front());
                shared_queue_.pop();
            }

            // 端到端延迟测量：从生产者写入时间戳到消费者处理完成
            const uint64_t t_start = order->timestamp_ns;

            if (order->type == me::OrderType::CANCEL) {
                book.cancel_order(order->order_id);
            } else {
                book.add_order(order.get());
            }

            const uint64_t latency = me::LatencyRecorder::now() - t_start;
            latency_recorder_.record(latency);

            // 保留 shared_ptr 直到 book 使用完毕，防止悬空指针
            alive.push_back(std::move(order));
        }

        // book 析构前清空，此后 alive 中的 shared_ptr 才安全释放
        book.clear();
        // alive 在此作用域结束时批量 delete 所有 Order
    }

    // ── 共享状态（生产者/消费者之间） ────────────────────────────────────────
    std::queue<std::shared_ptr<me::Order>> shared_queue_;
    std::mutex               queue_mtx_;
    std::condition_variable  cv_;
    std::atomic<bool>        producer_done_{false};
    std::atomic<bool>        running_{false};

    // 线程句柄
    std::thread producer_thread_;
    std::thread consumer_thread_;

    // 延迟记录器
    me::LatencyRecorder latency_recorder_{100'000};
};


// ─── 3. Benchmark：吞吐量 ────────────────────────────────────────────────────
//
// 结构与 bench_matching_engine.cpp 中 BM_Pipeline_Throughput 完全对称，
// 便于 real_time 直接对比。

static void BM_MutexPipeline_Throughput(benchmark::State& state) {
    const size_t order_count = static_cast<size_t>(state.range(0));
    constexpr double cancel_ratio = 0.2;

    MutexMatchingEngine engine;
    for (auto _ : state) {
        engine.start("BTCUSD", 100'000'000LL, order_count, cancel_ratio);
        engine.stop();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(order_count)
    );
    state.SetLabel("orders=" + std::to_string(order_count));
}

BENCHMARK(BM_MutexPipeline_Throughput)
    ->Arg(10'000)
    ->Arg(50'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);


// ─── 4. Benchmark：延迟报告（P50 / P99）─────────────────────────────────────
//
// 与 BM_Pipeline_LatencyReport 对称，运行 100K 订单后输出完整延迟百分位。

static void BM_MutexPipeline_LatencyReport(benchmark::State& state) {
    MutexMatchingEngine engine;
    for (auto _ : state) {
        state.PauseTiming();
        engine.start("BTCUSD", 100'000'000LL, 100'000, 0.2);
        engine.stop();
        state.ResumeTiming();

        std::printf("\n--- Mutex Baseline Latency Report (100K orders) ---\n");
        // 使用 const_cast 访问 non-const report（LatencyRecorder::report 会排序 samples_）
        const_cast<me::LatencyRecorder&>(engine.latency_recorder()).report(/*cpu_ghz=*/2.494);
        std::printf("\n");
    }
}

BENCHMARK(BM_MutexPipeline_LatencyReport)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);
