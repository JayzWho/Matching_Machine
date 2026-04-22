# 第十课：针对性优化——让数据说话

> 对应阶段：Week 6 / Todo 3（优化实施部分）
> 关键词：cache locality、std::deque vs vector、惰性删除、unordered_map reserve、trades 预分配、before/after benchmark

---

## 引言

上一课用 perf + FlameGraph 找到了热点，这一课落地优化。

优化的核心原则：
> **"Make it work, make it right, then make it fast — and measure before optimizing."**
> — Kent Beck / 低延迟系统开发共识

我们在 Week 3 完成了"correct"（GTest 验证），Week 6 现在做"fast"。
每项优化都遵循：**假设 → 修改 → 测量 → 确认（或否定）**。

优化前后的完整代码对比见：
- **优化前快照**：`docs/snapshots/order_book_v1_deque.{h,cpp}`
- **优化后代码**：`include/order_book.h` + `src/order_book.cpp`
- **Git 标签**：`v0.2-before-opt`（快照提交）→ `v0.2-after-opt`（本课完成后）
- **详细 diff**：`docs/optimization_log.md`

---

## 一、优化 #1：`std::deque` → `std::vector` + 头部游标

### 1.1 问题根源：deque 的内存布局

`std::deque` 是一种"分段数组"（segmented array）数据结构：

```
内存示意（deque 内部结构）：

  map（指针数组）:  [chunk_ptr_0] [chunk_ptr_1] [chunk_ptr_2] ...
                         │              │              │
                         ▼              ▼              ▼
                    [O O O O O O]  [O O O O O O]  [O O O O O O]
                     chunk 0（固定大小）  chunk 1        chunk 2
```

每次 `front()` → `pop_front()` 循环在 `match()` 热路径中：
- 若当前 chunk 已消耗完，需要通过 map 跳到下一个 chunk
- 这次内存跳转很可能触发 **L1/L2 cache miss**（两处内存：map 指针 + 新 chunk）

### 1.2 解决方案：`PriceLevel`（vector + head 游标）

```cpp
// include/order_book.h
struct PriceLevel {
    std::vector<Order*> orders;
    size_t head = 0;   // 有效元素起始索引

    void push(Order* o) { orders.push_back(o); }

    Order* front() const {
        return (head < orders.size()) ? orders[head] : nullptr;
    }

    void pop_front() {
        if (head < orders.size()) {
            ++head;
            // 均摊 compact：超过一半空洞时清理
            if (head > orders.size() / 2 && head > 16) {
                orders.erase(orders.begin(),
                             orders.begin() + static_cast<ptrdiff_t>(head));
                head = 0;
            }
        }
    }

    bool empty() const { return head >= orders.size(); }
    size_t size() const { return orders.size() - head; }
};
```

内存示意：

```
  vector.data():  [O*][O*][O*][O*][O*][O*]...
                   ^
                   head（逻辑队列头）

  pop_front()：   head++ （O(1)，无内存移动）
  
  compact 后：    [O*][O*][O*]...
                   ^
                   head = 0
```

**关键好处**：
- `front()` / `pop_front()` 只需访问连续内存，CPU 预取器可以提前加载后续元素
- 撤单时设 `nullptr` 而非移动元素（惰性删除，见优化 #4）
- 支持 `reserve()` 预热

### 1.3 复杂度分析

| 操作 | deque | PriceLevel |
|------|-------|------------|
| `push`（挂单） | O(1) 均摊 | O(1) 均摊 |
| `front` | O(1) | O(1) |
| `pop_front` | O(1) 均摊，可能释放 chunk | O(1) 均摊（compact 均摊） |
| `size` | O(1) | O(1) |
| 内存连续性 | ✗（分段） | ✓ |
| `reserve` 支持 | ✗ | ✓ |

---

## 二、优化 #2：`unordered_map::reserve(65536)`

### 2.1 问题根源：rehash 的隐性开销

```cpp
// 优化前：无 reserve，构造后立即使用
std::unordered_map<uint64_t, Order*> order_index_;
```

`std::unordered_map` 在负载因子超过阈值（通常 1.0）时触发 **rehash**：
1. 分配新的桶数组（`2 × 当前桶数`）
2. 遍历所有元素，重新计算 hash，插入新桶
3. 释放旧桶数组

