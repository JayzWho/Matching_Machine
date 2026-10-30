/**
 * bench_baseline_mutex.cpp
 *
 * Mutex 基线版本 Benchmark —— 对比用（不考虑优化，实现尽量简单）
 *
 * 本文件包含：
 *   1. MutexMatchingEngine — 极简双线程实现：
 *                    Producer: 生成订单，写 TSC 时间戳，加锁 push + notify
 *                    Consumer: wait + pop，调用 OrderBookV1::add_order，记录延迟
 *                    共享队列：std::queue<Order*> + std::mutex + std::condition_variable
 *   2. BM_MutexPipeline_Throughput    — 10K / 50K / 100K 吞吐测试
 *   3. BM_MutexPipeline_LatencyReport — 100K 笔订单的排队时间、服务时间 P50/P99 报告
 *
 * 撮合用 order_book_v1.h 里的 OrderBookV1：v1 deque 版本（std::mutex + std::deque + std::unordered_map），
 * 逻辑与 docs/snapshots/order_book_v1_deque.{h,cpp}（见 tag archive/pre-cleanup-2026-10）完全一致，
 * 放在 me_v1 命名空间里，避免与主项目 order_book.h 的命名冲突。
 *
 * 运行方式（Release 模式，绑核）：
 *   cmake --build build/release -j$(nproc)
 *   taskset -c 0,1 ./build/release/bench_baseline_mutex
 *
 * 对比目标（SPSC 优化版，来自 learning_notes/11_pipeline.md，见 tag archive/pre-cleanup-2026-10）：
 *   吞吐：10K=1.75M/s，50K=3.23M/s，100K=3.15M/s
 *   P50：~1.26ms，P99：~2.49ms（VM 环境）
 *
 * 注意：
 *   - 此文件完全独立，不修改 src/ 或 include/ 中的任何代码
 *   - 线程摆法与 bench_matching_engine 相同：进程正好能用两个 CPU 时，生产者钉在编号小的、
 *     消费者钉在编号大的上（thread_placement.h），否则不钉核；摆法写在 label 里
 *   - 延迟的测量点和单位与 SPSC 版本相同：取出订单时、撮合完成时各读一次 TSC，单位 TSC ticks
 */

#include <benchmark/benchmark.h>

// 复用主项目中的基础类型和工具（只包含纯头文件，无符号冲突）
#include "order.h"           // Order, Trade, Side, OrderType, OrderStatus
#include "feed_simulator.h"  // FeedSimulator::generate_random
#include "latency_recorder.h" // LatencyRecorder::now_serialized(), report()
#include "thread_placement.h" // ThreadCpus, PinnedThread, allowed_cpu_pair()
#include "order_book_v1.h"   // me_v1::OrderBookV1

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

// ─── 1. MutexMatchingEngine：极简双线程实现 ──────────────────────────────────
//
// 设计原则：尽量简单，不考虑任何优化。
//   - 生产者：流式生成订单，每条用 make_shared 堆分配，push 入共享队列，notify 消费者
//   - 消费者：wait 直到有新订单或生产者完成，pop 后调用 OrderBookV1::add_order，
//             shared_ptr 引用计数归零后自动释放，无需手动 delete
//   - 共享队列：std::queue<shared_ptr<Order>> + std::mutex + std::condition_variable
//   - 订单内存：每条订单独立堆分配（make_shared），无 MemoryPool，无预分配
//   - 延迟测量：生产者 push 前写 TSC 时间戳；消费者取出时、撮合完成时各读一次，
//               分出排队时间和服务时间

class MutexMatchingEngine {
public:
    MutexMatchingEngine() = default;

    ~MutexMatchingEngine() {
        if (running_.load()) stop();
    }

    // 禁止拷贝/移动（含有 mutex/cv，语义复杂）
    MutexMatchingEngine(const MutexMatchingEngine&) = delete;
    MutexMatchingEngine& operator=(const MutexMatchingEngine&) = delete;

    /// 启动双线程流水线，处理 order_count 笔订单；cpus、record_latency 与 MatchingEngine::start 相同
    void start(std::string_view symbol,
               int64_t         base_price,
               size_t          order_count,
               double          cancel_ratio   = 0.2,
               me::ThreadCpus  cpus           = {},
               bool            record_latency = false) {
        if (running_.load()) throw std::runtime_error("already running");

        // 重置状态
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            while (!shared_queue_.empty()) shared_queue_.pop();
        }
        producer_done_.store(false);
        queue_latency_.reset();
        service_latency_.reset();
        running_.store(true);

        try {
            // 启动消费者（先起，避免生产者 push 时消费者未 wait）
            consumer_thread_ = me::PinnedThread(cpus.consumer, [this, sym = std::string(symbol), record_latency] {
                consumer_loop(sym, record_latency);
            });

            // 启动生产者（传入 symbol/base_price/cancel_ratio，线程内流式生成）
            producer_thread_ = me::PinnedThread(cpus.producer, [this, sym = std::string(symbol),
                                                                base_price, order_count, cancel_ratio,
                                                                record_latency] {
                producer_loop(sym, base_price, order_count, cancel_ratio, record_latency);
            });
        } catch (...) {
            // 线程没起来：与 MatchingEngine 相同，让已起来的消费者退出后再抛出
            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                producer_done_.store(true, std::memory_order_release);
            }
            cv_.notify_all();
            if (consumer_thread_.joinable()) consumer_thread_.join();
            running_.store(false);
            throw;
        }
    }

    /// 等待两线程完成
    void stop() {
        if (producer_thread_.joinable()) producer_thread_.join();
        if (consumer_thread_.joinable()) consumer_thread_.join();
        running_.store(false);
    }

    me::LatencyRecorder& queue_latency() { return queue_latency_; }
    me::LatencyRecorder& service_latency() { return service_latency_; }

