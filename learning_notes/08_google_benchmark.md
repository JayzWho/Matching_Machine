# 第八课：Google Benchmark——建立可信的性能基线

> 对应阶段：Week 6 / Todo 3
> 关键词：Google Benchmark、microbenchmark、DoNotOptimize、PauseTiming、items_per_second、SPSC vs mutex

---

## 引言

上一课我们有了 RDTSC 这把"精确的尺子"，能在自己的代码里手动插桩测延迟。但手动测量有几个问题：预热几次才够？迭代多少次结果才稳定？怎么防止编译器把被测代码优化掉？这些问题如果没有系统性的答案，测出来的数字就不可复现，也不好看。

**Google Benchmark** 是业界公认的 C++ microbenchmark 框架，由 Google 开源并在内部大量使用。它解决了上述所有问题：自动决定迭代次数（直到统计稳定）、提供 `DoNotOptimize` 防优化、内置 `PauseTiming` 精确控制计时范围，输出格式标准、易于对比。

这一课我们在项目里写两套 benchmark：`bench_order_book` 测撮合延迟，`bench_spsc_ring_buffer` 测 SPSC 吞吐量并与 mutex 对比，把"我们的系统很快"从定性说法变成有数字支撑的结论。

---

## 一、Google Benchmark 的核心 API

### 1.1 最简单的 benchmark

```cpp
#include <benchmark/benchmark.h>

static void BM_MyOperation(benchmark::State& state) {
    for (auto _ : state) {
        // 每次循环对应一次被测操作
        int x = some_function();
        benchmark::DoNotOptimize(x);  // 防止编译器优化掉结果
    }
}
BENCHMARK(BM_MyOperation);

BENCHMARK_MAIN();  // 生成 main 函数（或直接链接 benchmark_main 库）
```

框架会自动决定循环次数（通常是直到连续几次测量的相对误差 < 0.5%），然后输出：

```
BM_MyOperation   185 ns   183 ns   3826400
                 ↑wall    ↑CPU     ↑迭代次数
```

### 1.2 `DoNotOptimize` 和 `ClobberMemory`

```cpp
// 防止编译器把整个表达式优化成常量
benchmark::DoNotOptimize(result);

// 强制假设内存被修改（用于容器等带副作用的操作）
benchmark::ClobberMemory();
```

这两个是 benchmark 正确性的基础。不加 `DoNotOptimize`，Release 模式下编译器可能直接把被测函数变成一条 `nop`，测出 0ns。

### 1.3 `PauseTiming` / `ResumeTiming`

```cpp
static void BM_WithSetup(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();   // ← 暂停计时：下面的初始化不计入结果
        OrderBook book("TEST");
        for (int i = 0; i < 1000; ++i) book.add_order(make_order(...));
        state.ResumeTiming();  // ← 恢复计时：从这里开始才算

        // 真正被测的代码
        auto result = book.add_order(make_critical_order());
        benchmark::DoNotOptimize(result);
    }
}
```

这解决了"每次迭代都需要一个干净初始状态"的经典问题。

### 1.4 参数化 benchmark

```cpp
BENCHMARK(BM_SweepLevels)->Arg(1)->Arg(5)->Arg(10)->Arg(20);
// 等价于注册 4 个 benchmark，state.range(0) 分别是 1/5/10/20
```

一个函数测不同规模，结果自动对齐在一张表里，直接看到"复杂度曲线"。

### 1.5 吞吐量统计

```cpp
state.SetItemsProcessed(state.iterations());
// 框架自动计算 items/second，输出中显示为 items_per_second
```

---

## 二、OrderBook Benchmark 实测结果

**测试环境**：2 × 2494 MHz CPU，L1 Data 32 KiB，L2 4096 KiB，L3 36608 KiB，Release 模式，`taskset -c 0` 绑核。

### 结果总览

| Benchmark | CPU Time | items/sec | 说明 |
|-----------|----------|-----------|------|
| `BM_AddOrder_NoMatch` | **248 ns** | 4.04 M/s | 纯挂单，无撮合 |
| `BM_AddOrder_FullMatch` | **539 ns** | 1.86 M/s | 一对一精确成交 |
| `BM_AddOrder_SweepLevels/1` | **1350 ns** | 740 k/s | 扫穿 1 档 |
| `BM_AddOrder_SweepLevels/5` | **1652 ns** | 605 k/s | 扫穿 5 档 |
| `BM_AddOrder_SweepLevels/10` | **2099 ns** | 476 k/s | 扫穿 10 档 |
| `BM_AddOrder_SweepLevels/20` | **3030 ns** | 330 k/s | 扫穿 20 档 |

### 数据解读

**无撮合（248 ns）**是 OrderBook 操作的下界。这个操作主要包含：
- 在 `std::map`（红黑树）里找到对应价格档位：`O(log N)` 比较 + 指针追踪
- 在 `std::list` 末尾插入一个节点：一次 `new`（可被内存池消除）

