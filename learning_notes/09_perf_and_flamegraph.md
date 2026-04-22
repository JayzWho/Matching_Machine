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

## 三、perf report 与火焰图阅读指南

### 3.1 perf report 关键字段

```
Overhead  Samples  Symbol
  9.22%     1522   std::__detail::_Map_base::operator[]
  7.67%     1265   me::OrderBook::add_order
  4.77%      787   malloc
```

- **Overhead**：该符号占全部 cpu-clock 采样的百分比，即 CPU 时间占比。
- **Samples**：该符号被采样命中的次数，与 Overhead 成正比。
- **Symbol**：函数符号名（需要 `-g` 编译和符号文件）。

> **本项目环境说明（重要）**：本项目运行于**腾讯云 Ubuntu 虚拟机**，
> 由 hypervisor 虚拟化支持。`perf stat` 中 `cycles`、`instructions`、
> `branches`、`branch-misses` 全部显示 `<not supported>`，
> **硬件 PMU 计数器（IPC、cache-miss rate、branch-miss rate）完全不可用**。
> 所有 perf 分析只能依赖 `cpu-clock` 软件事件的采样做**函数级热点定位**，
> perf report 中 IPC 列全部为 `-`，调用链叶节点多为无法解析的匿名地址，均与此有关。
> 需在所有性能结论中明确标注这一限制。

### 3.2 如何阅读火焰图

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

## 四、为什么不能只对整个 bench 运行 perf？

`bench_order_book` 包含多种 BM 函数，每种场景的热路径完全不同。对整体运行 perf 存在两个根本问题：

**问题 1：`BM_MixedWorkload` 独占约 48% 的 CPU 时间，稀释了其他场景的信号。**

根据 baseline benchmark 结果（bench_updated 版，2026-04-14）反推各 BM 在整个 perf run 中的实际 CPU 时间占比：

| BM 函数 | CPU Time/次 | 迭代次数 | 估算总 CPU 时间 | 占比 |
|---------|------------|---------|----------------|------|
| `BM_AddOrder_NoMatch` | 83 ns | ~9.0 M | ~747 ms | ~4.5% |
| `BM_AddOrder_FullMatch` | 753 ns | ~994 K | ~748 ms | ~4.5% |
| `BM_AddOrder_SweepLevels`（4档合计）| 均值~1750 ns | ~4.3 M | ~2,800 ms | ~17.0% |
| `BM_CancelOrder` | 60 μs | ~12 K | ~723 ms | ~4.4% |
| `BM_MixedWorkload` | 258 ms/轮 | 50轮 | ~12,900 ms | **~54%** |
| `PauseTiming` 重建开销（perf 仍采样） | - | - | ~2,500 ms | ~15% |
| 合计 | | | ~20,400 ms | ✓（估算） |

> **注意**：相比旧版 benchmark，`PauseTiming` 开销大幅降低（~32% → ~15%），因为新 bench 的
> SweepLevels 和 CancelOrder 改用 `clear()` 代替重建 `OrderBook`，仅重填订单而非整体析构/构造。
> 这使得混合 perf 的信噪比有所改善，但 `BM_MixedWorkload` 的 CPU 时间占比进一步提升至约 54%，
> 对整体 perf 的稀释效应依然存在，分场景 perf 的必要性不变。

**问题 2：`PauseTiming` 区间内的开销（约 15%）仍污染采样；SweepLevels 额外引入 `clock_gettime` 噪声。**

`perf record` 全程采样，不感知 `PauseTiming` 的边界，会把 `PauseTiming` 内
重填订单的开销（`deque` 初始化、order 插入）都纳入采样，导致
`_Deque_base::_M_initialize_map`、`~_Deque_base` 等符号出现在热点列表里，
但这些并**不在被测路径上**。新 bench 虽将此噪声从 ~32% 降至 ~15%，
但 SweepLevels 场景额外引入了 `PauseTiming`/`ResumeTiming` 的 `clock_gettime` 调用噪声（高达约 43%）。

**结论：混合 perf 只能给出全局轮廓，无法为具体场景的优化决策提供精确依据。**
应使用 `--benchmark_filter` 对各 BM 分别运行 perf，以获得纯净的场景热点数据。

---

## 五、分场景 perf 测试计划

基于第八章 baseline benchmark 的结果，按照"优化收益 × 问题复杂度 × perf 信噪比"
三个维度，对 bench_order_book 的各子 BM 制定如下分级 perf 计划。

---

### 5.1 P0：优先执行（决策关键）

#### P0-A：`BM_MixedWorkload`——真实场景稳态热点

**理由**：
- `MixedWorkload` 以真实业务比例（70% 挂单 + 20% 成交 + 10% 撤单）混合运行，
  是整个性能优化的**核心指标载体**（baseline 3.69 M orders/s）。
- 它在混合 perf 中已独占约 54% CPU 时间，但因与其他 BM 混杂，调用树不纯净。
- 单独 perf 后，可以看到在真实流量分布下 `match()`、`order_index_`、`mutex`
  各自的相对权重，确认**稳态最大热点**。
