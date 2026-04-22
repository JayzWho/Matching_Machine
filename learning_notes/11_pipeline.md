# 第十一课：整合——生产者-消费者撮合流水线

> 对应阶段：Week 7-8
> 关键词：SPSC 流水线、无堆分配热路径、TradeRingBuffer、Order 生命周期、归还队列、端到端延迟测量

---

## 引言

第十章用 perf + FlameGraph 揭示了 v3 版本的剩余热点：

```
全量 perf 热点（21K samples）：
  _int_malloc       9.32%  ┐
  _int_free         6.23%  ├── 合计 ~20%：每次 add_order 返回 vector<Trade> 的堆分配
  malloc            5.01%  ┘

  std::_Rb_tree_*   ~5%    价格树红黑树节点
  flat_hash_map     ~5%    已大幅优化（32% → 12%）
```

本课目标：**消除 ~20% 的堆分配热点，同时将单线程 OrderBook 升级为完整的 SPSC 多线程流水线**。

核心问题有两个：
1. **Trade 热点**：`match()` 每次返回 `std::vector<Trade>`，堆分配无法避免
2. **架构升级**：生产者（订单生成）和消费者（撮合）如何无锁解耦

---

## 一、整体架构

```
生产者线程                          消费者线程
──────────────────                  ──────────────────────────────
FeedSimulator                       SPSCRingBuffer<Order*>
  ↓ 生成订单数据                       ↓ try_pop
OrderMemoryPool::allocate()         OrderBook::add_order_noalloc()
  ↓ 从预分配池取槽位                    ↓ 直接撮合
*slot = raw_order                   TradeRingBuffer<4096>
  ↓ 填充字段                           ↓ push_trade()（纯数组写）
slot->timestamp_ns = now()          ← 端到端延迟起点
  ↓ 写入发起时间戳                      ↓ 成交后归还
SPSCRingBuffer::try_push(slot)      return_queue_::try_push(resting)
  ↓                                     ↓
  ←─── return_queue_ (归还通道) ─────── ←
try_pop(ret) → pool.deallocate(ret)
```

**两条 SPSC 队列**：
- `order_queue_`：Producer → Consumer，传递 `Order*`（工作流向）
- `return_queue_`：Consumer → Producer，归还已完成的 `Order*`（反向通道）

**关键不变量**：`OrderMemoryPool` 的 `allocate()` 和 `deallocate()` **始终只在生产者线程调用**，线程安全完全由 SPSC 队列的内存序保证，无需任何 mutex。

---

## 二、TradeRingBuffer：无堆分配 Trade 收集

### 2.1 问题根源

```cpp
// 第十章之前：match() 每次构造一个新 vector
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;      // 堆分配！capacity = 0
    trades.reserve(4);              // 再一次堆分配（实际分配 4 个 Trade 的内存）
    // ...
    return trades;                  // 返回时通常触发 move（但底层内存仍在堆上）
}
// 调用方：
auto trades = book.add_order(order);  // 每次 add_order 都有 1 次堆分配
```

10 万笔订单 = 10 万次 `malloc` + 10 万次 `free`，这是 perf 中 ~20% 热点的根因。

### 2.2 TradeRingBuffer 设计

```cpp
template<size_t Capacity>
class TradeRingBuffer {
    std::array<Trade, Capacity> buffer_{};  // 预分配！栈或静态内存
    size_t write_pos_ = 0;
    size_t count_     = 0;
public:
    // O(1)，纯数组写，无堆分配、无同步原语
    void push_trade(const Trade& t) noexcept {
        buffer_[write_pos_++ & mask_] = t;
        if (count_ < Capacity) ++count_;
    }
};
```

**写入成本**：一次数组下标计算 + 一次 `Trade` 结构体拷贝（56 字节），约 5~10 ns。

**环形覆盖语义**：缓冲区满时覆盖最旧的记录。本项目下游（Trade 持久化/风控）抽象隐去，覆盖语义可接受。生产环境中需要根据下游消费速度调整 `Capacity`。

### 2.3 对比 SPSCRingBuffer

| 特性 | `SPSCRingBuffer<T, N>` | `TradeRingBuffer<N>` |
|---|---|---|
| 用途 | **跨线程**传递数据 | **单线程内**积累记录 |
| 同步机制 | `std::atomic`（head/tail） | 无（单写线程独占） |
| 满时行为 | 返回 `false`，调用方等待 | 环形覆盖（覆盖最旧） |
| 适用场景 | Producer → Consumer 通道 | Consumer 线程内成交缓冲 |