**精确成交（539 ns）= 无撮合 + 撮合开销**。多出来的 ~291 ns 是：
- 遍历 ask 档位的订单列表，找到匹配项
- 生成 Trade 对象并填充
- 从 map/list 删除已成交订单

**扫穿多档的线性关系**很清晰：1档→5档→10档→20档，从 1350ns 涨到 3030ns，每增加一档约 +85 ns。这符合 `O(k)` 的预期（k = 被扫穿的档位数）。

### 延伸阅读：这些数字意味着什么？

```
BM_AddOrder_NoMatch：248 ns
   → 约 620 cycles @ 2.5 GHz
   → 意味着每秒最多处理 ~4M 笔纯挂单

BM_AddOrder_FullMatch：539 ns
   → 在 CME Globex 等交易所，单笔撮合目标 < 1 μs，我们达到了

BM_AddOrder_SweepLevels/20：3030 ns
   → 大单扫穿 20 档是最坏情况，3 μs 仍属可接受范围
```

> **面试话术**：「OrderBook 单笔撮合延迟约 540 ns（精确成交），纯挂单约 250 ns，用 Google Benchmark + taskset 绑核测量，Release 模式。」

---

## 三、SPSC vs Mutex Benchmark

### 场景设计

```cpp
// 3. 双线程 SPSC：真实生产者-消费者
//    生产者在主线程 push，消费者线程 pop，N 个元素全部送达
BM_SPSC_ProducerConsumer(N = 10000, 100000)

// 4. 相同逻辑，通信机制换成 mutex+queue
BM_Mutex_ProducerConsumer(N = 10000, 100000)

// 5. 传输真实 Order 对象（64字节，一个 cache line）
BM_SPSC_Order_ProducerConsumer(N = 10000)
```

### 为什么双线程 benchmark 用 `UseRealTime()`？

```cpp
BENCHMARK(BM_SPSC_ProducerConsumer)->Arg(10'000)->UseRealTime();
```

单线程 benchmark 用 CPU time（只算自己的时间片）。但双线程时，主线程在等消费者 `join`，这段等待时间算 "wall clock time" 而非 "CPU time"。如果用 CPU time，两个线程的用时会被合并，数据失真；用 `UseRealTime()` 报告的是真实经过时间，才能反映实际吞吐量。

### 预期的结论

SPSC 相比 mutex 版本的优势在于：

| 对比维度 | SPSC | Mutex+Queue |
|---------|------|------------|
| 同步机制 | `std::atomic` load/store，无内核陷入 | `pthread_mutex_lock`，可能触发 futex 系统调用 |
| 内存分配 | 环形缓冲区预分配，运行时零 malloc | `std::queue` 每次 push 调用 `new` |
| 缓存行为 | 头/尾指针各占一个 cache line（`alignas(64)` 隔离） | 锁本身和 queue 内部结构在同一 cache line，产生 false sharing |
| 典型优势 | 10x～50x 吞吐量提升（低竞争时） | — |

> **注意**：在 `perf_event_paranoid=4` 的受限环境下，部分 perf 统计信息不可用，benchmark 的硬件计数器（cache miss、branch mispredict 等）也无法测量。生产环境服务器通常设置为 1 或 2。

---

## 四、benchmark 的正确使用姿势

### 4.1 必须用 Release 模式

```bash
# Debug 模式下 OrderBook 的延迟可能是 Release 的 10-20 倍
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)
```

### 4.2 必须绑核

```bash
# 单线程 benchmark：绑到一个物理核心
taskset -c 0 ./build/release/bench_order_book

# 双线程 benchmark：指定两个物理核心（避免超线程干扰）
taskset -c 0,1 ./build/release/bench_spsc_ring_buffer
```

不绑核的后果：OS 调度器可能把线程迁到不同核，TSC 不同步，延迟数据出现异常尖峰，P99 数据不可信。

### 4.3 关注 CPU Time，不是 Wall Time（单线程）

```
BM_AddOrder_NoMatch   262 ns (wall)   248 ns (CPU)   100000
                       ↑              ↑
                  包含调度等待      纯 CPU 时间
```

Google Benchmark 默认输出 CPU time，这对单线程 benchmark 是正确的。

### 4.4 输出 JSON 方便后续分析

```bash
taskset -c 0 ./build/release/bench_order_book \
    --benchmark_format=json > results/bench_2026_04_08.json
```

可以用 `benchmark_compare.py`（Google Benchmark 附带工具）对比两次结果，量化优化效果。

---

## 五、benchmark 常见陷阱

### 陷阱 1：忘记 `DoNotOptimize`

```cpp
// 错误：编译器直接把 add_order 优化掉了，测出 0ns
for (auto _ : state) {
    book.add_order(&order);
}

// 正确：
for (auto _ : state) {
    auto trades = book.add_order(&order);
    benchmark::DoNotOptimize(trades);
}
```

