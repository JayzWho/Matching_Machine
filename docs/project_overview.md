# 低延迟撮合引擎（C++20）— 项目介绍

---

## 1. 项目概述

### 是什么

一个用 C++20 从零实现的**交易所撮合引擎核心组件**，支持限价单（Limit）与撤单（Cancel）的价格-时间优先撮合（FIFO 优先），具备完整的多线程流水线架构、RDTSC 端到端延迟测量体系，以及 perf 驱动的数据结构优化闭环。

### 解决什么问题

现代电子交易所每秒需要处理数百万笔订单，并对延迟极度敏感（P99 目标通常 < 1 ms）。本项目以"如何在 C++ 中设计并验证一个低延迟撮合引擎"为主线，逐步解决以下工程问题：

1. **线程安全传递订单**：生产者线程生成订单，消费者线程撮合，如何无锁解耦？
2. **消除热路径堆分配**：每笔订单一次 `malloc` 在高频下累计成为 ~20% CPU 热点，如何消除？
3. **精确度量端到端延迟**：跨线程延迟如何测量才准确？`std::chrono` 的精度够吗？
4. **数据结构瓶颈**：`std::unordered_map` 在高频 lookup 下占用 ~32% CPU，如何优化？

### 演进路径

```
Mutex + deque/unordered_map    (baseline)
    → lock-free SPSC + v1 OrderBook    (吞吐 1.19 → 1.75 M/s)
    → flat_hash_map + vector+head      (MixedWorkload 吞吐 +60%)
    → TradeRingBuffer + zero-alloc     (单线程 +30%, 热路径零堆分配)
    → 完整 SPSC 双线程流水线            (vs mutex: 吞吐 2.65×, P99 5.1×)
```

每一步改动均有 `perf` 火焰图 + Google Benchmark 数字背书，详见 `learning_notes/`。

---

## 2. 系统架构

### 2.1 高层流水线

```
生产者线程                                消费者线程
──────────────────────                   ──────────────────────
FeedSimulator::generate_random()
  │ 生成订单参数（随机游走价格）
  ▼
OrderMemoryPool::allocate()               ┌── try_pop() [acquire] ◄───────────┐
  │ 预分配池，O(1)，无 malloc              │                                   │
  ▼                                       ▼                                   │
*slot = raw_order（填充字段）         OrderBook::add_order_noalloc()           │
  │                                       │ 价格-时间优先撮合                  │
  ▼                                       ▼                                   │
slot->timestamp_ns = RDTSC()         TradeRingBuffer<4096>                    │
  │ 延迟测量起点                           │ push_trade()（纯数组写，无堆分配） │
  ▼ [order_queue_]                        ▼                                   │
try_push() [release] ───────────►   latency = RDTSC() - timestamp_ns          │
                                         │ record 到 LatencyRecorder          │
                                         ▼ [return_queue_]                    │
pool.deallocate(ptr) ◄─── try_pop() ◄── try_push(filled Order*)  ─────────────┘
  │ 生产者独占 deallocate
```

**两条 SPSC 队列**：
- `order_queue_`：Producer → Consumer，传递 `Order*`（工作流向）
- `return_queue_`：Consumer → Producer，归还已完成的 `Order*`（内存回收通道）

**关键设计约束**：`OrderMemoryPool` 的 `allocate()` 和 `deallocate()` **始终只在生产者线程调用**，线程安全完全由 SPSC 队列的 `release`/`acquire` 内存序保证，无需任何 mutex。

### 2.2 OrderBook 数据结构

```
bid_levels_   std::map<price, PriceLevel, std::greater<>>    // 买方价格降序
               └── PriceLevel { std::vector<Order*> orders; size_t head; }
                    ↑ head 游标：O(1) pop_front，无元素移动
                    ↑ compact() 均摊回收：空洞超 50% 时触发

ask_levels_   std::map<price, PriceLevel>                    // 卖方价格升序

order_index_  absl::flat_hash_map<order_id, Order*>
               reserved(65536) — benchmark 期间零 rehash
```

`std::map::begin()` 直接给出最优报价（O(1) access），`flat_hash_map` 的 Swiss Table
open-addressing 将撤单 lookup 从 ~32% CPU 热点降至 ~12%。

### 2.3 Order 结构与 Cache Line 纪律

`Order` 结构：`alignas(64)`，`sizeof == 64`（`static_assert` 强制），恰好一条 cache line。
关键字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `order_id` | `uint64_t` | 唯一标识 |
| `symbol` | `char[8]` | 品种，固定数组（无 `std::string` 堆分配） |
| `price` | `int64_t` | 整数定价，6 位小数精度（100.123456 → 100123456） |
| `quantity` / `filled_qty` | `int64_t` | 数量和已成交量 |
| `timestamp_ns` | `uint64_t` | RDTSC 时间戳，兼作生命周期延迟起点 |
| `side` / `type` / `status` | enum | 方向、类型、状态（PENDING/FILLED/CANCELLED） |