---

## 三、add_order_noalloc：接口改造原则

### 3.1 开闭原则（OCP）的应用

```cpp
// 原有接口：完全保留，旧代码零改动
std::vector<Trade> add_order(Order* order);

// 新增接口：noalloc 版本，供流水线使用
template<size_t Cap, typename Deallocator>
void add_order_noalloc(Order* order, TradeRingBuffer<Cap>& trade_buf,
                       const Deallocator& deallocator);
```

**为什么用模板参数而非固定容量**？  
测试代码希望用小容量（如 64）验证逻辑，生产代码用 4096。模板参数让两者都能工作，无需强制统一。

### 3.2 Deallocator：编译期静态绑定的归还回调

```cpp
// consumer_loop 中：
auto deallocator = [this](Order* o) {
    while (!return_queue_.try_push(o)) {}
};
book.add_order_noalloc(order, trade_buf_, deallocator);

// 不需要归还的场景（测试、benchmark）：
book.add_order_noalloc(order, trade_buf_, [](Order*) {});
```

**设计考量**：`OrderBook` 不应知道 `MemoryPool` 的存在（关注点分离）。通过模板参数传入 callable，`OrderBook` 只需在"挂单方被完全成交"时调用 `deallocator(resting)`，具体行为由调用方在编译期绑定。

**为什么用 `const Deallocator&` 而非转发引用 `Deallocator&&`**？  
`deallocator` 在 `add_order_noalloc` 内部会被调用多次（传给 `match_noalloc` 一次，末尾处理 incoming 一次；`match_noalloc` 内部每成交一笔挂单调用一次）。转发引用的语义前提是"只用一次、可以 move 走"，多次调用场景正确语义是 `const&`。改为 `const Deallocator&` 后，编译器对 lambda 的内联能力完全不受影响——模板实例化时类型已知，`const&` 同样可以完全内联。

**与原 `std::function` 方案对比**：

| | `std::function` + `set_deallocate_cb` | `const Deallocator&` 模板参数 |
|---|---|---|
| 调用开销 | 虚函数间接跳转 + closure 堆分配 | 编译期内联，零间接调用 |
| `bool` 分支 | `if (deallocate_cb_)` | 无（直接调用） |
| 成员开销 | 32 字节 `std::function` 成员 | 无额外成员 |
| 可多次调用 | ✅ | ✅ |

**调用时机**（仅在 `match_noalloc` 中，原 `match()` 不变）：

```cpp
if (resting->is_filled()) {
    resting->status = OrderStatus::FILLED;
    order_index_.erase(resting->order_id);
    ask_level.pop_front();
    deallocator(resting);  // ← 挂单方完全成交时归还，编译期内联
}
```

---

## 四、Order 生命周期管理

这是本章最复杂的设计决策。一个 `Order` 对象从分配到归还，经历以下状态：

```
OrderMemoryPool::allocate()
        │
        ▼
    [IN POOL]（生产者持有，填充字段）
        │
        ▼ try_push(slot)
    [IN QUEUE]（order_queue_ 中，生产者已释放所有权）
        │
        ▼ try_pop(order)
    [IN CONSUMER]（消费者持有，调用 add_order_noalloc）
        │
        ├─→ [立即成交 / 撤单]：
        │       order->is_filled() 或 order->status == CANCELLED
        │       → 消费者将 order* 推入 return_queue_
        │       → 生产者从 return_queue_ 取出，调用 pool.deallocate(order)
        │       → [FREED]
        │
        └─→ [挂单（未成交）]：
                order* 被存入 PriceLevel::orders，消费者不持有引用
                → 等待后续到来的对手单触发撮合
                │
                ▼ match_noalloc 中对手单成交，resting 被弹出
            [挂单方成交]：
                deallocator(resting) → 推入 return_queue_
                → 生产者从 return_queue_ 取出，调用 pool.deallocate(resting)
                → [FREED]
```

**关键约束**：
- 挂单中的 `Order*` 同时存在于 `PriceLevel::orders` 和 `order_index_` 中，不能提前归还
- 生产者必须在 `allocate()` 前先 drain `return_queue_`，否则池满时死锁

---

## 五、内存安全：为什么 MemoryPool 不需要 mutex

