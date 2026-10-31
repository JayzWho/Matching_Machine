/**
 * Queue microbenchmarks: the SPSC ring on one thread, and producer-consumer transfers
 * through the SPSC ring and through a std::queue guarded by a std::mutex.
 *
 * Run through scripts/record/bench_record.py, which confines the process to two CPUs;
 * the two-threaded benchmarks then pin the producer and the consumer to them.
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

// ── One thread ──────────────────────────────────────────────────────────────
static void BM_SPSC_SingleThread_Push(benchmark::State& state) {
    SPSCRingBuffer<int64_t, 4096> buf;

    int64_t val = 0;
    for (auto _ : state) {
        if (!buf.try_push(val)) {
            // Full: pop half, untimed.
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

// ── Memory blocks and placements of the two-threaded benchmarks ─────────────
// A two-threaded result depends on the physical memory the queue lands in and on which
// CPU each thread runs on, and one launch would draw only one combination. So each kind
// of queue gets kBlocks blocks, mapped once at start-up with MAP_POPULATE and never
// freed (freeing and mapping again could return the same pages), and every benchmark
// runs as one instance per block and placement. Placement 0 pins the producer to the
// lower-numbered of the process's two CPUs and the consumer to the higher; placement 1
// swaps them.

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

// Without exactly two CPUs nothing is pinned and both placements are the same.
static ThreadCpus placement_cpus(int64_t placement) {
    ThreadCpus cpus = allowed_cpu_pair();
    if (placement == 1) std::swap(cpus.producer, cpus.consumer);
    return cpus;
}

// The label records the block and the CPUs, so the raw JSON shows each instance's placement.
static std::string placement_label(int64_t block, const ThreadCpus& cpus) {
    std::string label = "block=" + std::to_string(block) + " ";
    if (cpus.producer < 0) return label + "unpinned";
    return label + "producer=cpu" + std::to_string(cpus.producer) +
           " consumer=cpu" + std::to_string(cpus.consumer);
}

// Every block in both placements. Callers fix the iteration count so that a repetition
// takes about 10 to 60 ms; the default minimum time would make a launch take minutes.
static void sampled(benchmark::internal::Benchmark* b, int64_t items) {
    b->ArgNames({"", "block", "placement"});
    for (int64_t block = 0; block < kBlocks; ++block)
        for (int64_t placement = 0; placement < 2; ++placement)
            b->Args({items, block, placement});
}

// ── SPSC ring, two threads ──────────────────────────────────────────────────
// Arguments: items per iteration, memory block, placement.
using IntRing = SPSCRingBuffer<int64_t, 65536>;
static const MemoryBlocks g_int_rings(sizeof(IntRing));

static void BM_SPSC_ProducerConsumer(benchmark::State& state) {
    const int64_t N = state.range(0);
    const ThreadCpus cpus = placement_cpus(state.range(2));
    ScopedPin pin(cpus.producer);  // the main thread is the producer
    IntRing& buf = *::new (g_int_rings[state.range(1)]) IntRing;

    for (auto _ : state) {
        std::atomic<bool> done{false};
        int64_t sum_consumer = 0;

        // N is captured by value and the checksum kept in a local: a variable the producer
        // reads for every item must not share a cache line with one the consumer writes for
        // every item, or the benchmark measures that line moving between cores.
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

        for (int64_t i = 0; i < N; ++i) {
            while (!buf.try_push(i)) {}
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
    ->UseRealTime();
BENCHMARK(BM_SPSC_ProducerConsumer)
    ->Apply([](benchmark::internal::Benchmark* b) { sampled(b, 100'000); })
    ->Iterations(10)
    ->UseRealTime();

// ── Mutex queue, two threads ────────────────────────────────────────────────
// BM_SPSC_ProducerConsumer with a std::queue guarded by a std::mutex.

// The shared state starts on a cache line: scattered on the stack, its position within
// a line moved with each launch's stack address and the result moved with it.
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

        // N by value and a local checksum, as in BM_SPSC_ProducerConsumer.
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

// ── SPSC ring of 64-byte Orders, two threads ────────────────────────────────
using OrderRing = SPSCRingBuffer<Order, 16384>;
static const MemoryBlocks g_order_rings(sizeof(OrderRing));

static void BM_SPSC_Order_ProducerConsumer(benchmark::State& state) {
    const int64_t N = state.range(0);
    const ThreadCpus cpus = placement_cpus(state.range(2));
    ScopedPin pin(cpus.producer);
    OrderRing& buf = *::new (g_order_rings[state.range(1)]) OrderRing;

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

        // Count and checksum in the consumer's locals, as in BM_SPSC_ProducerConsumer.
        PinnedThread consumer(cpus.consumer, [&buf, &sum, N] {
            Order o{};
            int64_t received = 0;
            int64_t local_sum = 0;
            while (received < N) {
                if (buf.try_pop(o)) {
                    local_sum += o.price;
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