整数定价消除浮点比较的精度陷阱，与 CME MDP 3.0 及主流交易所 Level 2 行情编码一致。

### 2.4 Order 生命周期

```
pool.allocate()  →  [生产者持有：填充字段 + 写 RDTSC]
    → try_push(slot) [release]
    → [消费者 try_pop()] [acquire：所有字段可见]
        ├─ 立即成交/撤单：
        │   order 处理完毕 → return_queue_.try_push(order)
        │   生产者取出 → pool.deallocate(order)
        │
        └─ 挂单（未立即成交）：
            order* 存入 PriceLevel::orders + order_index_
            等待对手单到来…
            match_noalloc 成交时：deallocator(resting)
            → return_queue_.try_push(resting) → pool.deallocate(resting)
```

---

## 3. 核心技术

| 组件 | 技术选型 | 选型原因 |
|------|----------|----------|
| 订单队列 | `SPSCRingBuffer<T, N>`（自实现） | 无 mutex，无 syscall；`head_`/`tail_` 各 `alignas(64)` 消除生产者/消费者 core 间 false sharing |
| 内存管理 | `OrderMemoryPool`（自实现 free-list） | `glibc ptmalloc` 单次分配约 50~500 ns 且有尾刺；pool 稳定 < 10 ns，生产者独占消除竞争 |
| Trade 收集 | `TradeRingBuffer<4096>`（自实现） | 替代每次 `add_order` 返回的 `std::vector<Trade>`，消除 trade 构造、`map`/`vector` 扩容等热路径堆分配 |
| 订单索引 | `absl::flat_hash_map`（Swiss Table） | open-addressing + SIMD 批量探测；将 hash map CPU 占比从 ~32% 降至 ~12%（perf 测量） |
| 价格层 | `std::map` + `PriceLevel` vector + head 游标 | `map::begin()` = 最优报价 O(1)；vector 连续内存优于 `deque` 分段结构；head 游标替代 `erase` |
| 延迟测量 | RDTSC（`__rdtsc`）+ P 分位数排序 | `std::chrono` 约 50~200 ns 开销；RDTSC 约 5~20 cycles；SPSC `acquire/release` 保证跨线程可见性 |
| 构建系统 | CMake + FetchContent | 自动拉取 Google Benchmark、GTest、abseil-cpp |
| 性能分析 | `perf record -g --cpu-clock` + FlameGraph | 云 VM 硬件 PMU 不可用；`cpu-clock` 软件采样做函数级热点定位 |

---

## 4. 关键设计决策

### 4.1 SPSC vs mutex：为什么不用 condition_variable？

`std::mutex` + `condition_variable` 每次 `notify_one` 需走 `futex` 系统调用。在腾讯云 VM 上，单次唤醒延迟约 **10~50 μs**。100K 订单每条触发一次，总 context switch 开销约 1~5 s 量级。

SPSC 自旋（busy-wait）完全在用户态，vCPU 让权仅由 hypervisor 时间片触发（周期约 1~4 ms），context switch 次数低 **2~3 个数量级**。

实测对比（100K 规模，VM 环境）：
- 吞吐：Mutex 1.19 M/s → SPSC **3.15 M/s**（**2.65×**）
- P99 延迟：12.59 ms → **2.49 ms**（**5.1×**）

### 4.2 为什么内存池只在生产者线程操作？

若允许消费者直接调用 `pool.deallocate()`，则需要对 pool 加 mutex——正好引入了我们想消除的那种同步开销。

**归还队列方案**：消费者处理完 `Order*` 后将其推入 `return_queue_`（第二条 SPSC），生产者在每次 `allocate()` 前先 drain 归还队列。这样：
- `OrderMemoryPool` 完全不需要线程安全
- 两条 SPSC 队列的 `release/acquire` 即提供全部内存序保证
- 无额外锁，无额外 syscall

### 4.3 `add_order_noalloc` 的模板 Deallocator 参数

原 `add_order` 返回 `std::vector<Trade>`，每次调用有堆分配。新接口：

```cpp
template<size_t Cap, typename Deallocator>
void add_order_noalloc(Order* order, TradeRingBuffer<Cap>& trade_buf,
                       const Deallocator& deallocator);
```