- 每次优化后只需对 `MixedWorkload` 重新 perf，观察热点是否转移，即可判断优化效果。

**运行命令**：

```bash
# 编译（确保 RelWithDebInfo 带 -fno-omit-frame-pointer）
cmake --build build/release -j$(nproc)

# ── 方式一：使用脚本一键执行（推荐）──
# 第三参数为 benchmark_filter，输出文件自动命名为 *_BM_MixedWorkload.*
./scripts/run_perf.sh ./build/release/bench_order_book "" "BM_MixedWorkload"
# 输出：results/perf_BM_MixedWorkload.data
#       results/perf_report_BM_MixedWorkload.txt
#       results/flamegraph_BM_MixedWorkload.svg

# ── 方式二：手动逐步执行 ──
# perf record：单独运行 BM_MixedWorkload
taskset -c 0 perf record \
    -g \
    --call-graph=fp \
    -F 99 \
    -o results/perf_mixed.data \
    -- ./build/release/bench_order_book \
       --benchmark_filter="BM_MixedWorkload" \
       --benchmark_min_time=10s

# 生成文本报告
perf report -i results/perf_mixed.data \
    --stdio --no-children --sort=symbol -n \
    2>/dev/null > results/perf_report_mixed.txt
head -60 results/perf_report_mixed.txt

# 生成火焰图
perf script -i results/perf_mixed.data \
    | ~/FlameGraph/stackcollapse-perf.pl \
    | ~/FlameGraph/flamegraph.pl \
        --title "BM_MixedWorkload — Baseline" \
        --width 1600 --colors hot \
    > results/flamegraph_mixed_baseline.svg
```

**预期回答的问题**：
- 在真实流量比例下，`order_index_`（unordered_map）的 `operator[]` 和 `_M_erase`
  合计是否仍是第一大热点？
- `match()` 路径（`map::erase` + `deque::pop_front` + `trades` malloc）占多少比例？
- `mutex` lock/unlock 的稳态开销是多少？

---

#### P0-A perf 结果分析（`BM_MixedWorkload` baseline，bench_updated）

> 采样总数：1,269 samples（`cpu-clock:pppH`，99 Hz，虚拟机环境，IPC 列全部为 `-`，
> 调用链叶节点多为匿名地址——均为腾讯云 VM hypervisor 限制，不影响函数级热点定位。）
>
> **与旧版 bench 的区别**：新 bench 的 BM_MixedWorkload 不再在 `PauseTiming` 内
> 重建 OrderBook，订单 ID 持续累加，order book 在整个 perf run 中长期保持有状态。
> 这使得 `~OrderBook` 噪声进一步减少（由 2.55% 降至 2.05%），但堆分配内部函数
>（`_int_malloc`、`malloc_consolidate`、`_int_free`、`unlink_chunk`）大量出现，
> 反映长期运行下 glibc malloc 的碎片整理开销。

**热点汇总表**

| 排名 | 符号 | Overhead | Samples | 归属 |
|------|------|----------|---------|------|
| 1 | `me::OrderBook::add_order` | 23.94% | 304 | 挂单主路径 |
| 2 | `unordered_map::operator[]`（`_Map_base::operator[]`） | 23.23% | 295 | `order_index_` 查找/插入 |
| 3 | `_int_malloc` | 5.83% | 74 | 堆分配（glibc 内部） |
| 4 | `_Hashtable::_M_rehash` | 5.75% | 73 | `order_index_` 扩容 rehash |
| 5 | `me::OrderBook::cancel_order` | 7.09% | 90 | 撤单主路径 |
| 6 | `_Hashtable::_M_erase`（第一处） | 4.65% | 59 | `order_index_` 删除 |
| 7 | `_Hashtable::_M_erase`（第二处） | 4.33% | 55 | `order_index_` 删除 |
| 8 | `malloc_consolidate` | 3.31% | 42 | 堆碎片整理（长期运行副产品） |
| 9 | `_int_free` | 2.44% | 31 | 堆释放（glibc 内部） |
| 10 | `BM_MixedWorkload`（框架函数本身） | 2.36% | 30 | benchmark 框架开销 |
| 11 | `me::OrderBook::match` | 2.28% | 29 | 成交路径 |
| 12 | `me::OrderBook::~OrderBook`（PauseTiming/bench 析构） | 2.05% | 26 | 噪声（已大幅降低） |
| 13 | `malloc` + `unlink_chunk` | 1.73% + 1.73% | 22+22 | 堆分配/释放 |
| 14 | `cfree` | 1.57% | 20 | 堆释放 |
| 15 | `pthread_mutex_lock` + `pthread_mutex_unlock` | 1.10% + 1.10% | 14+14 | 互斥锁开销 |
| 16 | `_Rb_tree_insert_and_rebalance` | 0.79% | 10 | 价格档位 map 插入 |
| 17 | `_Rb_tree::_M_erase` (ask/bid) ×2 | 0.24% + 0.16% | 3+2 | 价格档位 map 操作 |

**三个预设问题的实测答案**

