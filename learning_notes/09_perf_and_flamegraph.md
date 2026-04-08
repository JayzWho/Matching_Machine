# 第九课：perf + FlameGraph——用火焰图找到真正的瓶颈

> 对应阶段：Week 6 / Todo 3（性能分析部分）
> 关键词：perf record、perf report、FlameGraph、调用栈采样、CPU 热点、符号解析

---

## 引言

上一课我们用 Google Benchmark 建立了延迟基线，知道了"AddOrder 大约需要多少纳秒"。
但这只是**结果**——我们还不知道时间花在哪里。是 `std::map` 的红黑树查找？
是 `deque` 的 chunk 跳转导致的 cache miss？还是 `mutex` 的锁争用？

**perf** 是 Linux 内核自带的硬件性能计数器工具，能以极低开销在运行中的程序上进行
**统计性采样**（不是插桩！），每隔固定时间记录一次当前调用栈，最终给出"程序在
哪些函数上花了多少时间"的统计分布。

**FlameGraph**（Brendan Gregg 开发）把 perf 采集到的调用栈叠加成一张 SVG 可视化图，
横轴代表时间占比，纵轴代表调用深度，一眼就能看出最宽的"平顶山"就是热点。

---

## 一、perf 基础概念

### 1.1 采样式 profiling vs 插桩式 profiling

| 方式 | 原理 | 开销 | 精度 | 适用场景 |
|------|------|------|------|----------|
| **采样式**（perf） | 硬件计数器溢出时中断，记录当前 PC | < 1%（99Hz 采样） | 统计精度（样本足够时准确） | 生产剖析、宏观热点定位 |
| **插桩式**（gprof、Tracy） | 在每个函数入口/出口插入计时代码 | 5%~30% | 函数级精确 | 开发期详细分析 |

perf 是**采样式**，主要优势是开销极低，可以在接近真实负载的情况下运行。

### 1.2 perf 的工作模式

```
perf stat  ./my_program        # 统计整体硬件事件（cycles, cache-misses, etc.）
perf record ./my_program       # 采集调用栈（生成 perf.data）
perf report                    # 交互式查看 perf.data
perf script                    # 将 perf.data 导出为文本（FlameGraph 的输入）
```

---

## 二、安装与权限配置

### 2.1 安装 perf

```bash
# Ubuntu/Debian
sudo apt install linux-tools-generic linux-tools-$(uname -r)

# 验证
perf version
```

### 2.2 调整 perf_event_paranoid

`perf` 采集调用栈需要内核支持，默认配置可能受限：

```bash
cat /proc/sys/kernel/perf_event_paranoid
# 2 或 3 表示受限（无法采集内核调用栈）

# 临时解除限制（重启后失效）
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# 永久生效（写入 sysctl）
echo 'kernel.perf_event_paranoid=1' | sudo tee -a /etc/sysctl.conf
```

| perf_event_paranoid 值 | 含义 |
|------------------------|------|
| -1 | 完全开放（适合开发机） |
| 0 | 允许采集 CPU PMU 事件 |
| 1 | 允许普通用户采集 kernel 调用栈 |
| 2 | 仅允许 root（很多发行版默认值） |
| 3 | 完全禁用非特权用户 |

### 2.3 编译时保留帧指针

perf 默认用 **frame pointer** 展开调用栈。现代编译器的 `-O2` 会优化掉帧指针
（`-fomit-frame-pointer`），导致调用栈只有一层。

解决方案（二选一）：

```cmake
# CMakeLists.txt 中为 Release 版本保留帧指针
target_compile_options(bench_order_book PRIVATE
    -O2 -fno-omit-frame-pointer  # 保留帧指针，代价极小（< 1%）
)
```

或使用 DWARF 模式（更慢但不需要修改编译选项）：

```bash
perf record --call-graph=dwarf ./my_program
```

本项目的 `CMakeLists.txt` 在 `RelWithDebInfo` 类型中已添加 `-fno-omit-frame-pointer`。

---

## 三、实战：剖析 OrderBook 热路径

### 3.1 运行 perf record

```bash
# 绑核（避免跨核迁移干扰采样）+ perf record
taskset -c 0 perf record \
    -g \                              # 采集调用图
    --call-graph=fp \                 # 用帧指针展开
    -F 99 \                           # 99 Hz 采样（非整数 100 避免与定时器同步）
    -o results/perf_orderbook.data \
    -- ./build/release/bench_order_book --benchmark_min_time=5

# 查看文字报告（Top 10 函数）
perf report -i results/perf_orderbook.data --stdio --no-children | head -30
```

### 3.2 生成 FlameGraph

```bash
# 克隆 FlameGraph 工具
git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph

# 三步生成 SVG
perf script -i results/perf_orderbook.data \
    | ~/FlameGraph/stackcollapse-perf.pl \
    | ~/FlameGraph/flamegraph.pl \
        --title "OrderBook Hot Path" \
        --width 1600 \
        --colors hot \
    > results/flamegraph_orderbook.svg

# 在浏览器中打开（支持点击缩放任意栈帧）
firefox results/flamegraph_orderbook.svg
```

项目提供了一键脚本：

```bash
./scripts/run_perf.sh ./build/release/bench_order_book orderbook
```

