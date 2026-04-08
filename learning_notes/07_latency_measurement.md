# 第七课：RDTSC 延迟测量——拿到真实的纳秒级数据

> 对应阶段：Week 4-5 / Todo 2
> 关键词：RDTSC、CPU 时钟周期、P50/P99、taskset 绑核、测量误差

---

## 引言

前两课我们完成了 SPSC Ring Buffer 和内存池，把热路径上的主要性能隐患都处理掉了。但"处理掉了"只是定性的说法——到底快了多少？P99 从多少降到多少？没有数据就没有说服力，无论是面试还是简历上的 bullet point 都写不出来。

这一课的任务就是**建立一套可靠的延迟测量体系**。我们会学习为什么 `std::chrono` 不适合测量纳秒级操作、RDTSC 指令的工作原理、以及为什么要看 P99 而不是平均值。这些都是量化面试里会被直接问到的问题。

完成这一课后，第二阶段（Week 4-5）的所有核心组件就齐了：SPSC Ring Buffer、内存池、延迟测量工具。接下来的阶段我们会接入 Google Benchmark 建立正式的性能基线，并用 perf + FlameGraph 找到真正的瓶颈所在。

---

## 一、为什么不用 `std::chrono`？

```cpp
// 看起来很标准的做法
auto t0 = std::chrono::high_resolution_clock::now();
// ... 被测代码 ...
auto t1 = std::chrono::high_resolution_clock::now();
auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
```

问题：`high_resolution_clock` 底层调用 `clock_gettime(CLOCK_REALTIME)`，这是一个**系统调用**。

| 方式 | 精度 | 开销 | 
|------|------|------|
| `std::chrono` | 理论纳秒，实际精度受限 | 50-200 ns（系统调用本身就有延迟！） |
| `RDTSC` | 1 个时钟周期（< 1 ns @ 3GHz） | ~5-20 cycles（纯 CPU 指令） |

**问题的本质**：用一把比被测物体还大的尺子去量，结果没有意义。测量一个 100ns 的操作，如果测量本身消耗 200ns，数据完全失真。

---

## 二、RDTSC 是什么？

**RDTSC = Read Time-Stamp Counter**

x86 CPU 里有一个 64 位的硬件计数器，从 CPU 启动时开始计数，**每个时钟周期加 1**。RDTSC 指令把这个值读到寄存器里，耗时约 5-20 个时钟周期。

```cpp
// 在 C++ 中使用（需要 <x86intrin.h>）
uint64_t t0 = __rdtsc();   // 读时间戳
// ... 被测代码 ...
uint64_t t1 = __rdtsc();
uint64_t elapsed_cycles = t1 - t0;

// 转换为纳秒（需要知道 CPU 频率）
// 3GHz CPU：1 cycle = 1/3 ns ≈ 0.333 ns
double ns = elapsed_cycles / 3.0;  // cpu_ghz = 3.0
```

### 查看自己的 CPU 频率

```bash
grep "cpu MHz" /proc/cpuinfo | head -1
# 输出：cpu MHz : 3200.000  → cpu_ghz = 3.2
```

---

## 三、P50 / P99 统计——为什么不看平均值？

假设测了 1000 次撮合延迟（单位：纳秒）：

```
999 次：100 ns
1 次：  50000 ns（偶发的系统调度、缓存未命中）

平均值：= (999×100 + 50000) / 1000 ≈ 150 ns
P50（中位数）：100 ns
P99：          100 ns（前 990 次都是 100ns）
P999（千分位）：50000 ns
```

**平均值是骗人的**：1 次异常值把平均值抬高了 50%，但实际上 99.9% 的情况都是 100ns。

在量化交易中，关注的指标：

| 指标 | 含义 | 为什么重要 |
|------|------|----------|
| P50（中位数） | 50% 的请求低于这个延迟 | 典型情况 |
| P99 | 99% 的请求低于这个延迟 | 几乎所有情况 |
| P999 | 99.9% 的请求低于这个延迟 | 极端情况（重要！） |
| Max | 最差情况 | 系统的"天花板" |