**Q1：`order_index_`（unordered_map）是否仍是第一大热点？**

**是，结论不变。**
`operator[]`（23.23%）+ `_M_rehash`（5.75%）+ `_M_erase` 两处（4.65% + 4.33%）
合计 **~37.96%**，与旧版（38.6%）基本持平。
`_M_rehash` 持续存在（5.75%）说明新 bench 长期运行模式下，订单 ID 不断累加，
`order_index_` 的 load factor 仍反复触发 rehash（O(n) 全量 re-insert）。
**`reserve(N)` 预分配 bucket 仍是第一优先级优化目标。**

**新现象：glibc malloc 内部碎片开销显著化**

旧版 bench 主要显示 `malloc`(1.77%) + `cfree`(1.35%)，新版出现大量 glibc 内部函数：
`_int_malloc`(5.83%) + `malloc_consolidate`(3.31%) + `_int_free`(2.44%) + `malloc`(1.73%) + `unlink_chunk`(1.73%) + `cfree`(1.57%)，堆操作合计 **~16.6%**。

原因：新 bench 的 MixedWorkload 长期运行，OrderBook 内部订单数量不断增长，
大量分配/释放 `Order*` 指针、`Trade` 对象、`deque` chunk、`map` 节点，
glibc malloc 进入碎片整理（`malloc_consolidate`）和链表操作（`unlink_chunk`）阶段。
这部分开销在旧版因每次 PauseTiming 都全量重建（隐式清空堆碎片）而被掩盖。

**实际含义**：若 OrderBook 长期稳态运行（而非每轮重建），堆分配开销会进一步加剧，
这进一步强化了"减少动态分配"的优化方向。

**Q2：`match()` 路径占多少比例？**

**仅 2.28%，结论不变——match() 不是当前优化目标。**
与旧版 2.12% 基本一致。`_Rb_tree::_M_erase`（ask/bid map）合计 0.40%，
价格档位 `std::map` 操作合计 ~1.19%（含 insert + erase），仍可忽略。

**Q3：`mutex` lock/unlock 的稳态开销是多少？**

**2.20%（lock 1.10% + unlock 1.10%）**，与旧版（2.19%）基本一致。
单线程无争用，固有延迟不变。

**结论与优化优先级**

```
热点                              占比        优化动作                       预期收益
─────────────────────────────────────────────────────────────────────────────────────
order_index_ rehash（_M_rehash） 5.75%       reserve(65536) 预分配 bucket   消除 rehash，减少约 6% 开销
order_index_ 操作合计            ~38%        reserve 后 rehash 消除           直接减少约 6%；
                                              长期：换 robin-hood/flat map    可进一步降低 lookup 成本
堆分配/释放（glibc malloc 内部） ~16.6%      减少动态分配次数（reserve、pool）长期优化方向；与 order_index_ 密切相关
mutex lock/unlock                2.20%       暂缓（无争用，固有延迟小）        < 1% 改善空间
match() 路径                     ~2.3%       暂缓（权重极低）                 收益可忽略
std::map (ask/bid) 操作          ~1.19%      暂缓（权重极低）                 收益可忽略
```

**最高优先级**：`order_index_.reserve(65536)` ——一行代码，消除 rehash（约 5.75%），
同时降低 `operator[]` 的平均探测次数（load factor 降低，碰撞减少），
还能间接减少 `_int_malloc`/`malloc_consolidate` 等 rehash 触发的堆操作。

---

#### P0-B：`BM_AddOrder_SweepLevels`——裁定斜率递增的主因

**理由**：
- `SweepLevels/10`（1.69 μs）和 `/20`（3.10 μs）是延迟最高的场景，
  优化收益最大。
- 第八章观察到**斜率温和递增**（约 120→155 ns/档），有两个竞争假设：
  `trades` vector 周期性扩容 vs `map::erase` 红黑树旋转。单独 perf 才能裁定主因。
- 当前混合 perf 中 `SweepLevels` 仅占约 17% 的 CPU 时间，信号严重被稀释。

**运行命令**：

```bash
# ── 方式一：使用脚本一键执行（推荐）──
./scripts/run_perf.sh ./build/release/bench_order_book "" "BM_AddOrder_SweepLevels"
# 输出：results/perf_BM_AddOrder_SweepLevels.data
#       results/perf_report_BM_AddOrder_SweepLevels.txt
#       results/flamegraph_BM_AddOrder_SweepLevels.svg

# ── 方式二：手动逐步执行 ──
# perf record：同时跑所有 SweepLevels 档位（1/5/10/20）
taskset -c 0 perf record \
    -g \
    --call-graph=fp \
    -F 99 \
    -o results/perf_sweep.data \
    -- ./build/release/bench_order_book \
       --benchmark_filter="BM_AddOrder_SweepLevels" \
       --benchmark_min_time=10s

# 文本报告
perf report -i results/perf_sweep.data \
    --stdio --no-children --sort=symbol -n \
    2>/dev/null > results/perf_report_sweep.txt
head -60 results/perf_report_sweep.txt

# 火焰图
perf script -i results/perf_sweep.data \
    | ~/FlameGraph/stackcollapse-perf.pl \
    | ~/FlameGraph/flamegraph.pl \
        --title "BM_AddOrder_SweepLevels — Baseline" \
        --width 1600 --colors hot \
    > results/flamegraph_sweep_baseline.svg
```