脚本会自动检测 perf_event_paranoid 级别、FlameGraph 是否存在，
并在缺少依赖时给出具体的安装指引。

### 3.3 如何阅读火焰图

```
 ──────────────────────────────────────────────────────── 底部 = 入口 ──
 │ main                                                                   │
 │──────────────────────────────────────────────────────────────────────│
 │ benchmark::RunSpecifiedBenchmarks                                      │
 │──────────────────────────────────────────────────────────────────────│
 │ BM_AddOrder_FullMatch      │ BM_AddOrder_NoMatch │ BM_MixedWorkload   │
 │────────────────────────────│─────────────────────│───────────────────│
 │  OrderBook::add_order      │                     │                    │
 │────────────────────┬───────│                     │                    │
 │ OrderBook::match() │mutex  │                     │                    │
 │──────────────┬─────│       │                     │                    │
 │ std::map ops │deque│       │                     │                    │
 └──────────────┘─────┘───────┘─────────────────────┘────────────────────
```

- **宽度** = 该函数占用的 CPU 时间比例（越宽 = 越值得优化）
- **高度** = 调用深度（一般不重要，重点看宽度）
- **平顶山** = 在该函数本身花了大量时间（不是子调用），是优化目标

常见火焰图模式：

| 模式 | 含义 |
|------|------|
| 宽平顶山（`std::_Rb_tree`） | `std::map` 的红黑树操作耗时，考虑用 `flat_map` 或 `unordered_map` |
| 宽平顶山（`malloc/free`） | 频繁堆分配，使用 pool 或预分配 |
| 宽平顶山（`pthread_mutex`） | 锁争用，考虑 lock-free 或减小锁粒度 |
| 调用栈只有一层 | 帧指针被优化掉，需要 `-fno-omit-frame-pointer` |

---

## 四、perf stat：一行命令诊断 cache miss

在运行 `perf record` 前，可以先用 `perf stat` 快速确认是否存在 cache miss 问题：

```bash
taskset -c 0 perf stat \
    -e cycles,instructions,cache-misses,cache-references,branch-misses \
    ./build/release/bench_order_book 2>&1 | tail -15
```

示例输出解读：

```
 5,234,123,456  cycles                    # 总 CPU 周期
 8,123,456,789  instructions              # IPC = 8.12 / 5.23 ≈ 1.55（正常偏低）
     1,234,567  cache-misses              # LLC cache miss
    12,345,678  cache-references          # LLC 总访问
       123,456  branch-misses             # 分支预测失败

# cache miss rate = 1,234,567 / 12,345,678 ≈ 10%（> 5% 值得优化）
```

重要指标：
- **IPC（Instructions Per Cycle）**：< 1.0 说明存在流水线停顿（cache miss 是常见原因）
- **cache miss rate**：> 5% 值得调查，> 10% 是明显瓶颈
- **branch miss rate**：> 1% 可考虑 branchless 优化

---

## 五、本项目的 perf 分析结论

> 本节内容在你运行 `./scripts/run_perf.sh` 后根据实际火焰图填入。

### 预期热点（基于代码分析）

1. **`std::_Rb_tree::*`（std::map 操作）**
   - `bid_levels_.begin()` / `ask_levels_.begin()`：获取最优价格档位
   - 红黑树的 `begin()` 是 O(1)（维护了最左节点指针），开销不大
   - 但插入/删除是 O(log n)，会触发树旋转（cache 不友好）

2. **`std::deque::pop_front` / `std::vector::emplace_back`**
   - deque 版本：chunk 边界处的内存跳转
   - 优化后：vector 连续访问，cache 友好

3. **`std::mutex::lock/unlock`**
   - 在 benchmark（单线程）场景下，无争用的 mutex 开销约 10~20ns
   - 可通过火焰图确认是否出现在热路径上

### 优化后预期改善

通过本课的工具验证第十课的优化效果：
- vector + head 游标应减少 `match()` 中的 cache miss
- `reserve(65536)` 应消除 rehash 造成的周期性延迟尖峰

---

## 六、本课小结

| 工具 | 用途 | 一行命令 |
|------|------|----------|
| `perf stat` | 硬件事件统计（IPC, cache miss, branch miss） | `perf stat -e cycles,cache-misses ./binary` |
| `perf record` | 采集调用栈样本 | `perf record -g -F 99 ./binary` |
| `perf report` | 交互式文字报告 | `perf report --stdio` |
| `perf script` | 导出文本（FlameGraph 输入） | `perf script \| stackcollapse-perf.pl \| flamegraph.pl` |
| `scripts/run_perf.sh` | 本项目一键脚本 | `./scripts/run_perf.sh ./build/release/bench_order_book` |

**核心原则**：
> 不要盲目优化——先用 perf 确认热点在哪，再针对性修改。
> 火焰图是"让数据说话"的最直观工具，也是向面试官展示工程深度的有力佐证。

---

## 下一课预告

**第十课**：基于 perf 分析结果，我们实施三项针对性优化——
`deque → vector + head 游标`（cache locality）、
`unordered_map reserve`（消除 rehash），
以及 `match()` 中的 `trades.reserve(4)`（减少堆分配），
并用 benchmark 数据量化每项优化的效果。