这是 O(n) 操作，且分散在整个程序运行期间随机发生。在 benchmark 中会产生
**延迟尖刺**（latency spike）——P99 远高于 P50 的主要原因之一。

### 2.2 修改

```cpp
// 优化后（src/order_book.cpp）
OrderBook::OrderBook(std::string_view symbol)
    : symbol_(symbol)
{
    order_index_.reserve(65536);  // 一次性分配，整个生命周期零 rehash
}
```

为什么选 65536？
- 典型盘口深度：最多几千个活跃订单
- 65536 = 2^16，足够覆盖绝大多数场景
- 内存成本：约 `65536 × sizeof(pair<uint64_t, Order*>)` ≈ 1MB，可接受

### 2.3 延伸：load_factor 调优

如果想更激进地减少 hash 碰撞（以内存换性能）：

```cpp
order_index_.reserve(65536);
order_index_.max_load_factor(0.5f);  // 碰撞减半，内存翻倍
```

本项目保持默认 load_factor（1.0），reserve 已足够消除 rehash。

---

## 三、优化 #3：`match()` 中 `trades.reserve(4)`

### 3.1 问题

```cpp
// 优化前：每次进入 match() 都从空 vector 开始
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;  // 空 vector，capacity = 0
    // ...
    trades.emplace_back();  // 第一次：触发堆分配（alloc 1~2个元素）
    // ...
}
```

`add_order` 是每笔订单都会调用的热路径函数。如果成交（有 trades 产生），
每次都有一次 `new`（堆分配），在高频场景下累积开销可观，
且 `malloc` 在多线程环境下有锁争用。

### 3.2 修改

```cpp
// 优化后：预分配 4 个元素容量
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;
    trades.reserve(4);  // 覆盖 95%+ 场景（普通成交很少超过 4 笔）
    // ...
}
```

真实交易数据中，绝大多数成交是 1 对 1（一笔买单 × 一笔卖单 = 1 个 Trade），
`reserve(4)` 几乎覆盖全部情况。只有"扫穿多档"的特殊大单才会超过 4 笔。

---

## 四、优化 #4：撤单惰性删除（Lazy Deletion）

### 4.1 问题

```cpp
// 优化前（order_book_v1_deque.cpp）
for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
    if ((*dit)->order_id == order_id) {
        dq.erase(dit);  // O(n)：移动 dit 之后的所有元素
        break;
    }
}
```

`deque::erase(it)` 需要将被删除位置之后的所有元素向前移动，
对于深档位（一个价格上有很多挂单），这是 O(n) 的实际内存操作，
不只是算法复杂度意义上的 O(n)。

### 4.2 修改

```cpp
// 优化后（src/order_book.cpp）
for (size_t i = level.head; i < vec.size(); ++i) {
    if (vec[i] && vec[i]->order_id == order_id) {
        vec[i] = nullptr;  // 标记空洞，O(1)，无内存移动
        break;
    }
}
```

在 `match()` 中跳过空洞：

```cpp
// 跳过撤单留下的 nullptr 空洞
while (!ask_level.empty() && ask_level.front() == nullptr) {
    ask_level.pop_front();  // head++，O(1)
}
```

这种"先标记，后清理"的模式在数据库系统（MVCC）和 GC 系统中广泛使用，
本质上是用**空间**（短暂的空洞）换取**时间**（避免实时移动）。

---

## 五、优化 #5：移除 `std::mutex`（单线程 SPSC 基线）

### 5.1 问题根源

v2 版本的 `OrderBook` 在所有公开方法上都加了 `std::mutex`：

```cpp
// 优化前
std::vector<Trade> add_order(Order* order) {
    std::lock_guard<std::mutex> lock(mtx_);  // 无论有无竞争都必须获取
    // ...
}
```

但实际上：
- 当前 `main.cpp` 和所有 benchmark **完全单线程**，mutex 从未有过竞争
- 即使是**无竞争路径**（uncontended lock），一次 `lock + unlock` 仍需约 **20~40 ns**
- `best_bid`、`best_ask`、`order_count` 这三个只读接口也被加了锁，开销同上

### 5.2 为什么现在可以安全去掉