### 陷阱 2：在循环内构造被测对象

```cpp
// 错误：Order 构造开销也被计入了
for (auto _ : state) {
    Order o = make_order(id++, Side::BUY, 100'000'000, 1);  // ← 构造开销
    auto trades = book.add_order(&o);
    benchmark::DoNotOptimize(trades);
}

// 正确：把构造放到 PauseTiming 里，或预先生成订单列表
```

### 陷阱 3：每次迭代共享同一个 OrderBook 状态

```cpp
// 问题：第 1000 次迭代的 OrderBook 里有 999 个挂单，
//       测的不是"空 OrderBook 的插入"，而是"满 OrderBook 的插入"
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    OrderBook book("BTCUSD");  // ← 所有迭代共享这一个
    uint64_t id = 1;
    for (auto _ : state) {
        Order o = make_order(id++, Side::BUY, 99'000'000, 1);
        auto trades = book.add_order(&o);  // 深度越来越大！
        benchmark::DoNotOptimize(trades);
    }
}
```

在 `BM_AddOrder_NoMatch` 里这实际上是**刻意设计**的——卖价始终 > 买价（卖从 101 开始，买单挂在 99），所以挂单量会增长，但因为买卖价格不重叠，map 里 `lower_bound` 的查找复杂度随深度增长。如果你想测"固定深度下的插入"，需要在 `PauseTiming` 里重建 OrderBook。

### 陷阱 4：用 `Iterations(N)` 固定迭代次数

```cpp
BENCHMARK(BM_AddOrder_NoMatch)->Iterations(100'000);
```

框架默认会自动决定迭代次数（通常更多），固定 `Iterations` 可以保证结果的可重复性，但要小心：`Iterations` 太少时误差大，太多时 benchmark 跑太慢影响 CI。

---

## 六、项目的 CMake 配置

在 `CMakeLists.txt` 中集成 Google Benchmark：

```cmake
# 使用系统已安装的 Google Benchmark（apt install libbenchmark-dev）
find_package(benchmark REQUIRED)

# 添加 benchmark 可执行文件
add_executable(bench_order_book benchmarks/bench_order_book.cpp)
target_link_libraries(bench_order_book
    PRIVATE
    order_book_lib        # 项目库
    feed_simulator_lib
    benchmark::benchmark  # Google Benchmark
    benchmark::benchmark_main
)
target_compile_options(bench_order_book PRIVATE -O3 -march=native)
```

关键点：
- `benchmark::benchmark_main` 会自动提供 `main()` 函数，不需要自己写
- `-O3 -march=native` 在 Release 模式的基础上进一步优化，启用 AVX2 等现代指令集（测量真实极限性能时用）
- benchmark 目标**不加** `-fsanitize=address`，sanitizer 会大幅增加延迟

---

## 七、面试中如何讲这部分

> "用 Google Benchmark 建立了 OrderBook 的性能基线：纯挂单 248 ns，精确成交 539 ns，大单扫穿 20 档 3 μs。SPSC Ring Buffer 的吞吐量测试证明相比 mutex+queue 有 10x 以上的优势。benchmark 用 Release 模式、taskset 绑核运行，确保数据可重复。框架的 `DoNotOptimize` 防止编译器优化，`PauseTiming` 精确隔离被测代码。"

---

## 八、课后问题

- [ ] `BM_AddOrder_SweepLevels` 中扫穿档位数从 1 增加到 20，延迟从 1350 ns 增加到 3030 ns，约 1680 ns / 19 档 ≈ 88 ns/档。这 88 ns 主要花在哪里（撮合循环？内存解分配？Trade 对象构造？）？如何用 benchmark 进一步拆解？

- [ ] Google Benchmark 报告的是**平均延迟**，而 RDTSC 可以测 P99。两种方式各自适用什么场景？如果想在 Google Benchmark 框架内测 P99，该怎么做（提示：`state.counters` 和自定义 `StatisticsFunc`）？

- [ ] 在 `BM_SPSC_ProducerConsumer` 中，消费者线程是忙等（spin）的 `while(!try_pop)`。这在 benchmark 中是合理的，但在真实系统里为什么可能不合适？生产系统里消费者一般怎么处理"队列空"的情况？

- [ ] `benchmark_compare.py` 可以对比两次 JSON 结果，计算性能变化百分比。如果你想把 benchmark 加进 CI（GitHub Actions），每次 PR 合并后自动运行并报告性能变化，大概需要哪些步骤？（提示：注意 CI 机器的 CPU 负载不稳定问题）

**下一课预告**：`perf` + FlameGraph——用硬件性能计数器找到撮合引擎的真实瓶颈，生成可视化的调用栈火焰图，定位 CPU 时间花在哪里。