**预期回答的问题**：
- `std::_Rb_tree_insert_and_rebalance`（map insert）与
  `std::_Rb_tree_rebalance_for_erase`（map erase）各占多少？
- `vector::_M_realloc_insert`（trades 扩容）在纯 SweepLevels 场景中占多少？
- `malloc` / `cfree` 合计比例是否高于混合 perf 中的 9.8%？

---

#### P0-B perf 结果分析（`BM_AddOrder_SweepLevels` baseline，bench_updated）

> 采样总数：**15K samples**（`cpu-clock:pppH`，99 Hz），统计置信度高。
> 虚拟机环境限制同前（IPC 列全部为 `-`，匿名地址无法解析）。
>
> **与旧版 bench 的重大区别**：新 bench 的 SweepLevels 保留了 `PauseTiming`/`ResumeTiming`
> 来包裹重填订单的区间，而 perf 全程采样（不感知 PauseTiming 边界）。
> 在 99 Hz 采样下，`PauseTiming`/`ResumeTiming` 本身需要调用 `steady_clock::now()`
>（即 `clock_gettime`），这个系统调用被大量命中，成为**第一大伪热点**，
> 严重稀释了真实 OrderBook 逻辑的信号。

**热点汇总表**

| 排名 | 符号（简写） | Overhead | Samples | 归属 |
|------|------------|----------|---------|------|
| 1 | `0x0000000000000882`（→ `clock_gettime`） | **23.01%** | 3,582 | **PauseTiming/ResumeTiming 计时调用（框架噪声）** |
| 2 | `0xffffffff87fc714e`（kernel, → `clock_gettime`） | 7.68% | 1,195 | 同上（kernel 侧） |
| 3 | `0xffffffff87fd59a5`（kernel, → `clock_gettime`） | 7.64% | 1,190 | 同上（kernel 侧） |
| 4 | `_int_free` | 5.19% | 808 | 堆释放（glibc 内部） |
| 5 | `0x000000000000075e`（→ `clock_gettime`） | 4.87% | 758 | 框架计时噪声 |
| 6 | `malloc` | 4.77% | 742 | 堆分配 |
| 7 | `BM_AddOrder_SweepLevels lambda` | 4.41% | 687 | benchmark 框架 |
| 8 | `_int_malloc` | 3.83% | 596 | 堆分配（glibc 内部） |
| 9 | `me::OrderBook::add_order` | 3.73% | 580 | 挂单主路径 |
| 10 | `unordered_map::operator[]` | 3.66% | 570 | `order_index_` 查找/插入 |
| 11 | `_Hashtable::_M_erase` | 2.93% | 456 | `order_index_` 删除 |
| 12 | `me::OrderBook::match` | 2.63% | 410 | 成交主路径 |
| 13 | `cfree` | 2.31% | 360 | 堆释放 |
| 14 | `_Rb_tree_insert_and_rebalance`（map insert） | 1.35% | 210 | ask/bid map 插档 |
| 15 | `vector::_M_realloc_insert`（trades 扩容） | 1.25% | 194 | trades vector 扩容 |
| 16 | `_Rb_tree_rebalance_for_erase`（map erase） | 1.18% | 183 | ask/bid map 删档 |
| 17 | `_Deque_base::_M_initialize_map`（PauseTiming 重建） | 1.12% | 174 | 噪声 |
| 18 | `malloc_consolidate` | 0.93% | 145 | 堆碎片整理 |
| 19 | `operator new` | 0.86% | 134 | 堆分配 |
| 20 | `benchmark::State::PauseTiming` | 0.75% | 116 | 框架噪声 |
| 21 | `pthread_mutex_lock` | 0.73% | 113 | 互斥锁 |
| 22 | `_Hashtable::_M_erase`（第二处） | 0.67% | 105 | `order_index_` 删除 |
| 23 | `_Rb_tree::_M_get_insert_hint_unique_pos` | 0.48% | 74 | ask/bid map 插入辅助 |
| 24 | `pthread_mutex_unlock` | 0.45% | 70 | 互斥锁 |
| 25 | `_Deque_base::~_Deque_base` | 0.42% | 65 | PauseTiming 析构噪声 |
| 26 | `benchmark::State::ResumeTiming` | 0.39% | 60 | 框架噪声 |
| 27 | `operator delete` ×2 | 0.30% + 0.22% | 47+35 | 堆释放 |
| 28 | `_Prime_rehash_policy::_M_need_rehash` | 0.22% | 34 | rehash 判断 |

**⚠️ 关键新发现：`clock_gettime` 成为第一大伪热点（合计 ~43%）**

`clock_gettime` 相关符号（`0x882`、两个 kernel 地址、`0x75e`）合计占 **~43%** 的采样。
这是新 bench 结构的副产品：