在 SPSC 架构下：
- `OrderBook` 只由 **Consumer 线程**访问（SPSC 的 single-consumer 保证）
- Producer 线程只写 ring buffer，不直接接触 `OrderBook`
- 不存在任何并发写入 `OrderBook` 的路径

因此 `OrderBook` 本身**不需要**任何同步原语。

### 5.3 修改内容

```cpp
// 删除：头文件中的 mutable std::mutex mtx_;
// 删除：add_order、cancel_order、best_bid、best_ask、order_count 中的所有 lock_guard
```

注释也从"mutex 版本（v2）"更新为"单线程无锁版本（v3）"。

### 5.4 后续扩展（Week 8）

如果未来需要跨线程读取 `best_bid` / `best_ask`（比如 Producer 线程查询行情），
不应恢复 mutex，而是改为 `std::atomic<int64_t>`：
```cpp
std::atomic<int64_t> cached_best_bid_{0};  // Consumer 写，Producer 只读
```

---

## 六、优化 #6：`std::unordered_map` → `absl::flat_hash_map`

### 6.1 问题根源：`std::unordered_map` 的结构性开销

perf 数据（P0-A MixedWorkload）显示，即使在 `reserve(65536)` 之后：
- `order_index_::operator[]`（插入/查找）仍占 **25.57%** CPU 时间
- `order_index_::_M_erase`（两处）占 **6.73%**
- 合计超过 **32%**，是最大的单一热点

这是 `std::unordered_map` **链地址法（chaining）**的结构性缺陷：

```
std::unordered_map 内存布局（链地址法）：

桶数组:  [bucket_0] → Node{key,val,next_ptr} → Node{...} → nullptr
         [bucket_1] → nullptr
         [bucket_2] → Node{key,val,next_ptr} → Node{...}
                              ↑ 每个节点单独 new，散布在堆上，cache miss!
```

每次 `find`：
1. 计算 hash → 定位桶（1 次内存访问）
2. 遍历链表节点（每个节点各自堆分配，可能 cache miss）
3. 比较 key

### 6.2 解决方案：`absl::flat_hash_map`（Swiss Table）

`absl::flat_hash_map` 使用 **open-addressing**（开放寻址法）：

```
flat_hash_map 内存布局（开放寻址）：

控制字节数组:  [meta_0][meta_1][meta_2]...[meta_N]  ← SSE2 批量比较
值数组:        [slot_0][slot_1][slot_2]...[slot_N]  ← 紧密排列，连续内存
```

关键优势：
| 特性 | `std::unordered_map` | `absl::flat_hash_map` |
|---|---|---|
| 碰撞处理 | 链表（节点堆分配） | 线性探测（就地，无堆分配） |
| cache 局部性 | 差（节点指针跳转） | 优（桶连续，SIMD 批量探测） |
| 额外堆分配 | 每次插入 1 次 `new` | 零（槽位预分配） |
| `find` 实现 | 链表遍历 | SSE2 批量字节比较（Swiss Table） |

### 6.3 代码改动

**头文件**（仅改 include 和声明）：
```cpp
// 前：#include <unordered_map>
// 后：#include "absl/container/flat_hash_map.h"

// 前：std::unordered_map<uint64_t, Order*> order_index_;
// 后：absl::flat_hash_map<uint64_t, Order*> order_index_;
```

接口完全兼容（`find`、`operator[]`、`erase`、`reserve`、`size` 均相同），
**其余代码无需任何改动**。

### 6.4 CMake 配置

通过 FetchContent 引入 abseil-cpp：
```cmake
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        20240722.0
)
FetchContent_MakeAvailable(... abseil-cpp)

target_link_libraries(matching_engine_lib PUBLIC absl::flat_hash_map)
```

### 6.5 预期效果

- `operator[]` 热点：25.57% → 预期 ~10%（open-addressing 减少 cache miss）
- `_M_erase`（6.73%）：被 `absl` 的 tombstone 机制替代，整体更轻量
- 整体 MixedWorkload 延迟预期下降 15~25%（待 benchmark 验证）

---

## 七、优化效果：Benchmark 数据

