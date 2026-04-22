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

**测试环境**：2 × 2494 MHz CPU，L1 Data 32 KiB，L2 4 MiB，L3 ~35.7 MiB，Release 模式，重复 5 次取均值。  
**结果文件**：`results/order_book_2026-04-12_19-27-50_baseline.json`（baseline 版本，deque 实现）

### 结果总览

| Benchmark | CPU Time (mean) | items/sec | CV | 说明 |
|-----------|-----------------|-----------|-----|------|
| `BM_AddOrder_NoMatch` | **81.8 ns** | 12.2 M/s | 1.6% | 纯挂单，无撮合（100档背景深度） |
| `BM_AddOrder_FullMatch` | **758.3 ns** | 1.32 M/s | 1.4% | 一对一精确成交 |
| `BM_AddOrder_SweepLevels/1` | **211.9 ns** | 4.74 M/s | **8.1%** | 扫穿 1 档 |
| `BM_AddOrder_SweepLevels/5` | **930.8 ns** | 1.07 M/s | 1.9% | 扫穿 5 档 |
| `BM_AddOrder_SweepLevels/10` | **1926.7 ns** | 527 k/s | **13.2%** | 扫穿 10 档 |
| `BM_AddOrder_SweepLevels/20` | **4207.9 ns** | 238 k/s | 1.5% | 扫穿 20 档 |
| `BM_CancelOrder` | **~59.7 μs/迭代** | 16.8 k/s | 2.4% | 撤单（每迭代含重建 1000 单） |
| `BM_MixedWorkload` | **270.9 ms/轮** | 3.69 M订单/s | 3.2% | 混合流量（100万订单/轮，稳态测量） |

> **注**：取两次测试（19:23:23 和 19:27:50）中更稳定的后一次数据（load_avg 更低）。`SweepLevels/1` 和 `/10` 的 CV 偏高（8.1%、13.2%），说明这两档存在较大的测量噪声，mean 值代表性有限，后续分析以 median 为参考（/1 median ≈ 204.6 ns，/10 median ≈ 1933.7 ns）。

### 数据解读

**无撮合（81.8 ns）**是 OrderBook 操作的下界。这个操作主要包含：
- 在 `std::map`（红黑树）里找到对应价格档位：`O(log N)` 比较 + 指针追踪
- 在 `std::deque` 末尾插入一个 `Order*`：均摊 O(1)（chunk 内写指针）

**精确成交（758.3 ns）= 无撮合 + 撮合开销**。多出来的 ~676 ns 是：
- 遍历 ask 档位的 deque，找到 front 匹配项
- 生成 Trade 对象并 `emplace_back`（触发 `vector<Trade>` 堆分配）
- `deque::pop_front()` + 从 `order_index_` 删除成交订单 + `map::erase` 删空档位

**扫穿多档的延迟趋势**：
```
1档(211.9 ns) → 5档(930.8 ns) → 10档(1926.7 ns) → 20档(4207.9 ns)
```
从档位数看，1→10档延迟约 10×（1926/212 ≈ 9.1），1→20档延迟约 20×（4208/212 ≈ 19.8），**整体趋势接近 O(k) 线性**（k = 被扫穿档位数）。但各段斜率略有差异（1→5档 ~180 ns/档，5→10档 ~199 ns/档，10→20档 ~228 ns/档），斜率随档数有温和的递增趋势，说明每档的边际成本略有上升，并非严格意义上的线性常数斜率。

**混合流量（3.69 M订单/s）**：每次迭代处理 100 万条订单（70% 挂单 + 20% 一对一成交 + 10% 撤单），序列足够长，OrderBook 在冷启动后进入稳态工作状态，测出的是真实稳态吞吐量。CV 3.2%，统计置信度可接受。

### 延伸阅读：这些数字意味着什么？

```
BM_AddOrder_NoMatch：81.8 ns
   → 约 204 cycles @ 2.5 GHz
   → 意味着每秒最多处理 ~12.2M 笔纯挂单

BM_AddOrder_FullMatch：758.3 ns
   → 在 CME Globex 等交易所，单笔撮合目标 < 1 μs，我们达到了

BM_AddOrder_SweepLevels/20：4207.9 ns ≈ 4.2 μs
   → 大单扫穿 20 档已超出 < 1 μs 范围，是明显的优化目标

BM_MixedWorkload：3.69 M订单/s
   → 稳态混合流量下约 271 ns/单，接近精确成交的延迟水平
```