- 每次迭代的 `PauseTiming`/`ResumeTiming` 都会调用 `steady_clock::now()`
  触发 `clock_gettime` vDSO/syscall。
- 在 99 Hz 采样下，这些调用时间够长（尤其是 VM 环境下 syscall 开销更高），
  导致 perf 大量采样到 `clock_gettime` 内部。
- 旧 bench 也有 PauseTiming，但旧版新建 OrderBook 时重建开销更重（覆盖了计时采样），
  而新版的 `clear()` 更轻量，使得框架计时开销相对凸显。

**实际含义**：剔除 `clock_gettime` 噪声（~43%）和 PauseTiming 相关噪声（~3%）后，
真实 OrderBook 逻辑约占 **54%** 的采样。下面的占比分析均以"剔除框架噪声后"为基础进行解读。

**分组汇总（剔除框架噪声后的相对权重）**

| 热点组 | 原始占比 | 剔除噪声后相对权重 | 说明 |
|--------|---------|------------------|------|
| **堆分配/释放**（malloc+_int_malloc+cfree+_int_free+new+delete+malloc_consolidate） | **~16%** | **~30%** | SweepLevels 最大热点组 |
| **`order_index_` unordered_map**（operator[]+_M_erase×2+_M_need_rehash） | **~7.5%** | **~14%** | 挂单/撤单均需 lookup |
| **`match()` 函数本身** | **2.63%** | **~5%** | 含 deque::pop_front、map 遍历 |
| **`std::map` 红黑树**（erase+insert+get_insert_pos） | **~3.0%** | **~5.6%** | 价格档位 ask/bid 操作 |
| **`trades` vector 扩容** | **1.25%** | **~2.3%** | 每次迭代重新扩容 |
| **`_Deque_base` 初始化/析构** | **~1.5%** | **~2.8%** | PauseTiming 内重填 deque 噪声 |
| **mutex lock/unlock** | **1.18%** | **~2.2%** | 单线程固有延迟 |

**三个预设问题的实测答案**

**Q1：`_Rb_tree_insert_and_rebalance` 与 `_Rb_tree_rebalance_for_erase` 各占多少？**

- `_Rb_tree_insert_and_rebalance`（map insert）：**1.35%**（210 samples）（旧 2.32%）
- `_Rb_tree_rebalance_for_erase`（map erase）：**1.18%**（183 samples）（旧 3.16%）
- `_Rb_tree::_M_get_insert_hint_unique_pos`：**0.48%**（74 samples）
- `_Rb_tree::_M_erase` / get_insert_unique_pos 等（可能被匿名地址覆盖）：**~0.05%**
- 合计：**~3.06%**（旧 ~6.17%）

占比下降原因：大量采样被 `clock_gettime` 占据，稀释了 map 操作的相对比例。
剔除噪声后估算相对权重约 5.6%，与旧版结论方向一致——`_Rb_tree_rebalance_for_erase`
随扫档数量 N 线性递增，仍是斜率递增的贡献因素之一。

**Q2：`vector::_M_realloc_insert`（trades 扩容）占多少？**

**1.25%**（194 samples，旧 1.36%）。结论不变，非斜率递增主因。

**Q3：`malloc`/`cfree` 合计是否高于 MixedWorkload？**

SweepLevels 堆操作（`malloc`+`_int_malloc`+`cfree`+`_int_free`+`malloc_consolidate`+
`operator new/delete`×2）合计约 **~16%**，仍远高于 MixedWorkload（~16.6%，但后者
样本数少得多）。剔除 clock_gettime 噪声后 SweepLevels 堆分配的相对权重约 30%，
是所有热点中**最重**的组。

**关键结论：裁定竞争假设（结论不变）**

> **第八章提出的两个假设：`trades` vector 扩容 vs `map::erase` 红黑树旋转。**

实测结果（剔除 clock_gettime 噪声后）：
- 堆分配（以 `_int_malloc`/`malloc`/`cfree`/`_int_free` 等为代表）：**最大热点组 ~30%**
- `_Rb_tree_rebalance_for_erase`（1.18%）+ `_Rb_tree_insert_and_rebalance`（1.35%）= **2.53%**
- `vector::_M_realloc_insert` = **1.25%**

**裁定结论：斜率递增的主因是堆分配，而非 vector 扩容。结论与旧版一致，只是绝对数字因
`clock_gettime` 稀释而降低。** 核心逻辑路径：每档扫单触发 `map::erase` → map 节点 free
→ `match()` 追加 Trade → `operator new`/`malloc`，N 档线性放大，
堆操作随 N 近似线性递增与观测到的延迟斜率吻合。

**与 P0-A（MixedWorkload）的对比**