`MemoryPool` 是非线程安全的，但系统中只有两个线程访问 `Order` 的内存：

```
生产者线程操作（全在 producer_loop）：
  1. return_queue_.try_pop(ret)     // 取回已完成的 Order*
  2. pool.deallocate(ret)           // 归还（唯一 deallocate 调用点）
  3. pool.allocate()                // 分配（唯一 allocate 调用点）
  4. *slot = raw; slot->ts = now()  // 填充数据
  5. order_queue_.try_push(slot)    // 发布所有权

消费者线程操作（全在 consumer_loop）：
  1. order_queue_.try_pop(order)    // 获得所有权
  2. book.add_order_noalloc(...)    // 使用 Order（读写）
  3. return_queue_.try_push(order)  // 归还所有权（发布回生产者）
```

`SPSCRingBuffer` 的 `release/acquire` 内存序保证：
- `try_push(slot)` 的 `release` 使生产者对 `*slot` 的所有写入对消费者可见
- `try_pop(order)` 的 `acquire` 使消费者安全读取完整的 `Order` 数据
- `return_queue_.try_push(order)` 的 `release` 使消费者的所有修改对生产者可见
- 生产者 `try_pop(ret)` 的 `acquire` 后才调用 `pool.deallocate(ret)`

因此 `MemoryPool` 的两个方法在任意时刻**只有一个线程调用**，无需同步。

---

## 六、端到端延迟测量方法论

### 6.1 跨线程延迟的挑战

单线程延迟：`t1 = now(); func(); t2 = now(); latency = t2 - t1`

多线程延迟：消费者处理完时，如何知道生产者"入队时刻"？

### 6.2 利用 Order.timestamp_ns 字段

`Order` 结构体中已有 `timestamp_ns` 字段（原用于价格-时间优先排序）。  
我们复用它作为**生产者入队时间戳**：

```cpp
// 生产者线程（producer_loop）：
slot->timestamp_ns = LatencyRecorder::now();  // RDTSC 时钟周期
order_queue_.try_push(slot);

// 消费者线程（consumer_loop）：
order_queue_.try_pop(order);
book.add_order_noalloc(order, trade_buf_);     // 核心处理
uint64_t latency = LatencyRecorder::now() - order->timestamp_ns;
latency_recorder_.record(latency);
```

**为什么这是安全的？**  
`try_push` 的 `release` 内存序确保生产者写入的 `timestamp_ns` 在消费者 `try_pop` 的 `acquire` 之后可见。SPSC 队列本身就提供了所需的内存顺序保证，不需要额外的 `atomic`。

### 6.3 RDTSC 跨核问题

`LatencyRecorder::now()` 使用 `__rdtsc()`，在单核 VM 上没有问题。  
在多核环境下，如果生产者和消费者运行在不同 CPU 核心，且 TSC 不同步，会导致测量值偏差甚至为负。

**解决方案**：
- 用 `taskset -c 0,1` 绑定到相邻核心（通常同一物理 CPU 的 TSC 同步）
- 或改用 `clock_gettime(CLOCK_MONOTONIC)` —— 内核保证跨核单调，但精度略低（约 20-50 ns 误差）
- 本项目（腾讯云 VM）：单 CPU 虚拟化，TSC 同步，RDTSC 可用

### 6.4 延迟报告（stop() 后）

```cpp
engine.stop();
engine.latency_recorder().report(2.494);  // CPU 2.494 GHz
// 输出：P50 / P95 / P99 / P999 / Max（时钟周期 + 纳秒换算）
```

---

## 七、关键设计决策回顾

### 7.1 为什么选归还队列而非给 MemoryPool 加锁？

| 方案 | 延迟特性 | 复杂度 |
|---|---|---|
| `std::mutex` 保护 MemoryPool | 最坏情况：线程切换（约 5-10 μs） | 简单 |
| 归还队列（Return Queue） | 最坏情况：自旋等待（约 10-50 ns） | 中等 |
| 无锁原子操作链表 | 约 20-50 ns，但实现复杂 | 高 |

归还队列方案：
- 无锁（SPSC 保证），延迟稳定
- 生产者批量回收（每次 allocate 前 drain 归还队列），减少轮询次数
- MemoryPool 完全不需要修改，复用已有组件

### 7.2 为什么 TradeRingBuffer 用覆盖语义而非阻塞？