- `TradeRingBuffer`：Trade 写入预分配数组，无堆分配
- `const Deallocator&`（模板参数 vs `std::function`）：零虚函数调用，零 closure 堆分配，lambda 在编译期完全内联；多次调用语义用 `const&`（`&&` 转发引用语义为"只用一次"，与此处不符）
- 原 `add_order` 接口完整保留（开闭原则），旧测试和 mutex baseline 零改动

### 4.4 Cache line 纪律

- `Order`：`alignas(64)`，`sizeof == 64`，`static_assert` 强制——order 整体恰好占一条 cache line，无跨行读
- SPSC `head_`/`tail_`：各 `alignas(64)` 独占 cache line——消除生产者/消费者更新各自指针时的 false sharing
- `Order::symbol`：`char[8]` 固定数组（非 `std::string`）——字符串在同一 cache line 内，无堆间接

### 4.5 惰性删除：O(1) 撤单

`deque::erase` 是 O(n)（移动后续所有元素）。改为：
- 撤单时将 `PriceLevel::orders[i]` 置 `nullptr`（O(1)）
- 撮合循环跳过 `nullptr`（惰性）
- `compact()` 均摊回收：空洞比例超 50% 时重建 vector

实测 `BM_CancelOrder` 延迟降低 **55.9%**（60,456 → 26,659 ns）。

### 4.6 整数定价

所有价格以 `int64_t` 存储，6 位小数精度（100.123456 → `100123456`）。避免 IEEE 754 浮点比较的精度陷阱，这是正确性需求，与 CME MDP 3.0 编码一致。

---

## 5. 性能与结果

> **测试环境**：腾讯云 VM-0-3-ubuntu，2 vCPU @ 2494 MHz，Release（-O2）  
> **运行**：`taskset -c 0,1 ./build/release/bench_matching_engine`  
> **注意**：虚拟化环境硬件 PMU 不可用（cycles/IPC/cache-miss 均 `<not supported>`）；  
> 延迟由 RDTSC 时钟周期换算，绝对值因 VM hypervisor 调度偏高，相对改善比例有效。

### 5.1 OrderBook 微基准——优化演进

| Benchmark | v1 基线（deque + unordered_map） | v3 优化（vector+head + flat_hash_map） | 改善 |
|-----------|:---:|:---:|:---:|
| BM_AddOrder_NoMatch | 83.5 ns | 46.4 ns | **+44%** |
| BM_CancelOrder | 60,456 ns | 26,659 ns | **+56%** |
| BM_AddOrder_SweepLevels/20 | 3,093 ns | 2,297 ns | **+26%** |
| BM_MixedWorkload 吞吐 | 3.87 M ops/s | 6.19 M ops/s | **+60%** |

`absl::flat_hash_map` 将 hash map CPU 占比从 ~32% 降至 ~12%（perf 火焰图测量）。

### 5.2 流水线吞吐量对比（真实吞吐 = order_count / real_time）

| 规模 | Mutex 基线 | SPSC 优化版 | 提升倍数 |
|------|:---:|:---:|:---:|
| 10K  | 1.22 M/s | 1.75 M/s | 1.43× |
| 50K  | 1.17 M/s | 3.23 M/s | 2.76× |
| 100K | 1.19 M/s | 3.15 M/s | **2.65×** |

Mutex 版吞吐在 50K→100K 基本持平（~1.19 M/s）：`condition_variable` 内核调度成为瓶颈，无法随规模扩展。SPSC 版稳定在 3.15~3.23 M/s。

### 5.3 端到端延迟分布（100K 订单，RDTSC）

| 百分位 | Mutex 基线 | SPSC 优化版 | 改善 |
|:---:|:---:|:---:|:---:|
| P50  | ~4.77 ms | ~1.26 ms | **3.8×** |
| P95  | ~11.88 ms | ~2.03 ms | 5.9× |
| P99  | ~12.59 ms | ~2.49 ms | **5.1×** |
| P999 | ~12.66 ms | ~2.84 ms | 4.5× |
| Max  | ~12.67 ms | ~2.84 ms | 4.5× |

SPSC P999 ≈ Max（2.84 ms）：尾延迟集中、无严重离群，分布形态健康。  
VM 环境下毫秒级 P50 属正常（hypervisor 时间片约 1~4 ms）；裸金属同等架构 P50 通常 200~800 ns。

### 5.4 noalloc 接口改进（单线程）

| 接口 | 吞吐 | 说明 |
|------|:---:|------|
| `add_order`（vector<Trade> 返回） | 6.19 M ops/s | v3 数据结构优化后的基线 |
| `add_order_noalloc` | **8.04 M ops/s** | **+30%**，消除热路径堆分配 |