> **面试话术**：「OrderBook（deque baseline 版本）单笔精确成交延迟约 758 ns，纯挂单约 82 ns，用 Google Benchmark 重复 5 次取均值，Release 模式。混合流量稳态吞吐量约 3.7 M 订单/s。大单扫穿 20 档约 4.2 μs，延迟随扫穿档数近线性增长（约 210 ns/档），是明显的优化目标。」

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

### 实测结果

**结果文件**：`results/spsc_ring_buffer_2026-04-12_19-23-23_baseline.json`（最新基线，load_avg ≈ 1.02）  
**参考文件**：`results/spsc_ring_buffer_2026-04-12_16-59-39_baseline.json`（早期对比，load_avg ≈ 1.05）

| Benchmark | Real Time (mean) | items/sec | CV | 说明 |
|-----------|-----------------|-----------|-----|------|
| `BM_SPSC_SingleThread_Push` | **2.44 ns** | 410 M/s | 0.16% | 单线程写，~3.28 GB/s |
| `BM_SPSC_SingleThread_PushPop` | **2.73 ns** | 368 M/s | 0.45% | push+pop 往返延迟 |
| `BM_SPSC_ProducerConsumer/10k` | **115.7 μs** | 86.8 M/s | 6.7% | 双线程 int64 |
| `BM_SPSC_ProducerConsumer/100k` | **842.3 μs** | 119.1 M/s | 6.5% | 双线程 int64 |
| `BM_Mutex_ProducerConsumer/10k` | **1984.6 μs** | 5.09 M/s | 11.0% | mutex 对比 |
| `BM_Mutex_ProducerConsumer/100k` | **17215.4 μs** | 6.00 M/s | 19.2% | mutex 对比 |
| `BM_SPSC_Order_ProducerConsumer/10k` | **189.8 μs** | 52.7 M/s | 2.1% | 64字节 Order，~3.37 GB/s |

> **两次测试对比说明**（16:59:39 vs 19:23:23）：单线程 Push/PushPop 两次结果吻合极好（<0.5% 差异），说明单线程基线稳定；双线程 SPSC 两次相差 ~5%（load_avg 相近），属正常波动；Mutex 两次差异较大（10k: 2265 μs → 1985 μs，100k: 21248 μs → 17215 μs），Mutex 对 OS 调度噪声更敏感，CV 达 11~19%。取 19:23:23 为最新基线（负载略低，单线程 CV 更优）。

### SPSC vs Mutex 实测对比

| 规模 | SPSC real_time | Mutex real_time | **加速比** |
|------|---------------|-----------------|-----------|
| 10k 元素 | 115.7 μs | 1984.6 μs | **~17.1x** |
| 100k 元素 | 842.3 μs | 17215.4 μs | **~20.4x** |

实测加速比 **17x～20x**，与预期的"10x～50x"吻合。Mutex 版本 CV 在 11~19%，说明 mutex 争抢下延迟高度不稳定；SPSC 的 CV 控制在 2~7%，稳定性明显更好。

### 关键发现：单线程性能

`BM_SPSC_SingleThread_Push` 仅需 **2.44 ns**（410 M ops/s），说明在无竞争情况下，SPSC 的 push 操作接近于两次 `std::atomic` load/store（各约 1 ns），基本达到硬件极限。传输真实 Order 对象（64字节）时达到 **3.37 GB/s**，接近 L1 Cache 带宽上限。

### SPSC 相比 mutex 的理论优势

| 对比维度 | SPSC | Mutex+Queue |
|---------|------|------------|
| 同步机制 | `std::atomic` load/store，无内核陷入 | `pthread_mutex_lock`，可能触发 futex 系统调用 |
| 内存分配 | 环形缓冲区预分配，运行时零 malloc | `std::queue` 每次 push 调用 `new` |
| 缓存行为 | 头/尾指针各占一个 cache line（`alignas(64)` 隔离） | 锁本身和 queue 内部结构在同一 cache line，产生 false sharing |
| 实测优势 | **~23x 吞吐量提升**（低竞争时） | — |

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