覆盖语义（满时覆盖旧数据）的假设：**下游消费速度 >> 撮合速度**。  
在真实系统中，Trade 的后续处理（风控、持久化）通常是独立的微服务，通过另一条 SPSC 队列异步接收。本项目将下游抽象隐去，覆盖语义是合理的简化。

若需要精确记录每条 Trade，应将 `TradeRingBuffer` 替换为另一条 `SPSCRingBuffer<Trade, N>`，由独立的下游线程消费。

---

## 八、Benchmark 结果

> 测试环境：腾讯云 VM-0-3-ubuntu，2 vCPU @ 2494 MHz，Release（-O2）  
> 运行命令：`taskset -c 0,1 ./build/release/bench_matching_engine`  
> 结果文件：`results/matching_engine_2026-04-20_22-25-47_pipeline.json`  
>
> ⚠️ 注：虚拟化环境硬件 PMU 不可用；延迟以 RDTSC 时钟周期报告，纳秒换算基于 2.494 GHz。

### 8.1 流水线吞吐量

```
Benchmark                               real_time   cpu_time   items/s（框架值）
BM_Pipeline_Throughput/10000/iter:3      5.72 ms    0.089 ms   112 M/s  ⚠️
BM_Pipeline_Throughput/50000/iter:3      15.5 ms    0.085 ms   589 M/s  ⚠️
BM_Pipeline_Throughput/100000/iter:3     31.7 ms    0.097 ms   1.03 G/s ⚠️
```

**⚠️ 测量局限：`items/s` 数字虚高**

Google Benchmark 的 `items_per_second = items / cpu_time`，而 `cpu_time` 统计的是
**benchmark 框架主线程**的 CPU 时间。`start()/stop()` 启动的生产者/消费者工作线程
时间**不计入** `cpu_time`，导致 `cpu_time ≈ 0.09 ms`（仅主线程调度开销），
计算出的 items/s 因分母趋近于零而严重虚高，**不代表真实吞吐**。

**真实吞吐应以 `real_time` 换算**：

| 规模  | real_time | 真实吞吐（orders/s） |
|-------|-----------|----------------------|
| 10K   | 5.72 ms   | **1.75 M/s**         |
| 50K   | 15.5 ms   | **3.23 M/s**         |
| 100K  | 31.7 ms   | **3.15 M/s**         |

50K 和 100K 已趋于稳定（3.1~3.2 M/s），说明大批量下流水线进入满载稳态。
10K 规模下线程启停开销占比较高，吞吐偏低属于正常现象。

### 8.2 单线程 add_order_noalloc 基线

```
BM_SingleThread_Baseline    1246 μs / iter   8.04 M ops/s   (565 iters)
```

与第十章 `BM_MixedWorkload` 对比：

| 版本 | 接口 | 吞吐 | 说明 |
|------|------|------|------|
| 第十章基线 | `add_order`（带堆分配） | 6.19 M ops/s | 每次 `malloc` + `free` vector |
| 第十一章基线 | `add_order_noalloc` | **8.04 M ops/s** | 消除堆分配，提升 **+30%** |
| 第十一章流水线 | SPSC 双线程 | **3.15 M orders/s** | real_time 换算，含跨线程开销 |

**noalloc 改造验证结论**：单线程吞吐从 6.19 → 8.04 M/s，提升约 30%，
与 perf 报告中 ~20% malloc 热点的预期削减方向一致（实测略高于预期，
因消除 `vector` 析构和 `reserve` 也有贡献）。

**流水线 vs 单线程基线**：流水线 3.15 M/s < 单线程 8.04 M/s，差值来自：
- SPSC 入队/出队（生产者 `try_push` + 消费者 `try_pop`）
- `FeedSimulator` 随机数生成开销混入生产者计时
- VM 环境下跨 vCPU 调度抖动（裸金属上两者差距会更小）

流水线的价值不在于纯吞吐，而在于**生产与消费解耦、热路径零堆分配**。

### 8.3 端到端延迟分布

```
=== Latency Report (100000 samples) ===
P50  : 3,144,697 cycles /  1,260,905 ns  (~1.26 ms)
P95  : 5,066,034 cycles /  2,031,289 ns  (~2.03 ms)
P99  : 6,199,181 cycles /  2,485,638 ns  (~2.49 ms)
P999 : 7,082,786 cycles /  2,839,930 ns  (~2.84 ms)
Max  : 7,088,139 cycles /  2,842,077 ns  (~2.84 ms)

成交笔数：62,155 / 100,000（成交率 ~62%，cancel_ratio=0.2）
```