与 perf 报告一致：v3 阶段 `_int_malloc` + `_int_free` 合计约 20% CPU（trade vector 构造、order 构造、map 扩容等多来源），noalloc 改造后降至可忽略。

---

## 6. 个人贡献

本项目为独立完成，以下按技术难度列出核心模块：

### 核心模块实现

- **`SPSCRingBuffer<T, N>`**：lock-free 单生产者单消费者环形队列；`head_`/`tail_` 各 `alignas(64)`，`try_push` 用 `release`，`try_pop` 用 `acquire`；正确性通过 ThreadSanitizer 验证
- **`OrderMemoryPool`**：free-list 内存池，placement new，`allocate`/`deallocate` 均 O(1)；无锁设计（单线程独占），消除热路径 `malloc` 尾刺
- **`TradeRingBuffer<N>`**：预分配 Trade 缓冲；环形覆盖语义；与 `SPSCRingBuffer` 的核心区别：无原子操作，消费者线程独占写
- **`OrderBook`（v1→v3 优化）**：从 `deque`+`unordered_map` 迭代至 `vector+head游标`+`flat_hash_map`；`add_order_noalloc` 模板接口；惰性删除+`compact()` 均摊回收
- **`MatchingEngine`（SPSC 流水线）**：双 SPSC 队列架构；归还队列内存回收机制；RDTSC 跨线程延迟测量；`start()`/`stop()` 生命周期管理
- **`LatencyRecorder`**：RDTSC 采样，P50/P95/P99/P999/Max 分位数报告，CPU 频率换算为纳秒

### 优化工作

- 用 `perf record -g --cpu-clock` + FlameGraph 定位 `std::unordered_map` 热点（~32% CPU），迁移至 `absl::flat_hash_map`，热点降至 ~12%
- 识别 `std::vector<Trade>` 堆分配为 ~20% CPU 热点，设计 `TradeRingBuffer` + `Deallocator` 模板方案消除
- 设计并实现 mutex baseline benchmark，量化 SPSC 方案的实际改善幅度（吞吐 2.65×，P99 5.1×）
- 通过 `taskset` 绑核、`alignas(64)` cache line 对齐、整数定价等细节优化保证测量和实现质量

### 测试与验证

- 33 个 GTest 用例（OrderBook / SPSC / MemoryPool / FeedSimulator / MatchingEngine）
- 覆盖：完全成交、部分成交、撤单、多级扫单、并发生产者-消费者场景
- AddressSanitizer（内存越界/UAF）+ ThreadSanitizer（数据竞争）全程开启验证

### 简历要点（草稿，可按需对外）

> 建议口径：性能数据来自云 VM（2 vCPU，2.494 GHz，Release -O2，`taskset -c 0,1`，100K orders，cancel_ratio=0.2）。
> VM 下绝对延迟会受 hypervisor 调度放大，但 mutex baseline 与 SPSC 的**相对改善比例**用于对比是可靠的。

- **无锁流水线**：用 C++20 设计并实现双线程撮合流水线（Producer/Consumer），以双 SPSC 队列替换 `std::mutex` + `condition_variable`；在 100K 订单规模下实现吞吐 **2.65×**（1.19 → 3.15 M orders/s），P99 延迟 **5.1×** 改善（12.59 → 2.49 ms），使用 RDTSC 端到端计时。
- **热路径零堆分配**：实现 `OrderMemoryPool`（free-list + placement new，O(1)）与 `TradeRingBuffer`（预分配环形缓冲），并用 noalloc API 消除 trade 构造/频繁扩容等堆分配；单线程 `add_order_noalloc` 吞吐 **+30%**（6.19 → 8.04 M ops/s）。
- **perf 驱动的数据结构优化**：通过 `perf record -g --cpu-clock` + FlameGraph 定位 `std::unordered_map` 为主要热点（~32% CPU），迁移至 `absl::flat_hash_map`（Swiss Table），热点降至 ~12%，混合负载吞吐 **+60%**（3.87 → 6.19 M ops/s）。
- **缓存友好与一致性**：用 `alignas(64)` 强制 `Order` 恰好 64 字节并保持关键字段同 cache line；SPSC `head_`/`tail_` 分别对齐 64 字节以避免生产者/消费者间 false sharing；价格采用 6 位精度的整数编码避免浮点比较陷阱。
- **并发正确性与内存安全**：通过“归还队列”保证 `OrderMemoryPool` 仅由生产者线程访问，利用 SPSC `release/acquire` 作为唯一同步手段；配套 33 个 GTest + ASan/TSan 覆盖撮合/撤单与并发场景。

（如需英文版，我建议面试/投递时再从上述 5 条压缩成 2~3 条更短的 bullets。）