**计算方法**：对所有样本排序，取对应百分位的值：

```cpp
std::sort(samples.begin(), samples.end());
uint64_t p99 = samples[static_cast<size_t>(0.99 * (samples.size() - 1))];
```

---

## 四、RDTSC 的陷阱：多核 TSC 不同步

### 问题

```
Core 0: TSC = 1000
Core 1: TSC = 800    ← 不同核的 TSC 起点不一样！

如果线程在 Core 0 读 t0，在 Core 1 读 t1：
elapsed = t1 - t0 = 800 - 1000 = -200（负数！）
```

现代 CPU 通常支持 **Invariant TSC**（不变 TSC），所有核的 TSC 单调递增且频率相同，但起点可能不同。如果线程在测量过程中被调度到另一个核，`t1 - t0` 可能出现错误结果。

### 解决方案：绑核（CPU Pinning）

```bash
# taskset -c 0 ./your_program
# 强制 your_program 只运行在 CPU 0 上
taskset -c 0 ./bench_order_book
```

绑核后，测量的 t0 和 t1 一定来自同一个核，TSC 单调递增，差值正确。

这也是为什么量化系统的性能测试脚本里几乎都有 `taskset` 或等价操作。

### 验证是否支持 Invariant TSC

```bash
grep "constant_tsc\|nonstop_tsc" /proc/cpuinfo | head -1
# 有这两个 flag 说明 TSC 可靠
```

---

## 五、测量误差的来源（诚实地认识数据局限性）

| 误差来源 | 影响 | 缓解方法 |
|---------|------|---------|
| RDTSC 自身开销（5-20 cycles） | 系统误差，每次都有 | 预先测量 RDTSC overhead，从结果中减去 |
| 乱序执行（Out-of-Order） | CPU 可能在 rdtsc 前后乱排指令 | 用 `__rdtscp` + `cpuid` 插入串行化 |
| 系统调度（OS scheduler） | 进程被切换导致延迟尖峰 | 绑核 + 设置实时优先级 |
| 缓存预热（Cache Warmup） | 第一次访问冷数据较慢 | 预热循环（warm-up loop） |
| 编译器优化（被测代码被优化掉） | 数据为 0，实际什么都没测 | 用 `DoNotOptimize`（Google Benchmark 提供）|

### `__rdtscp` vs `__rdtsc`

`__rdtscp` 是 `__rdtsc` 的串行化版本，会等待所有前序指令完成后再读计数器，避免乱序执行的影响。代价是多几个时钟周期。在高精度测量中推荐使用。

---

## 六、LatencyRecorder 的使用示例

```cpp
#include "latency_recorder.h"
#include "order_book.h"

void measure_orderbook_latency() {
    me::OrderBook book("TEST");
    me::LatencyRecorder rec(100'000);

    // 预热（让 Cache 和分支预测器进入稳定状态）
    for (int i = 0; i < 1000; ++i) {
        me::Order o{};
        o.order_id = i;
        o.price = 100'000'000;
        o.quantity = 10;
        o.side = me::Side::BUY;
        book.add_order(&o);
        book.cancel_order(o.order_id);
    }

    // 正式测量
    for (int i = 0; i < 100'000; ++i) {
        me::Order o{};
        o.order_id = i + 10000;
        o.price = 100'000'000;
        o.quantity = 1;
        o.side = (i % 2 == 0) ? me::Side::BUY : me::Side::SELL;

        auto t0 = me::LatencyRecorder::now();
        book.add_order(&o);
        auto t1 = me::LatencyRecorder::now();

        rec.record(t1 - t0);
    }

    // 假设 CPU 3.2 GHz
    rec.report(3.2);
}
```

典型输出：
```
=== Latency Report (100000 samples) ===
P50  :    187 cycles /    58 ns
P95  :    312 cycles /    98 ns
P99  :    498 cycles /   156 ns
P999 :   2134 cycles /   667 ns
Max  :  18920 cycles /  5913 ns
```

实际跑起来的命令：