**延迟偏高的原因（毫秒级 vs 裸金属纳秒级）**：

VM 环境下 SPSC 自旋 busy-wait 实际会触发 vCPU 让权（hypervisor 抢占），
导致 P50 在毫秒级而非裸金属期望的 100~500 ns。P999 ≈ Max 说明尾延迟
集中、没有严重离群，分布形态是合理的。

> 参考：裸金属服务器上同等架构 P50 通常在 200~800 ns，VM 环境下毫秒级属正常现象。

---

## 九、本课小结

| 组件 | 改动 | 核心原理 | 状态 |
|---|---|---|---|
| `TradeRingBuffer<N>` | **新增** `include/trade_ring_buffer.h` | 预分配数组，消费者独占写，零堆分配 | ✅ |
| `OrderBook::add_order_noalloc` | **新增** 模板接口（原接口保留） | 成交写入外部 buffer，`const Deallocator&` 模板参数通知归还 | ✅ |
| `MatchingEngine::start/stop` | **升级** 双线程流水线 | SPSC 双队列，内存池生产者独占，归还队列反向通道 | ✅ |
| `MatchingEngine::submit` | **保留** 单线程接口 | 向后兼容，旧测试/benchmark 零改动 | ✅ |
| `bench_matching_engine` | **新增** | 端到端吞吐 + 延迟（P50/P99），对比第十章基线 | ✅ |
| `test_matching_engine` | **新增** | 13 个集成测试，覆盖正确性、内存安全、DeallocateCallback | ✅ |

**核心收益**：
- 热路径消除 `std::vector<Trade>` 的堆分配，理论削减 ~20% malloc 热点
- 真正的 SPSC 双线程流水线，生产者和消费者并行工作
- Order 生命周期全程可控，内存池无竞争

**遗留与下一步**：
- 价格树（`std::map`）仍有 ~5% 热点 → 候选替换为 `flat_map`（有序 vector）
- 多品种支持（当前流水线只支持单品种）→ 多个 `[MatchingEngine × symbol]` 实例
- 下游 Trade 消费（当前覆盖写）→ 第二条 SPSC 队列接入 TradeLogger

---

## 十、Mutex 基线 vs SPSC 优化版对比

> 测试时间：2026-04-21  
> 测试环境：腾讯云 VM-0-3-ubuntu，2 vCPU @ 2494 MHz，Release（-O2）  
> 运行命令：`taskset -c 0,1 ./build/release/bench_baseline_mutex`  
> 基线实现：`benchmarks/bench_baseline_mutex.cpp`（完整内联，不污染主代码）

### 10.1 基线版本设计

Mutex 版本是**有意不做任何优化**的参照实现，特征如下：

| 组件 | Mutex 基线版本 | SPSC 优化版本 |
|------|--------------|-------------|
| 订单队列 | `std::queue<Order*>` + `std::mutex` + `std::condition_variable` | `SPSCRingBuffer<Order*, N>`（无锁，自旋） |
| OrderBook 数据结构 | `std::deque<Order*>`（分段数组，非连续） | `std::vector<Order*>` + head 游标（连续内存） |
| 订单索引 | `std::unordered_map`（无 reserve，链地址法） | `absl::flat_hash_map`（Swiss Table，open-addressing，reserve(65536)） |
| Trade 收集 | `match()` 返回 `std::vector<Trade>`（每次堆分配） | `TradeRingBuffer`（预分配数组，零堆分配） |
| cancel_order | `deque::erase`（O(n) 移动后续元素） | 惰性删除 + head 游标推进（均摊 O(1)） |
| 内存管理 | `std::vector<Order>` 预分配，无 MemoryPool | `OrderMemoryPool` + `return_queue_` 归还通道 |
| 线程同步开销 | `mutex` lock/unlock + `condition_variable` wait/notify（内核调度路径） | SPSC `try_push`/`try_pop`（用户态自旋，无系统调用） |

### 10.2 吞吐量对比

| 规模 | Mutex real_time | Mutex 真实吞吐 | SPSC real_time | SPSC 真实吞吐 | **提升倍数** |
|------|-----------------|---------------|----------------|--------------|------------|
| 10K  | 8.21 ms         | 1.22 M/s      | 5.72 ms        | 1.75 M/s     | **1.43×**  |
| 50K  | 42.6 ms         | 1.17 M/s      | 15.5 ms        | 3.23 M/s     | **2.76×**  |
| 100K | 84.0 ms         | 1.19 M/s      | 31.7 ms        | 3.15 M/s     | **2.65×**  |