// 正确：预先生成订单列表，循环内直接取引用
std::vector<Order> orders(kBatchSize);
for (int i = 0; i < kBatchSize; ++i) orders[i] = make_order(...);

for (auto _ : state) {
    auto trades = book.add_order(&orders[idx % kBatchSize]);
    benchmark::DoNotOptimize(trades);
    ++idx;
}
```

**本项目的修复**：`BM_AddOrder_NoMatch` 和 `BM_AddOrder_FullMatch` 均已采用预生成订单列表的方式，消除循环内构造开销。

### 陷阱 3：每次迭代共享同一个 OrderBook 状态

```cpp
// 问题：第 1000 次迭代的 OrderBook 里有 999 个挂单，
//       测的不是"固定深度的插入"，而是"深度随迭代线性增长"
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    OrderBook book("BTCUSD");  // ← 所有迭代共享，深度越来越大
    uint64_t id = 1;
    for (auto _ : state) {
        Order o = make_order(id++, Side::BUY, 99'000'000, 1);
        auto trades = book.add_order(&o);
        benchmark::DoNotOptimize(trades);
    }
}
```

**说明**：这个问题比"完全错误"更微妙。"共享状态模拟现实中有挂单量"的逻辑是成立的，但问题在于：

- 深度从 0 增长到 `iterations` 的过程中，`std::map::lower_bound` 的 `O(log N)` 开销从接近 O(1) 增长到 `O(log iterations)`
- 最终报告的"平均值"是从快到慢的混合值，**既不代表空盘口，也不代表稳态盘口**，测量目标是模糊的

**本项目的修复**：`BM_AddOrder_NoMatch` 每 `kBatchSize` 次迭代在 `PauseTiming` 内重建 `OrderBook`（保留固定 100 档卖单背景深度），使每次 `add_order` 面对的树深度恒定，测出的是**稳态固定深度下的插入延迟**。

### 陷阱 4：用 `Iterations(N)` 固定迭代次数

```cpp
BENCHMARK(BM_AddOrder_NoMatch)->Iterations(100'000);
```

框架默认会自动决定迭代次数（直到连续几次测量的相对误差 < 0.5% 才停）。
固定 `Iterations` 的问题：
- 次数太少时误差大，结果不稳定
- 在"共享 OrderBook 状态"的场景下，固定次数会加剧深度不一致的问题（因为你可以精确算出最终深度，但平均值仍然混合了不同深度的延迟）

**本项目的修复**：`BM_AddOrder_NoMatch` 和 `BM_AddOrder_FullMatch` 均已去掉 `->Iterations(N)`，交由框架自动决定，确保统计稳定性。`BM_CancelOrder` 和 `BM_MixedWorkload` 保留固定次数，因为它们的每次迭代开销较重（前者每迭代重建 1000 单，后者每迭代处理 100 万单），自动迭代会导致运行时间过长。

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

> "用 Google Benchmark 建立了 OrderBook 的性能基线（deque 实现）：纯挂单 82 ns，精确成交 758 ns，大单扫穿 20 档约 4.2 μs（均 Release 模式，重复 5 次取均值）。混合流量稳态吞吐量约 3.7 M 订单/s。SPSC Ring Buffer 的吞吐量测试证明相比 mutex+queue 有约 17x～20x 的优势（实测：SPSC 116 μs vs Mutex 1.98 ms，10k 元素；SPSC 842 μs vs Mutex 17.2 ms，100k 元素）。扫穿延迟随档位数近线性增长（约 210 ns/档），trades vector 无 reserve 是斜率有温和递增趋势的可能原因之一，已在优化版本中修复。benchmark 用 `DoNotOptimize` 防止编译器优化，`PauseTiming` 精确隔离被测代码。"

---

## 八、课后问题

- [x] `BM_AddOrder_SweepLevels` 中扫穿档位数从 1 增加到 20，延迟从 ~212 ns 增加到 ~4208 ns（约 **3996 ns / 19 档 ≈ 210 ns/档**，平均斜率）。整体趋势接近线性，但各段斜率有温和递增（1→5档 ~180 ns/档，5→10档 ~199 ns/档，10→20档 ~228 ns/档）。这 ~210 ns/档 主要花在哪里（撮合循环？内存解分配？Trade 对象构造？）？如何用 benchmark 进一步拆解？

  **开销来源猜想**

  每扫穿一档，代码层面可以识别出几类固定操作，以下是对各项开销的推理（未经 perf 验证，属于代码级猜想）：

  **猜想一（可能是斜率略微递增的主因）：`trades` vector 周期性扩容**

  `match()` 中 `std::vector<Trade> trades` 无 `reserve`，初始 capacity=0，每次 `emplace_back` 按 2× 扩容：capacity 0→1→2→4→8→16→…每次扩容触发 `malloc` + 旧数据 `memcpy`，拷贝量随 n 线性增长，但扩容次数是 log₂(n)，摊到每档的额外开销是 **O(1) 均摊，但每次扩容的瞬时尖峰随 n 增大**。这可以解释为什么斜率随档位数有温和上升——大部分迭代是 O(1) 均摊，但偶发的大扩容拉高了均值。

  **猜想二（每档固定成本的主体）：三次内存回收**

  每扫穿一档固定触发：
  1. `ask_levels_.erase(begin())`：红黑树节点回收（`free` + O(log n) 树旋转）
  2. `order_index_.erase(order_id)`：`unordered_map` 桶节点回收
  3. `ask_dq.pop_front()` 消耗完 chunk 时：`deque` chunk 释放

  `ptmalloc` 执行 `free` 时需访问 chunk header 并链入 bin 链表，涉及分配器内部的随机内存地址，容易引发 cache miss。这三次 `free` 的随机内存访问可能是每档 ~200 ns 基础成本的重要来源之一，但具体比重需要 perf 的 cache miss 计数器验证。

  **SweepLevels/1 和 /10 的 CV 偏高（8.1%、13.2%）**

  从数据上看，`/1`（CV 8.1%）和 `/10`（CV 13.2%）的噪声明显高于相邻档位。推测原因：
  - `/1`：每次迭代耗时极短（~212 ns），批量 kBatch=500 的设计下 PauseTiming 系统调用的相对开销依然可感知，导致迭代间抖动偏大
  - `/10`：10 档产生 10 笔 Trade，恰好处于 vector 扩容的临界点——capacity 从 8 扩到 16 发生在第 9 笔 Trade，而这一扩容触发时间因迭代间的内存布局差异有所不同，导致部分迭代触发扩容（+malloc+memcpy 开销），部分不触发，造成双峰分布，均值代表性下降，CV 显著偏高。这个解释是合理猜想，可以通过观察 `/8` vs `/9` vs `/10` 的具体延迟分布来验证。

  **定性总结**：整体接近 O(k) 线性，每档边际成本的温和递增可能来自 `trades` vector 周期性扩容的瞬时尖峰。以上是代码级推理，具体哪个贡献最大，将在第 09 章用 perf stat 硬件计数器验证。

  **如何进一步验证（思路）**

  1. **加 `trades.reserve(levels)` 消除 vector 扩容**：若各档斜率趋于更均匀（尤其 `/10` CV 下降），说明扩容是斜率递增和 `/10` CV 偏高的主因；若变化不大，则说明 map/deque 的分配器开销占主导。
  2. **用 `perf stat` 直接测硬件计数器**：分别对 `SweepLevels/1` 和 `SweepLevels/20` 统计 `LLC-load-misses`、`L1-dcache-load-misses` 等事件，量化 `free` 路径随机内存访问的实际 cache miss 贡献。
  3. **用内存池（PMR arena）消除 free**：用 `std::pmr::monotonic_buffer_resource` 替换 map/unordered_map 的 allocator，对比前后每档增量，量化 `free` 路径的贡献。




- [ ] Google Benchmark 报告的是**平均延迟**，而 RDTSC 可以测 P99。两种方式各自适用什么场景？如果想在 Google Benchmark 框架内测 P99，该怎么做（提示：`state.counters` 和自定义 `StatisticsFunc`）？
    - Benchmark适合：对比优化前后的.json数据，吞吐量基准，探究测试的CV大小来判断稳定性和测试的可靠性。
    - RDTSC适合：满足现实低延迟交易系统对尾延迟的需要，诊断抖动来源（观察P99是否远大于P50）。
    - 想用Benchmark测P99可以吗？
        - 手动对齐，在state循环里调用rdtsc计时，最后一并计算，然后存入state.counters["pxxx"]。
        - 自定义StatisticsFunc
    



- [ ] 在 `BM_SPSC_ProducerConsumer` 中，消费者线程是忙等（spin）的 `while(!try_pop)`。这在 benchmark 中是合理的，但在真实系统里为什么可能不合适？生产系统里消费者一般怎么处理"队列空"的情况？
    - benchmark中合理是因为，为了测最大吞吐量，冲击SPSC环形缓冲区硬件极限速度。同时对比mutex，mutex消费者也在一直尝试lock和unlock mutex.
    - 现实中不合适：
        - 交易系统的工作量不是一直高涨的，在休市、冷清期间不需要处理大量工作。
        - 真实服务器上，同一核心可能在运行多个线程，比如风控、日志、监控等线程，忙等都给消费者线程占住了，别的线程干不了活，不利于整体工作效率。
        - 对于支持超线程的处理器，可能生产消费者线程被调度到同一物理核的两个逻辑核上，自己打架。
        - 长期高功耗会让CPU高强度负载，若触发高温降频得不偿失。
    - 真实解法：
        - 有限尝试spin+分级退让：自旋有限次数，然后进入不同层次的退让。
        - 分级退让：_mm_pause -> yield -> sleep_for
        - 条件变量+唤醒，但是这个要用mutex。

- [ ] `benchmark_compare.py` 可以对比两次 JSON 结果，计算性能变化百分比。如果你想把 benchmark 加进 CI（GitHub Actions），每次 PR 合并后自动运行并报告性能变化，大概需要哪些步骤？（提示：注意 CI 机器的 CPU 负载不稳定问题）
    - **基本概念**：
        - **PR（Pull Request）**：团队协作中不允许直接往主分支推代码，开发者在独立分支改完代码后，在 GitHub 上提交"合并请求"（Pull Request），经过 code review 后才合入主分支。
        - **CI（Continuous Integration，持续集成）**：每次提 PR 或代码合入 master，自动触发编译、测试、benchmark 等检查流水线，不需要人手动执行。GitHub Actions 是 GitHub 内置的 CI 平台，通过 `.github/workflows/` 下的 `.yml` 文件配置触发条件和执行步骤。
    - **把 benchmark 加进 CI 的流程**：
        1. 编写 `benchmark.yml`，配置在 PR 合并到 master 后自动触发
        2. CI 机器上用 Release 模式编译，运行 benchmark 并输出 JSON（`--benchmark_format=json`）
        3. 从专用分支或 Actions Artifact 下载 master 上次的 baseline JSON
        4. 用 `benchmark_compare.py` 对比两个 JSON，计算各项性能变化百分比
        5. 用 GitHub Actions 脚本把对比表格自动贴回 PR 评论区
        6. 若某项退化超过阈值（如 >10%），CI 以非 0 状态码退出，阻止 PR 合入
    - **最大的坑：CI 机器 CPU 负载不稳定**：GitHub Actions 的免费 runner 是共享虚拟机，宿主机上同时跑着几十个其他人的 job，同一段代码今天 82 ns 明天 95 ns，纯粹是环境噪声，与代码改动无关，导致大量误报。
    - **应对思路**：
        - 放宽阈值（20%~30% 才告警），避免噪声触发误报，代价是小退化被淹没
        - `--benchmark_repetitions=5` 多次重复取均值，缓解单次随机抖动，但成倍增加 CI 时间
        - **Self-hosted Runner（工业级方案）**：在固定物理机上部署专用 runner，隔离负载、固定频率、绑核，彻底解决噪声问题
        - 降级为趋势监控：将结果存入时序数据库画历史折线图，长期趋势上升才报警，比单次 diff 更鲁棒（chromium、LLVM 等大项目的实际做法）
    

**下一课预告**：`perf` + FlameGraph——用硬件性能计数器找到撮合引擎的真实瓶颈，生成可视化的调用栈火焰图，定位 CPU 时间花在哪里。