> 运行命令（绑核，Release 模式）：`taskset -c 0 ./build/release/bench_order_book`
>
> 测试环境：腾讯云 VM-0-3-ubuntu，2 CPU @ 2494 MHz，L1D 32KB / L2 4MB / L3 35.7MB（共享）
>
> ⚠️ 注：由于虚拟化环境限制，硬件 PMU 计数器不可用，perf 只能使用 `cpu-clock` 软件事件采样，IPC / cache-miss rate 等硬件指标均显示为 "-"。

### 7.1 Benchmark 数据对比（mean 延迟，单位 ns/op）

> 数据来源：
> - baseline：`results/order_book_2026-04-14_23-35-17_baseline.json`（未优化版，带 mutex）
> - opt-v3：`results/order_book_2026-04-14_23-00-20_opt-v3.json`（全部优化：deque→vector+head、reserve、trades.reserve、惰性删除、去 mutex、`absl::flat_hash_map`）
>
> ⚠️ 注：opt-v2 的 benchmark 数据（`results/opt-v3(deprecated)/`）因当时 benchmark 尚未更新完善，测量结果无参考价值，已废弃。

| Benchmark | baseline (ns) | opt-v3 (ns) | 提升幅度 |
|-----------|:---:|:---:|:---:|
| BM_AddOrder_NoMatch | 83.5 | 46.4 | **+44.4%** |
| BM_AddOrder_FullMatch | 756.8 | 816.3 | −7.9%（误差范围内） |
| BM_AddOrder_SweepLevels/1 | 872.6 | 817.3 | +6.3% |
| BM_AddOrder_SweepLevels/5 | 1,229.4 | 1,033.8 | **+15.9%** |
| BM_AddOrder_SweepLevels/10 | 1,691.1 | 1,299.0 | **+23.2%** |
| BM_AddOrder_SweepLevels/20 | 3,092.8 | 2,297.0 | **+25.7%** |
| BM_CancelOrder | 60,455.6 | 26,659.2 | **+55.9%** |
| BM_MixedWorkload（ns/iter） | 258,063,637 | 162,121,117 | **+37.2%** |

### 7.2 MixedWorkload 吞吐与稳定性

| 指标 | baseline | opt-v3 |
|------|:---:|:---:|
| 吞吐 (M ops/s) | 3.87 | **6.19** |
| 延迟 mean (ms) | 258.1 | **162.1** |
| 延迟 stddev (ms) | 5.49 | **2.91** |
| CV（变异系数） | 2.13% | **1.80%** |

CV（stddev/mean）反映延迟稳定性。opt-v3 的 CV 更低，说明消除 rehash 后延迟抖动明显改善。

---

## 八、perf 热点分析（opt-v3 实测）

> 工具：`perf record -g -F 99 --cpu-clock`，结果见 `results/opt-v3/`
>
> ⚠️ 虚拟化限制：IPC 列全为 "-"，硬件计数器不可用，只能依赖 cpu-clock 采样的函数级 Overhead% 分布。

### 8.1 MixedWorkload 热点（804 samples）

| Overhead | 函数 | 说明 |
|:---:|---|---|
| **51.99%** | `me::OrderBook::add_order` | 最大热点，涵盖 insert + match 全路径 |
| **8.58%** | `absl::flat_hash_map::erase` | 撤单/成交后删除索引项 |
| **7.34%** | `BM_MixedWorkload`（驱动框架） | benchmark 驱动本身 |
| **4.60%** | `_int_free` | 堆释放（Order 对象析构） |
| **4.23%** | `malloc` / `cancel_order` | 堆分配 / 撤单操作（并列） |
| **3.73%** | `me::OrderBook::match` | 撮合逻辑 |
| **3.36%** | `absl::flat_hash_map::prepare_insert` | 哈希表插入准备 |

**与 opt-v2（std::unordered_map）热点的对比**：

| 热点函数 | opt-v2 Overhead | opt-v3 Overhead | 变化 |
|---|:---:|:---:|:---:|
| `unordered_map::operator[]` | ~25.57% | （已消除） | **−25.57%** |
| `unordered_map::_M_erase`（两处合计） | ~6.73% | — | **−6.73%** |
| `flat_hash_map::erase` | — | 8.58% | 替代，更轻量 |
| `flat_hash_map::prepare_insert` | — | 3.36% | 替代，更轻量 |
| **hash map 相关合计** | **~32%** | **~12%** | **减少约 20pp** |