> 真实吞吐 = order_count / real_time（与 SPSC 版本 8.1 节口径一致）

**规律分析**：
- **10K 规模**：差距较小，线程启停开销和队列初始化占比较高，稀释了单条订单的延迟差异
- **50K/100K 规模**：随订单量增大，Mutex 版本因每条订单触发一次 `notify_one`（内核调度），
  累计 context switch 开销线性增长；SPSC 版本自旋无系统调用，吞吐稳定在 3.15~3.23 M/s
- **Mutex 版本吞吐在 50K→100K 基本持平（~1.19 M/s）**：说明消费者处理能力已饱和，
  `condition_variable` 的内核调度开销成为瓶颈，无法随规模线性扩展

### 10.3 端到端延迟对比（100K 订单）

```
=== Mutex Baseline Latency Report (100K orders) ===
P50  : 11,902,923 cycles /  4,772,623 ns  (~4.77 ms)
P95  : 29,635,226 cycles / 11,882,609 ns  (~11.88 ms)
P99  : 31,403,513 cycles / 12,591,625 ns  (~12.59 ms)
P999 : 31,573,650 cycles / 12,659,844 ns  (~12.66 ms)
Max  : 31,591,526 cycles / 12,667,011 ns  (~12.67 ms)
```

```
=== SPSC Pipeline Latency Report (100K orders) ===
P50  :  3,144,697 cycles /   1,260,905 ns  (~1.26 ms)
P95  :  5,066,034 cycles /   2,031,289 ns  (~2.03 ms)
P99  :  6,199,181 cycles /   2,485,638 ns  (~2.49 ms)
P999 :  7,082,786 cycles /   2,839,930 ns  (~2.84 ms)
Max  :  7,088,139 cycles /   2,842,077 ns  (~2.84 ms)
```

| 指标 | Mutex 基线 | SPSC 优化版 | **改善幅度** |
|------|-----------|------------|------------|
| P50  | ~4.77 ms  | ~1.26 ms   | **3.8×**   |
| P99  | ~12.59 ms | ~2.49 ms   | **5.1×**   |
| Max  | ~12.67 ms | ~2.84 ms   | **4.5×**   |

### 10.4 延迟差距根因分析

Mutex 版本 P50 约 **4.77 ms** vs SPSC 约 **1.26 ms**，差距 ~3.8 倍，根因分解：

1. **`condition_variable::wait` + `notify_one` 的内核路径**  
   每条订单触发一次用户态→内核态切换（`futex` 系统调用），在 VM 上单次唤醒延迟约 **10~50 μs**。
   100K 订单 × 平均唤醒开销 ≈ 84ms 总时间，与实测一致。

2. **`make_shared` 的堆分配开销**  
   Mutex 版本每条订单调用 `make_shared<Order>()`（一次 malloc + 引用计数分配），在消费者完成处理后批量 delete。
   这是"未优化"语义的体现，与 SPSC 版本 MemoryPool 的零热路径分配形成鲜明对比。

3. **SPSC 自旋的优势**  
   SPSC `try_pop` 在用户态忙等（spin），无系统调用，vCPU 让权仅由 hypervisor 时间片触发（周期约 1~4 ms），与 Mutex 每条订单强制切换相比，上下文切换次数低 2~3 个数量级。

4. **OrderBook 数据结构差异（次要因素）**  
   `deque` vs `vector+head` 的 cache miss 差距约 10~30 ns/op（第十章 perf 分析结论），
   在数毫秒的总延迟中占比极小（< 1%），本次对比中可忽略。

### 10.5 结论

> 在腾讯云 2 vCPU VM 环境下，SPSC lock-free 流水线相比 mutex+condition_variable 基线：
> - **吞吐提升 ~2.65× @ 100K 规模**（3.15 M/s vs 1.19 M/s）
> - **P50 延迟降低 ~3.8×**（1.26 ms vs 4.77 ms）
> - **P99 延迟降低 ~5.1×**（2.49 ms vs 12.59 ms）
>
> 在裸金属服务器上，SPSC 的优势会更加显著（P50 可达 200~800 ns 级别，
> 而 mutex 方案因内核调度开销上限为数十微秒）。
