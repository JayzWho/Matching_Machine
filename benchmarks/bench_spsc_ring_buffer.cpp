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
 *   5. BM_SPSC_Order_ProducerConsumer — 双线程传输 Order 对象
 *
 * 双线程基准（3–5）的成绩取决于队列落在哪块物理内存上、两个线程各在哪个 CPU 上，
 * 所以每个都在 16 块内存 × 2 种摆法上各测一次（见"双线程基准的内存块与摆法"）。
 *
 * 运行：
 *   taskset -c 0,1 ./build/release/bench_spsc_ring_buffer
 *   （进程正好能用两个 CPU 时，双线程基准把生产者、消费者分别钉在上面；否则不钉核）
 */

#include <benchmark/benchmark.h>
#include "spsc_ring_buffer.h"
#include "order.h"
#include "thread_placement.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <queue>
#include <mutex>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

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

// ── 双线程基准的内存块与摆法 ─────────────────────────────────────────────────
//
// 双线程基准的成绩跟着两样东西走：队列落在哪块物理内存上（tail_ 所在的 L3 分片
// 是否与生产者所在的 CPU 同号），以及生产者、消费者各在哪个 CPU 上（摆法）。
// 队列原来是栈上的对象、两个线程由调度器摆放，一次启动只抽到一个组合。
//
// 这里在进程启动时为每种队列一次性申请 kBlocks 块内存，每块都用 MAP_POPULATE
// 立即分配物理页，到进程结束都不释放：用完释放再申请，内核可能把同一批物理页还回来。
// 每个基准实例带"内存块、摆法"两个参数，队列从内存块的开头开始构造。
// 摆法 0 把生产者钉在进程能用的两个 CPU 中编号小的那个、消费者钉在编号大的，
// 摆法 1 反过来。一次启动测遍 kBlocks × 2 个组合。

constexpr int kBlocks = 16;

class MemoryBlocks {
public:
    explicit MemoryBlocks(size_t bytes) {
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        const size_t size = (bytes + page - 1) / page * page;
        for (void*& block : blocks_) {
            block = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
            if (block == MAP_FAILED) {
                std::perror("mmap");
                std::abort();
            }
        }
    }

    void* operator[](int64_t index) const { return blocks_.at(static_cast<size_t>(index)); }

private:
    std::array<void*, kBlocks> blocks_{};
};

// 摆法 0：生产者在编号小的 CPU、消费者在编号大的；摆法 1 反过来。
// 进程能用的 CPU 不是正好两个时不钉核，两种摆法相同
static ThreadCpus placement_cpus(int64_t placement) {
    ThreadCpus cpus = allowed_cpu_pair();
    if (placement == 1) std::swap(cpus.producer, cpus.consumer);
    return cpus;
}

// 写进 label：内存块和两个线程实际所在的 CPU，原始 JSON 里可以逐个实例核对
static std::string placement_label(int64_t block, const ThreadCpus& cpus) {
    std::string label = "block=" + std::to_string(block) + " ";
    if (cpus.producer < 0) return label + "unpinned";
    return label + "producer=cpu" + std::to_string(cpus.producer) +
           " consumer=cpu" + std::to_string(cpus.consumer);
}

// 一个双线程基准在一种项数下的全部实例：每块内存依次测两种摆法。
// 迭代次数在注册时固定：实例数是原来的 32 倍，按默认的最短时间（每个重复 0.5 秒）
// 一次启动要十几分钟；固定次数让每个重复约 10–60 毫秒。
// 每次迭代仍新建一个消费者线程，起步方式与原来相同。
static void sampled(benchmark::internal::Benchmark* b, int64_t items) {
    b->ArgNames({"", "block", "placement"});
    for (int64_t block = 0; block < kBlocks; ++block)
        for (int64_t placement = 0; placement < 2; ++placement)
            b->Args({items, block, placement});
}

// ── 3. 双线程 SPSC：真正的生产者-消费者 ──────────────────────────────────────
// 测量参数：每次迭代传输的元素数、内存块、摆法
using IntRing = SPSCRingBuffer<int64_t, 65536>;  // 64K 槽位
static const MemoryBlocks g_int_rings(sizeof(IntRing));