| 热点 | MixedWorkload | SweepLevels（原始） | SweepLevels（去噪后估算） | 差异解读 |
|------|-------------|------------------|------------------------|---------|
| `order_index_` 操作合计 | ~38% | ~7.5% | ~14% | Mixed 中挂单比例高（70%），order_index_ 占主导 |
| `match()` | 2.28% | 2.63% | ~5% | SweepLevels 100% 触发成交，权重上升 |
| 堆分配合计 | ~16.6% | **~16%** | **~30%** | N 档扫单线性放大 malloc 次数 |
| `std::map` 红黑树 | ~1.19% | **~3.06%** | **~5.6%** | 每档 erase 的树旋转在 SweepLevels 显著 |
| mutex | 2.20% | 1.18% | ~2.2% | 固有开销，与场景无关 |
| `clock_gettime` 框架噪声 | —（极小） | **~43%** | — | **新 bench 结构副产品，SweepLevels 独有** |

**优化优先级（SweepLevels 视角，结论与旧版一致）**

```
热点                        占比（去噪估算）  优化动作                              预期收益
──────────────────────────────────────────────────────────────────────────────────────
堆分配合计                  ~30%            trades.reserve(N) 预分配              消除 _M_realloc_insert
                                            map 换 flat_map / 预分配节点池         可大幅减少 map 节点 malloc
order_index_ 操作           ~14%            reserve(65536) 消除 rehash            降低 operator[] 碰撞
std::map 红黑树操作          ~5.6%           长期：用 sorted array 替代 std::map   潜在收益，属大重构
vector::_M_realloc_insert   ~2.3%           trades.reserve(4)                     消除首次扩容序列
```

**当前阶段最高价值的两步优化**（无需大重构，结论与旧版一致）：
1. `order_index_.reserve(65536)` — 消除 rehash，降低 operator[] 碰撞探测次数。
2. `trades.reserve(4)` — 消除 SweepLevels 中每次迭代的 vector 扩容序列（N≤4 不触发）。

**P0-B 进一步测试建议：`clock_gettime` 噪声的影响**

新 bench 中 `clock_gettime` 占约 43% 是显著的信噪比问题。
若需要更纯净的 SweepLevels 热点分析，可考虑：
1. **去掉 PauseTiming，改用固定迭代次数**（不重建，直接循环扫档）——但这会改变 bench 语义。
2. **perf 过滤 `clock_gettime` 符号**（`perf report --dsos=bench_order_book`）——
   只看用户态 bench 二进制的符号，排除 libc/kernel 的 clock_gettime 采样。
3. **接受当前信噪比**——核心结论（堆分配是主因）已足够清晰，不影响优化决策。

目前选择方案 3，结论已足够支持优化决策。

---

### 5.2 P1：条件执行（辅助验证）

#### P1：`BM_AddOrder_FullMatch`——理解一对一成交的基础成本

**理由**：
- `FullMatch`（758 ns）是 `SweepLevels` 每档成本的基础单元，
  热路径（`match()` 内部：`map::erase` + `deque::pop_front` + `trades` malloc）
  与 `SweepLevels` 高度重叠。
- 单独 perf 可作为 `SweepLevels` 结果的**参照基准**：
  对比两者中 `map::erase` 的占比，可以判断多档扫单的额外成本来自哪里。

**运行命令**：

```bash
# ── 方式一：使用脚本一键执行（推荐），手动指定输出名 match ──
./scripts/run_perf.sh ./build/release/bench_order_book match \
    "BM_AddOrder_FullMatch|BM_AddOrder_SweepLevels"
# 输出：results/perf_match.data
#       results/perf_report_match.txt
#       results/flamegraph_match.svg

# ── 方式二：手动逐步执行 ──
# 与 SweepLevels 联合运行（共用一次 perf，节省时间）
taskset -c 0 perf record \
    -g \
    --call-graph=fp \
    -F 99 \
    -o results/perf_match.data \
    -- ./build/release/bench_order_book \
       --benchmark_filter="BM_AddOrder_FullMatch|BM_AddOrder_SweepLevels" \
       --benchmark_min_time=10s

perf report -i results/perf_match.data \
    --stdio --no-children --sort=symbol -n \
    2>/dev/null > results/perf_report_match.txt
```

---

### 5.3 P2：暂缓（优先级低）

#### P2-A：`BM_AddOrder_NoMatch`——暂不 perf

- 81.8 ns 接近硬件极限，优化方向清晰（去掉 `mutex`、换 `map` 为数组），但属于大重构，
  不是当前阶段目标。
- 每次迭代仅 84 ns，`clock_gettime`（约 20 ns）框架噪声占比约 24%，
  perf 采样统计意义有限。

#### P2-B：`BM_CancelOrder`——暂不 perf

- 60 μs 主要来自 benchmark 设计的极端深盘口场景（1000 单 × 每单独占一档），
  不代表真实撤单频率。混合 perf 中 `cancel_order` 仅占 0.75%，权重很低。
- 第十章"惰性删除"优化针对同档位多单的 O(n) erase 场景，当前 benchmark
  未覆盖该场景，perf 结论对优化决策贡献有限。

---

### 5.4 perf 计划汇总