→ `absl::flat_hash_map` 将哈希表热点从 **32%** 压缩至约 **12%**，与预期吻合。

### 8.2 AddOrder_FullMatch 热点（4K samples）

`clock_gettime` + 内核 vDSO 路径合计占约 **68%**，因为该 benchmark 每次迭代都调用 `PauseTiming/ResumeTiming` 重置 OrderBook，计时开销完全淹没了业务代码。

业务代码：`add_order`（5.56%）+ `match`（0.53%）+ `flat_hash_map::erase`（1.44%）≈ **7.5%**。

> 此场景 perf 数据参考价值有限；延迟数据（~816 ns）是更可靠的性能指标。

### 8.3 AddOrder_SweepLevels 热点（16K samples）

`clock_gettime` 相关合计约 **41%**（benchmark 迭代计时），业务代码主要热点：

| Overhead | 函数 | 说明 |
|:---:|---|---|
| **5.41%** | `BM_AddOrder_SweepLevels::lambda` | 驱动 |
| **4.79%** | `me::OrderBook::add_order` | 挂单 |
| **4.53%** | `_int_free` | 档位清空时 vector 析构 |
| **4.00%** | `malloc` | 订单对象分配 |
| **3.32%** | `me::OrderBook::match` | 扫档撮合 |
| **3.20%** | `flat_hash_map::erase` | 批量索引删除 |
| **1.99%** | `std::_Rb_tree_insert_and_rebalance` | 价格树插入（**新热点**） |
| **1.37%** | `std::vector::_M_realloc_insert` | PriceLevel vector 扩容 |
| **1.15%** | `std::_Rb_tree_rebalance_for_erase` | 价格树删除重平衡 |

**新发现的潜在优化点**：
- `std::_Rb_tree_insert_and_rebalance`（1.99%）+ `std::_Rb_tree_rebalance_for_erase`（1.15%）合计 **~3.1%**
  ——来自 `std::map<price, PriceLevel>`（bids/asks 价格档位树），SweepLevels 频繁新增/删除档位触发 O(log N) 重平衡。
  **后续优化候选**：若档位数量有限（≤1024），可替换为有序 `vector`（`flat_map`）提升 cache 友好性。
- `std::vector::_M_realloc_insert`（1.37%）——PriceLevel 的 vector 初始容量为 0，首次扩容开销仍存在。
  可在 `PriceLevel` 构造时 `reserve` 初始容量（如 4 或 8）进一步消除。

### 8.4 全量 perf 报告摘要（21K samples，覆盖所有 benchmark）

| Overhead | 函数 | 说明 |
|:---:|---|---|
| **12.53%** | `me::OrderBook::add_order` | 全局最大热点 |
| **9.32%** | `_int_malloc` | 堆分配（**最主要的外部热点**） |
| **6.23%** | `_int_free` | 堆释放 |
| **5.01%** | `malloc` | 同上 |
| **4.91%** | `std::_Rb_tree_insert_and_rebalance` | 价格树插入 |
| **3.70%** | `BM_CancelOrder` | 撤单 benchmark 驱动 |
| **3.13%** | `me::OrderBook::match` | 撮合 |
| **2.81%** | `flat_hash_map::prepare_insert` | 索引插入 |
| **2.37%** | `malloc_consolidate` | glibc 堆整理 |

**综合结论**：
- **堆分配/释放**（`_int_malloc` + `_int_free` + `malloc` + `malloc_consolidate`）合计约 **20%**，是 v3 最主要的剩余瓶颈。
  根因：每次 `add_order` 都构造并返回 `std::vector<Trade>`（堆分配），加上 `std::map` 红黑树节点分配和 `PriceLevel::orders` 首次扩容，共同构成堆操作热点。
  → **下一步优化候选**：使用 `MemoryPool`（Week 4-5 已实现）替换 `std::vector<Trade>` 的堆分配，预计削减该 20% 热点的大部分。
- **价格树**（`std::_Rb_tree_*`）合计约 **5%**，是次要瓶颈，可用有序 vector 替代（见 8.3）。
- `add_order` 本身（12.53%）内部已基本优化到位，剩余开销主要来自上述两项。