```bash
# 必须用 Release 模式编译（Debug 数据没有参考价值）
cmake --build build/release -j$(nproc)

# 绑核运行，避免线程漂移导致 TSC 跳变
taskset -c 0 ./build/release/tests/test_latency_recorder
# 或者你的 latency 测试可执行文件名
```

如果你想测量 OrderBook 的撮合延迟，把上面示例代码放进一个 `main()` 或 GTest 里，同样用 Release + taskset 跑，输出的 P99 数字就是可以写进简历的那个数字。

---

## 七、为什么要在 Release 模式下测量

```bash
# Debug 模式（包含断言、未优化代码）
cmake .. -DCMAKE_BUILD_TYPE=Debug
# P99 可能是 5000 ns

# Release 模式（-O2，消除调试代码）
cmake .. -DCMAKE_BUILD_TYPE=Release
# P99 可能是 200 ns
```

Debug 模式的数据相差 10-25 倍，完全不具参考价值。  
**所有延迟测量和 benchmark 必须在 Release 模式下进行。**

---

## 八、面试中如何讲这部分

> "延迟测量用 RDTSC 指令直接读 CPU 时钟周期计数，精度是单个时钟周期（约 0.3ns @ 3GHz），避免了 `std::chrono` 系统调用带来的 100ns 以上的测量开销。收集 10 万个样本后排序取 P50/P99/P999，关注百分位而非平均值，因为平均值会被少数尖峰掩盖。测试时用 `taskset -c 0` 绑核，确保 TSC 单调递增，数据有效。"

---

## 九、课后问题

- [ ] 如果不绑核，RDTSC 测出负延迟，应该怎么检测和处理（直接忽略负值样本？还是另有方法）？
    - 实际上，由于使用uint64_t来存放周期数，所以负值会表现为极大值。这种极大值一般是由于OS跨核调度，导致rstsc记录的值是不同核的。
    - 处理方法有几种：（1）设置某个正常不可能碰到的极大值阈值，如果超过这个范围则记为无效，并把无效数+1，最后统一统计。
    - 使用rdtscp计时与保存核id。用core0，core1两个变量接收now采集的核id，前后对比如果不一致，就丢弃。这种做法最精准。


- [ ] P99 和 P999 在实际交易系统中分别对应什么业务含义？
    - 在交易系统中，P99 代表系统在绝大多数情况下的性能，而 P999 则代表极端尾部风险，是决定系统在高压和异常情况下是否还能稳定运行的关键指标，通常比平均延迟更重要。

- [ ] Google Benchmark 的 `state.PauseTiming()` / `state.ResumeTiming()` 解决了什么问题？和 LatencyRecorder 相比有什么优缺点？
    - 解决了测试时不希望将一些杂项的运行时间计入测试时间，比如用这两行代码包裹杂项代码，使其不计入核心代码测试时间统计。
    - 最大的差别是，Google Benchmark只提供均值，但Pxxx尾延迟才是我们最关心的核心指标。
    - Google Benchmark的优点是框架自动预热，提供DoNotOptimize()，但需要链接库。


- [ ] RDTSC 的结果是时钟周期数，不是纳秒。在 CPU 频率会动态变化（Intel Turbo Boost）的机器上，这会带来什么测量误差？如何处理？
    - rdtsc返回的是tsc的ticks，而非CPU周期数。现代CPU有constant_tsc flag的情况下，tsc的计数频率并不会CPU频率波动。真正的误差来源是，ns=cycles/cpu_ghz中，除数采用CPU频率而非tsc计数频率。
    - Turbo Boost超频会使我们使用的固定频率偏低，导致计算的时间偏高。
    - CPU降频则会导致使用固定频率偏高，导致计算时间偏低。
    - 处理方式：读取tsc频率，并用它而非CPU频率计算。也可以用clock_gettime在程序中进行校准，对齐时间和一段tsc ticks。calibrate_tsc_ghz=cycles/ns.

**下一课预告**：Google Benchmark——用框架级 benchmark 建立可重复的性能基线，解决"预热几次才够、迭代多少次才稳定、怎么防止编译器优化"等手动测量解决不了的问题。