```
优先级  BM 函数                         目的                                       输出文件
───────────────────────────────────────────────────────────────────────────────────────────────
P0-A   BM_MixedWorkload                确定稳态热点，作为优化前后对比基准           perf_report_mixed.txt
                                                                                  flamegraph_mixed_baseline.svg
P0-B   BM_AddOrder_SweepLevels         裁定斜率递增主因（trades扩容 vs map::erase） perf_report_sweep.txt
                                                                                  flamegraph_sweep_baseline.svg
P1     BM_AddOrder_FullMatch           理解单档成交基础成本，作为 SweepLevels 参照  perf_report_match.txt
P2-A   BM_AddOrder_NoMatch             暂缓（框架噪声大，优化方案明确）              —
P2-B   BM_CancelOrder                  暂缓（场景极端，权重低）                     —
```

---

## 六、本课小结

| 工具 | 用途 | 一行命令 |
|------|------|----------|
| `perf record` | 采集调用栈样本 | `perf record -g --call-graph=fp -F 99 ./binary` |
| `perf report` | 交互式 / 文字热点报告 | `perf report --stdio --no-children` |
| `perf script` | 导出文本（FlameGraph 输入） | `perf script \| stackcollapse-perf.pl \| flamegraph.pl` |
| `--benchmark_filter` | 只运行指定 BM | `--benchmark_filter="BM_MixedWorkload"` |

### `run_perf.sh` 脚本速查

```bash
# 全量 perf（运行整个 benchmark）
./scripts/run_perf.sh ./build/release/bench_order_book <output_name>

# 单个 BM perf（output_name 留空时从 filter 自动派生）
./scripts/run_perf.sh ./build/release/bench_order_book "" "<BM_函数名>"

# 多个 BM 联合 perf，手动指定 output_name
./scripts/run_perf.sh ./build/release/bench_order_book <output_name> "<BM_A|BM_B>"
```

有 `filter` 时脚本会自动将 `--benchmark_min_time` 延长至 10s（纯净信号）；
无 `filter` 全量运行时为 5s。输出文件均以 `output_name` 为前缀存放在 `results/`。

**核心原则**：
> 对整个 bench 运行 perf 只能得到全局轮廓；
> 要做优化决策，必须对关键场景（MixedWorkload、SweepLevels）单独 perf，
> 确保热点信号不被其他 BM 和 PauseTiming 噪声稀释。

---

## 下一课预告

**第十课**：P0-A 和 P0-B 已全部完成，热点已确认，进入实施阶段。
将按优先级依次实施以下三项针对性优化，每项优化后立即用 benchmark 量化效果：

> **新 baseline 数字（bench_updated，2026-04-14）**：
> - `BM_AddOrder_NoMatch`：83 ns | `BM_AddOrder_FullMatch`：753 ns
> - `BM_AddOrder_SweepLevels`：874 / 1230 / 1689 / 3100 ns（N=1/5/10/20）
> - `BM_MixedWorkload`：258 ms / 50轮 = **3.88 M orders/s**

1. **`order_index_.reserve(65536)`**（消除 rehash）
   - 依据：P0-A 中 `_M_rehash` 独占 5.75%，P0-B 中 rehash 相关约占 ~2%（clock_gettime 稀释后）
   - 预期：MixedWorkload 提升 ~6%，SweepLevels 也有一定提升

2. **`trades.reserve(4)`**（消除 vector 扩容序列）
   - 依据：P0-B 中 `_M_realloc_insert` 占 1.25%，且 SweepLevels 每次迭代都从 0 重建（`clear()` 后 vector 容量不变，但 `trades` 是局部变量每次从 0 开始）
   - 预期：SweepLevels 小档位（N≤4）提升明显，大档位（N=20）部分消除

3. **`deque<Order*>` → `vector<Order*>` + head 游标**（cache locality）
   - 依据：P0-B 中 `_Deque_base::_M_initialize_map` 和 `~_Deque_base` 仍有采样，
     且 deque 的 chunk 间接访问对 cache 不友好
   - 预期：`match()` 路径 pop_front 开销降低，SweepLevels 和 FullMatch 均受益

---

## 七、Baseline 版本综合总结与优化方向

> 本节基于 P0-A（BM_MixedWorkload）和 P0-B（BM_AddOrder_SweepLevels）的 perf 结果，
> 对 baseline 版本的主要性能热点和后续优化方向做出综合裁定。

### 7.1 Baseline 版本主要热点

当前 bench 测试 OrderBook 结构体基本操作的延迟与极限吞吐量。综合两次 perf 分析，
baseline 版本的主要热点集中在以下两个方面：

**（1）`order_index_`（`unordered_map`）的访问、插入与 rehash 开销**

- 在 MixedWorkload（真实流量分布）中，`unordered_map::operator[]` 独占 **23.23%**，
  加上 `_M_rehash`（5.75%）、`_M_erase`（9.0%），合计约 **~38%**，是绝对第一热点。
- `_M_rehash` 持续出现，说明在订单 ID 不断累加的稳态运行场景下，
  `order_index_` 的 load factor 反复超阈值，触发 O(n) 全量 re-insert。
  这是**完全可以用一行 `reserve()` 预分配消除**的无谓开销。
