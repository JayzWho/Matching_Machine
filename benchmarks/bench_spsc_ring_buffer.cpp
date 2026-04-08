/**
 * bench_spsc_ring_buffer.cpp
 *
 * Google Benchmark 套件：测量 SPSC Ring Buffer 的吞吐量，
 * 并与 std::queue + std::mutex 方案对比。
 *
 * 测量场景：
 *   1. BM_SPSC_SingleThread_Push     — 单线程连续 push（无竞争，测纯操作开销）
 *   2. BM_SPSC_SingleThread_PushPop  — 单线程交替 push/pop（测完整往返延迟）
 *   3. BM_SPSC_ProducerConsumer      — 真正双线程生产者-消费者（测并发吞吐量）
 *   4. BM_Mutex_ProducerConsumer     — mutex 版本双线程（对比基准）
 *
 * 运行：
 *   taskset -c 0,1 ./build/release/bench_spsc_ring_buffer
 *   （双线程 benchmark 建议指定两个物理核心）
 */

#include <benchmark/benchmark.h>
#include "spsc_ring_buffer.h"
#include "order.h"

#include <queue>
#include <mutex>
#include <thread>
#include <atomic>

using namespace me;

// ── 1. 单线程连续 push（队列不满时的纯写入开销） ────────────────────────────
static void BM_SPSC_SingleThread_Push(benchmark::State& state) {
    SPSCRingBuffer<int64_t, 4096> buf;

    int64_t val = 0;
    for (auto _ : state) {
        if (!buf.try_push(val)) {
            // 队列满了：消费一半腾出空间
            state.PauseTiming();
            int64_t tmp;
            for (size_t i = 0; i < 2048; ++i) buf.try_pop(tmp);
            state.ResumeTiming();
        }
        ++val;
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * sizeof(int64_t));
}
BENCHMARK(BM_SPSC_SingleThread_Push);

// ── 2. 单线程交替 push/pop（往返延迟） ───────────────────────────────────────
static void BM_SPSC_SingleThread_PushPop(benchmark::State& state) {
    SPSCRingBuffer<int64_t, 4096> buf;

    int64_t val = 0, out = 0;
    for (auto _ : state) {
        buf.try_push(val++);
        buf.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_SPSC_SingleThread_PushPop);

// ── 3. 双线程 SPSC：真正的生产者-消费者 ──────────────────────────────────────
// 测量参数：传输的元素总数（通过 state.range(0) 控制）
static void BM_SPSC_ProducerConsumer(benchmark::State& state) {
    constexpr size_t kCapacity = 65536;  // 64K 槽位
    SPSCRingBuffer<int64_t, kCapacity> buf;

    const int64_t N = state.range(0);  // 每次迭代传输的元素数

    for (auto _ : state) {
        std::atomic<bool> done{false};
        int64_t sum_consumer = 0;

        // 消费者线程
        std::thread consumer([&] {
            int64_t received = 0;
            int64_t val;
            while (received < N) {
                if (buf.try_pop(val)) {
                    sum_consumer += val;
                    ++received;
                }
            }
            done.store(true, std::memory_order_release);
        });

        // 生产者（主线程）
        for (int64_t i = 0; i < N; ++i) {
            while (!buf.try_push(i)) { /* 自旋等待：队列满了稍后再试 */ }
        }

        consumer.join();
        benchmark::DoNotOptimize(sum_consumer);
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * N);
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * N * sizeof(int64_t));
}
BENCHMARK(BM_SPSC_ProducerConsumer)
    ->Arg(10'000)
    ->Arg(100'000)
    ->UseRealTime();  // 多线程 benchmark 用 real time 更准确

// ── 4. 双线程 Mutex 版本（对比基准） ─────────────────────────────────────────
// 与 BM_SPSC_ProducerConsumer 逻辑完全相同，仅通信机制换成 mutex+queue。
// 对比两者数据，直观展示 lock-free 的性能优势。
static void BM_Mutex_ProducerConsumer(benchmark::State& state) {
    const int64_t N = state.range(0);

    for (auto _ : state) {
        std::queue<int64_t> q;
        std::mutex mtx;
        std::atomic<bool> producer_done{false};
        int64_t sum_consumer = 0;

        std::thread consumer([&] {
            int64_t received = 0;
            while (received < N) {
                std::unique_lock<std::mutex> lk(mtx);
                if (!q.empty()) {
                    sum_consumer += q.front();
                    q.pop();
                    ++received;
                    lk.unlock();
                }
                // else: 释放锁，让生产者继续 push
            }
        });

        for (int64_t i = 0; i < N; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            q.push(i);
        }
        producer_done.store(true);

        consumer.join();
        benchmark::DoNotOptimize(sum_consumer);
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * N);
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * N * sizeof(int64_t));
}
BENCHMARK(BM_Mutex_ProducerConsumer)
    ->Arg(10'000)
    ->Arg(100'000)
    ->UseRealTime();

// ── 5. 传输 Order 对象（真实负载大小） ───────────────────────────────────────
// 前面测试用 int64_t（8字节），这里改用 Order（64字节，一个 cache line）
// 模拟真实的撮合引擎负载，测量更接近实际的吞吐量。
static void BM_SPSC_Order_ProducerConsumer(benchmark::State& state) {
    constexpr size_t kCapacity = 16384;
    SPSCRingBuffer<Order, kCapacity> buf;

    const int64_t N = state.range(0);

    // 预生成订单（不计入 benchmark 时间）
    std::vector<Order> orders(N);
    for (int64_t i = 0; i < N; ++i) {
        orders[i].order_id = static_cast<uint64_t>(i + 1);
        orders[i].price    = 100'000'000 + i * 1000;
        orders[i].quantity = 10;
        orders[i].side     = (i % 2 == 0) ? Side::BUY : Side::SELL;
        orders[i].type     = OrderType::LIMIT;
        std::strncpy(orders[i].symbol, "BTCUSD", 7);
    }

    for (auto _ : state) {
        int64_t received = 0;
        int64_t sum = 0;

        std::thread consumer([&] {
            Order o{};
            while (received < N) {
                if (buf.try_pop(o)) {
                    sum += o.price;  // 防止被优化掉
                    ++received;
                }
            }
        });

        for (int64_t i = 0; i < N; ++i) {
            while (!buf.try_push(orders[i])) {}
        }

        consumer.join();
        benchmark::DoNotOptimize(sum);
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * N);
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * N * sizeof(Order));
}
BENCHMARK(BM_SPSC_Order_ProducerConsumer)
    ->Arg(10'000)
    ->UseRealTime();