private:
    void producer_loop(std::string_view symbol, int64_t base_price,
                       size_t order_count, double cancel_ratio, bool record_latency) {
        me::FeedSimulator sim(symbol, base_price, cancel_ratio);

        for (size_t i = 0; i < order_count; ++i) {
            // 每条订单独立堆分配（make_shared），无内存池，无预分配
            auto order = std::make_shared<me::Order>();
            sim.generate_into(order.get());

            // 写入 TSC 时间戳（排队时间的起点，紧贴 push 前）；不记录延迟时与 SPSC 版本相同，
            // 照旧写一个不带栅栏的读数，免得订单簿在消费者线程上补读
            order->timestamp_ns = record_latency ? me::LatencyRecorder::now_serialized()
                                                 : me::LatencyRecorder::now();

            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                shared_queue_.push(std::move(order));
            }
            cv_.notify_one();
        }

        // 通知消费者：生产者已完成。
        // 标志必须在锁内设置：消费者持锁检查条件后才登记等待，
        // 锁外的设置和通知可能落在这两步之间，通知丢失，消费者永久等待。
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            producer_done_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void consumer_loop(std::string_view symbol, bool record_latency) {
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

            auto match = [&] {
                if (order->type == me::OrderType::CANCEL) {
                    book.cancel_order(order->order_id);
                } else {
                    book.add_order(order.get());
                }
            };

            if (record_latency) {
                // 取出的时刻：排队时间的终点，服务时间的起点（与 SPSC 版本相同）
                const uint64_t t_pop = me::LatencyRecorder::now_serialized();
                const uint64_t t_push = order->timestamp_ns;

                match();

                const uint64_t t_done = me::LatencyRecorder::now_serialized();
                queue_latency_.record(t_pop - t_push);
                service_latency_.record(t_done - t_pop);
            } else {
                match();
            }

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

    // 线程句柄（与 std::thread 同样大小，对象布局不变）
    me::PinnedThread producer_thread_;
    me::PinnedThread consumer_thread_;

    // 排队时间、服务时间的记录器（消费者线程写，stop() 后 report）
    me::LatencyRecorder queue_latency_{100'000};
    me::LatencyRecorder service_latency_{100'000};
};


// ─── 2. Benchmark：吞吐量 ────────────────────────────────────────────────────

// 线程摆法，写进 label（与 bench_matching_engine.cpp 相同）
static std::string placement(const me::ThreadCpus& cpus) {
    if (cpus.producer < 0) return "unpinned";
    return "producer=cpu" + std::to_string(cpus.producer) + " consumer=cpu" + std::to_string(cpus.consumer);
}
//
// 结构与 bench_matching_engine.cpp 中 BM_Pipeline_Throughput 完全对称，
// 便于 real_time 直接对比。

static void BM_MutexPipeline_Throughput(benchmark::State& state) {
    const size_t order_count = static_cast<size_t>(state.range(0));
    constexpr double cancel_ratio = 0.2;
    const me::ThreadCpus cpus = me::allowed_cpu_pair();

    // 对象固定从缓存行开头开始。否则栈地址随机化让它每次启动落在不同偏移，
    // 两个线程共用的字段如何分布在缓存行上随之改变，吞吐在两档之间跳动。
    alignas(64) MutexMatchingEngine engine;
    for (auto _ : state) {
        engine.start("BTCUSD", 100'000'000LL, order_count, cancel_ratio, cpus);
        engine.stop();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(order_count)
    );
    state.SetLabel("orders=" + std::to_string(order_count) + " " + placement(cpus));
}

BENCHMARK(BM_MutexPipeline_Throughput)
    ->Arg(10'000)
    ->Arg(50'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3)
    ->UseRealTime();   // 与 BM_Pipeline_Throughput 相同：主线程只在 join 里等待，按墙钟时间算吞吐


// ─── 3. Benchmark：延迟报告（P50 / P99）─────────────────────────────────────
//
// 与 BM_Pipeline_LatencyReport 对称，运行 100K 订单后输出完整延迟百分位。

static void BM_MutexPipeline_LatencyReport(benchmark::State& state) {
    const me::ThreadCpus cpus = me::allowed_cpu_pair();
    alignas(64) MutexMatchingEngine engine;  // 与吞吐测试相同，固定对象位置
    for (auto _ : state) {
        state.PauseTiming();
        engine.start("BTCUSD", 100'000'000LL, 100'000, 0.2, cpus, /*record_latency=*/true);
        engine.stop();
        state.ResumeTiming();

        std::printf("\n--- Mutex Baseline Latency Report (100K orders) ---\n");
        engine.queue_latency().report("queue");
        engine.service_latency().report("service");
        std::printf("\n");
    }
    state.SetLabel(placement(cpus));
}

BENCHMARK(BM_MutexPipeline_LatencyReport)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);