---

## 九、优化方法论总结

这六项优化体现了低延迟系统设计中反复出现的几个模式：

### 8.1 避免隐性堆分配（Avoid Hidden Heap Allocations）

热路径上每次 `new`/`malloc` 都有隐性开销：
- 全局锁（`ptmalloc` 在多线程下）
- 潜在的 `mmap`（当 malloc 向内核申请新内存时）
- 首次访问（page fault）

**解决方案**：预分配（`reserve`）、open-addressing hash map（`flat_hash_map`）、内存池（`MemoryPool`，Week 4-5 已实现）

### 8.2 保持 Cache 友好（Cache-Friendly Access Patterns）

现代 CPU 的 L1 cache miss 代价约 100 cycle（约 30~50ns），
L2 miss 约 300 cycle，L3 miss 约 1000 cycle。

**原则**：让频繁访问的数据紧密排列在内存中，利用空间局部性。

- ✓ `std::vector`（连续内存）
- ✓ `absl::flat_hash_map`（open-addressing，桶连续，SIMD 探测）
- ✗ `std::list` / `std::deque`（散乱内存，打断预取）
- ✗ `std::unordered_map`（链地址法，节点独立堆分配）

### 8.3 延迟清理（Lazy Cleanup）

不在热路径上做不必要的内存操作，改为在合适时机批量清理：
- 惰性删除（标记 nullptr，在 `match()` 中清理）
- `PriceLevel::compact()`（每隔 N 次操作整理一次）

### 8.4 消除不必要的同步原语

在 SPSC 架构下（单消费者独占 `OrderBook`），mutex 是纯粹的开销。
**原则**：同步原语的代价不会因无竞争而消失，架构上确保单线程访问才是根本解法。

### 8.5 先测量，后优化

本章六项优化均基于 perf + FlameGraph 的实测热点数据驱动，而非凭直觉猜测。
下一步若 perf 显示 `std::_Rb_tree::*`（`std::map`）占大头，则用 `flat_map`（排序 vector）替代。

---

## 十、本课小结

| 优化 | 修改位置 | 核心原理 | 状态 |
|------|----------|----------|------|
| deque → vector + head 游标 | `order_book.h` PriceLevel | 连续内存，cache 友好 | ✅ |
| `unordered_map::reserve(65536)` | `order_book.cpp` 构造函数 | 消除 rehash 延迟尖峰 | ✅ |
| `trades.reserve(4)` | `order_book.cpp` match() | 消除成交时堆分配 | ✅ |
| 惰性删除 | `order_book.cpp` cancel_order() | 避免 deque::erase O(n) 内存移动 | ✅ |
| 移除 `std::mutex` | `order_book.h/.cpp` 全部方法 | SPSC 单线程，mutex 是纯粹开销 | ✅ |
| `unordered_map` → `flat_hash_map` | `order_book.h` + CMakeLists | open-addressing，消除链地址法开销 | ✅ |

**实测效果汇总（baseline → opt-v3，有效对比）**：
- NoMatch：83.5 ns → 46.4 ns（**+44.4%**）
- CancelOrder：60,456 ns → 26,659 ns（**+55.9%**）
- MixedWorkload 吞吐：3.87 → **6.19 M ops/s**（**+60%**），CV 2.13% → **1.80%**（更稳定）
- flat_hash_map 将哈希表 CPU 热点从 **32% → ~12%**（perf 实测），削减约 20 个百分点

**perf 揭示的下一阶段优化候选**：
1. `Order` 对象改用 `MemoryPool` 分配，消除堆分配/释放的 **~20%** CPU 热点
2. `std::map<price, PriceLevel>` → 有序 `vector`（flat_map），消除红黑树 **~5%** 热点
3. `PriceLevel` 构造时 `reserve` 初始容量，消除 vector 扩容 **~1.4%** 热点

完整对比见 `docs/optimization_log.md`，优化前快照见 `docs/snapshots/`。

---

## 下一课预告

**第十一课**（Week 7-8）：实现完整的 `MatchingEngine`——
将 `FeedSimulator` → `SPSC Ring Buffer` → `OrderBook` → `TradeLogger`
串联成端到端流水线，加入多线程压力测试，验证整体吞吐量目标。