- `unordered_map` 的链式哈希节点（每个 `_Hash_node` 独立堆分配）在高频插入/删除场景下
  产生大量细粒度 `malloc`/`free`，进一步加剧了下面提到的堆操作压力。
- **结论**：`unordered_map` 的数据结构选型本身就不适合高频插入/删除场景，
  需要从两个层面改进：短期预留容量（`reserve`），长期考虑换用开放地址哈希（如 robin-hood hash）。

**（2）堆内存的频繁分配与释放（`malloc`/`free` 及其内部开销）**

- 在 SweepLevels（N 档扫单，被测逻辑最密集）中，堆操作相关符号
  （`malloc`、`_int_malloc`、`cfree`、`_int_free`、`malloc_consolidate`、
  `operator new/delete`）合计约 **~16%** 原始占比，剔除 `clock_gettime` 框架噪声后
  相对权重高达 **~30%**，是该场景最重的热点组。
- 这 ~30% 来自三条具体路径：
  - **`std::map`（ask/bid 价格档位）节点 alloc/free**：每档扫单触发 `map::erase` →
    释放 `_Rb_tree_node`（~48 字节），N 档扫单产生 N 次 `_int_free`，随 N 线性放大，
    是 SweepLevels 延迟斜率递增的主因。
  - **`unordered_map`（`order_index_`）节点 alloc/free**：每次挂单/成交 erase 各产生
    一次 `_Hash_node` 的 `operator new`/`_int_free`，PauseTiming 内的重建也被 perf 采样到。
  - **`vector<Trade>`（`trades`）扩容序列**：`match()` 内 `trades` 是无 `reserve` 的
    局部变量，每次迭代从容量 0 开始，N=20 档时触发 5 次 `malloc` + 5 次 `free`。
    这是三条路径中唯一可以**用一行 `reserve` 几乎零成本消除**的部分。
- MixedWorkload 中堆操作同样合计 ~16.6%，且因长期运行积累碎片，
  `malloc_consolidate`（碎片整理）和 `unlink_chunk`（链表操作）显著化。
  旧版因每轮 PauseTiming 全量重建 OrderBook（相当于隐式清空堆碎片）而将此现象掩盖，
  新版 bench 的长期有状态运行将其如实暴露。

**核心判断：当前热点尚未到达撮合逻辑本身，而是停留在数据结构和内存管理层面。**
- `match()` 函数本身在 MixedWorkload 中仅占 **2.28%**，在 SweepLevels 去噪后约 **~5%**，
  权重极低，当前不是瓶颈。
- `mutex` lock/unlock 合计约 **2.2%**（单线程无争用，固有延迟），暂不构成优化目标。
- `std::map`（ask/bid）红黑树操作约 **~5.6%**（去噪后），方向明确但属大重构，暂缓。

### 7.2 Baseline 版本优化方向

综合两项 perf 结论，以下优化按性价比排序：

| 优先级 | 优化动作 | 依据 | 预期收益 |
|--------|----------|------|----------|
| **P0** | `order_index_.reserve(65536)` | `_M_rehash` 独占 5.75%（Mixed），rehash 是可完全消除的 O(n) 操作 | 消除 rehash，降低 operator[] 平均探测次数；Mixed 提升 ~6%，并间接减少 malloc 碎片 |
| **P1** | `trades.reserve(4)` 或 `reserve(N)` | `_M_realloc_insert` 占 1.25%，SweepLevels 每轮从 0 扩容 5 次 | 消除 N≤4 的全部扩容；N=20 时扩容次数从 5 次降至 3 次；一行代码，零副作用 |
| **P2** | `deque<Order*>` → `vector<Order*>` + head 游标 | `_Deque_base` 初始化/析构仍有采样；deque chunk 间接访问 cache 不友好 | 改善 `match()` 路径 pop_front 的 cache locality，SweepLevels 和 FullMatch 均受益 |
| **P3（长期）** | 换用开放地址哈希替代 `unordered_map` | `order_index_` 合计 ~38%，链式哈希节点独立分配是根本瓶颈 | 彻底消除 `_Hash_node` 的碎片化 malloc，lookup 性能大幅提升 |
| **P4（长期）** | `std::map` → sorted array / `flat_map` | `map` 节点 malloc 是 SweepLevels 斜率递增主因 | 连续内存存储，消除 `_Rb_tree_node` 的堆分配，cache miss 大幅改善 |

**近期实施重点**（无需大重构，可立即验证）：

1. **`order_index_.reserve(65536)`**——一行代码，消除 rehash，预期 MixedWorkload 提升 ~6%，
   同时缓解 `_int_malloc`/`malloc_consolidate` 的频率。
2. **`trades.reserve(4)`**——一行代码，消除 `vector<Trade>` 的扩容序列，
   SweepLevels 小档位（N≤4）收益最显著。
3. **`deque` → `vector` + head 游标**——中等重构量，改善 `match()` cache locality，
   同时消除 `_Deque_base::_M_initialize_map` 的初始化开销。