static void BM_SPSC_ProducerConsumer(benchmark::State& state) {
    const int64_t N = state.range(0);  // 每次迭代传输的元素数
    const ThreadCpus cpus = placement_cpus(state.range(2));
    ScopedPin pin(cpus.producer);  // 生产者是主线程：计时循环外钉核，函数返回时恢复
    IntRing& buf = *::new (g_int_rings[state.range(1)]) IntRing;

    for (auto _ : state) {
        std::atomic<bool> done{false};
        int64_t sum_consumer = 0;

        // 消费者线程。N 按值捕获，校验和在局部变量里累加、结束时只写一次：
        // 生产者每项都读的变量若与消费者每项都写的变量共用一个缓存行，
        // 测到的就是这一行在两个核之间来回搬运（伪共享），而不是队列本身
        PinnedThread consumer(cpus.consumer, [&buf, &done, &sum_consumer, N] {
            int64_t received = 0;
            int64_t val;
            int64_t sum = 0;
            while (received < N) {
                if (buf.try_pop(val)) {
                    sum += val;
                    ++received;
                }
            }
            sum_consumer = sum;
            done.store(true, std::memory_order_release);
        });

        // 生产者（主线程）
        for (int64_t i = 0; i < N; ++i) {
            while (!buf.try_push(i)) { /* 自旋等待：队列满了稍后再试 */ }
        }

        consumer.join();
        benchmark::DoNotOptimize(sum_consumer);
    }
    std::destroy_at(&buf);

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * N);
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * N * sizeof(int64_t));
    state.SetLabel(placement_label(state.range(1), cpus));
}
BENCHMARK(BM_SPSC_ProducerConsumer)
    ->Apply([](benchmark::internal::Benchmark* b) { sampled(b, 10'000); })
    ->Iterations(100)
    ->UseRealTime();  // 多线程 benchmark 用 real time 更准确
BENCHMARK(BM_SPSC_ProducerConsumer)
    ->Apply([](benchmark::internal::Benchmark* b) { sampled(b, 100'000); })
    ->Iterations(10)
    ->UseRealTime();

// ── 4. 双线程 Mutex 版本（对比基准） ─────────────────────────────────────────
// 与 BM_SPSC_ProducerConsumer 逻辑完全相同，仅通信机制换成 mutex+queue。
// 对比两者数据，直观展示 lock-free 的性能优势。

// 两个线程共用的状态放在一个从缓存行开头开始的结构里。散放在栈上时，它们落在
// 缓存行里的什么位置随每次启动的栈地址而变，成绩跟着变（与 bench_baseline_mutex.cpp
// 里的引擎对象相同）
struct alignas(64) MutexQueue {
    std::queue<int64_t> q;
    std::mutex mtx;
    std::atomic<bool> producer_done{false};
};
static const MemoryBlocks g_mutex_queues(sizeof(MutexQueue));

static void BM_Mutex_ProducerConsumer(benchmark::State& state) {
    const int64_t N = state.range(0);
    const ThreadCpus cpus = placement_cpus(state.range(2));
    ScopedPin pin(cpus.producer);
    void* const block = g_mutex_queues[state.range(1)];

    for (auto _ : state) {
        MutexQueue& shared = *::new (block) MutexQueue;
        int64_t sum_consumer = 0;

        // 与 BM_SPSC_ProducerConsumer 相同：N 按值捕获，校验和在局部变量里累加
        PinnedThread consumer(cpus.consumer, [&shared, &sum_consumer, N] {
            int64_t received = 0;
            int64_t sum = 0;
            while (received < N) {
                std::unique_lock<std::mutex> lk(shared.mtx);
                if (!shared.q.empty()) {
                    sum += shared.q.front();
                    shared.q.pop();
                    ++received;
                    lk.unlock();
                }
                // else: 释放锁，让生产者继续 push
            }
            sum_consumer = sum;
        });

        for (int64_t i = 0; i < N; ++i) {
            std::lock_guard<std::mutex> lk(shared.mtx);
            shared.q.push(i);
        }
        shared.producer_done.store(true);

        consumer.join();
        benchmark::DoNotOptimize(sum_consumer);
        std::destroy_at(&shared);
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * N);
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * N * sizeof(int64_t));
    state.SetLabel(placement_label(state.range(1), cpus));
}
BENCHMARK(BM_Mutex_ProducerConsumer)
    ->Apply([](benchmark::internal::Benchmark* b) { sampled(b, 10'000); })
    ->Iterations(4)
    ->UseRealTime();
BENCHMARK(BM_Mutex_ProducerConsumer)
    ->Apply([](benchmark::internal::Benchmark* b) { sampled(b, 100'000); })
    ->Iterations(1)
    ->UseRealTime();

// ── 5. 传输 Order 对象（真实负载大小） ───────────────────────────────────────
// 前面测试用 int64_t（8字节），这里改用 Order（64字节，一个 cache line）
// 模拟真实的撮合引擎负载，测量更接近实际的吞吐量。
using OrderRing = SPSCRingBuffer<Order, 16384>;  // 16K
static const MemoryBlocks g_order_rings(sizeof(OrderRing));

static void BM_SPSC_Order_ProducerConsumer(benchmark::State& state) {
    const int64_t N = state.range(0);
    const ThreadCpus cpus = placement_cpus(state.range(2));
    ScopedPin pin(cpus.producer);
    OrderRing& buf = *::new (g_order_rings[state.range(1)]) OrderRing;

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
        int64_t sum = 0;

        // 与 BM_SPSC_ProducerConsumer 相同：计数和校验和留在消费者的局部变量里
        PinnedThread consumer(cpus.consumer, [&buf, &sum, N] {
            Order o{};
            int64_t received = 0;
            int64_t local_sum = 0;
            while (received < N) {
                if (buf.try_pop(o)) {
                    local_sum += o.price;  // 防止被优化掉
                    ++received;
                }
            }
            sum = local_sum;
        });

        for (int64_t i = 0; i < N; ++i) {
            while (!buf.try_push(orders[i])) {}
        }

        consumer.join();
        benchmark::DoNotOptimize(sum);
    }
    std::destroy_at(&buf);

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * N);
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * N * sizeof(Order));
    state.SetLabel(placement_label(state.range(1), cpus));
}
BENCHMARK(BM_SPSC_Order_ProducerConsumer)
    ->Apply([](benchmark::internal::Benchmark* b) { sampled(b, 10'000); })
    ->Iterations(20)
    ->UseRealTime();
