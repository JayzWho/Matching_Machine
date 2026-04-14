# OrderBook 优化日志

本文档记录 Week 5-6 性能优化阶段对 `OrderBook` 所做的全部修改。
每条优化包含：**问题描述 → 修改内容 → 理论分析 → 基准数据**。

相关 git tag：
- `v0.2-before-opt`：优化前快照（另见 `docs/snapshots/order_book_v1_deque.{h,cpp}`）
- `v0.2-after-opt`：本次全部优化完成后的状态

---

## 优化 #1：`std::deque` → `std::vector` + 头部游标（PriceLevel）

### 问题

原始版本使用 `std::deque<Order*>` 作为每个价格档位（PriceLevel）的订单队列：

```cpp
// 优化前（order_book_v1_deque.h）
std::map<int64_t, std::deque<Order*>, std::greater<int64_t>> bid_levels_;
std::map<int64_t, std::deque<Order*>>                        ask_levels_;
```

`std::deque` 的内存模型是**分段数组**（chunk 链）：
- 内部维护一个指向各个 chunk 的指针数组（map）
- 顺序遍历时需要在 chunk 边界处跳转指针，造成额外的 **cache miss**
- 每次创建/销毁 deque 都有隐含的堆分配用于管理 chunk map
- 无法通过 `reserve()` 预分配容量

在撮合热路径上，`match()` 函数需要频繁调用 `front()` + `pop_front()`，
每次 `pop_front()` 在减少首个元素后可能触发 chunk 释放，增加内存管理负担。

### 修改

引入 `PriceLevel` 嵌套结构（见 `include/order_book.h`）：

```cpp
// 优化后（order_book.h）
struct PriceLevel {
    std::vector<Order*> orders;
    size_t head = 0;    // 有效元素起始索引，替代 pop_front 的实际移动

    void push(Order* o) { orders.push_back(o); }

    Order* front() const {
        return (head < orders.size()) ? orders[head] : nullptr;
    }

    void pop_front() {
        if (head < orders.size()) {
            ++head;
            // 超过一半是空洞时 compact，防止无限增长，均摊 O(1)
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

std::map<int64_t, PriceLevel, std::greater<int64_t>> bid_levels_;
std::map<int64_t, PriceLevel>                        ask_levels_;
```

### 理论分析

| 特性 | `std::deque` | `PriceLevel (vector + head)` |
|------|-------------|------------------------------|
| 内存连续性 | 分段（chunk 链） | 连续 |
| `front()` | O(1)，可能跨 chunk | O(1)，直接索引 |
| `pop_front()` | O(1)，可能释放 chunk | O(1)，仅递增游标 |
| CPU 预取友好性 | 差（chunk 跳转打断预取） | 好（顺序访问） |
| `reserve()` 支持 | ✗ | ✓（可预分配）|
| 撤单惰性删除 | ✗（需 erase 移动元素） | ✓（设 nullptr，head 游标跳过）|

**预期效果**：`match()` 热路径的 `front()` / `pop_front()` 调用对 cache 更友好，
在高频撮合场景（BM_AddOrder_FullMatch、BM_MixedWorkload）延迟降低。

---

## 优化 #2：`unordered_map::reserve()` 预分配

### 问题

```cpp
// 优化前：构造函数无 reserve
OrderBook::OrderBook(std::string_view symbol)
    : symbol_(symbol)
{}
```

`std::unordered_map` 在负载因子（load factor）超过阈值时会触发 **rehash**：
- rehash 是 O(n) 操作：重新分配所有桶，迭代所有元素重新插入
- 默认初始桶数仅为 1~16，高频 `add_order` 会频繁触发 rehash
- rehash 期间的内存碎片会影响后续访问的 cache 命中率

### 修改

```cpp
// 优化后：构造函数预分配
OrderBook::OrderBook(std::string_view symbol)
    : symbol_(symbol)
{
    // 预分配 64K 容量，足够容纳典型盘口深度，避免高频插入时 rehash
    // rehash 是 O(n) 且重分配全部桶，在热路径上开销不可控
    order_index_.reserve(65536);
}
```

### 理论分析

- `reserve(65536)` 在构造时一次性分配约 512KB（64K × 8 bytes）
- 在订单数 < 65536 的场景内，整个生命周期**零 rehash**
- 所有桶在同一块连续内存上，cache 命中率更高

---

## 优化 #3：`match()` 中 `trades` 向量预分配

### 问题

```cpp
// 优化前：无 reserve，首次 emplace_back 触发堆分配
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;
    // ...
}
```

每次 `add_order` → `match()` 都会：
1. 构造空 `std::vector<Trade>`
2. 第一次 `emplace_back` 时触发堆分配（通常分配 1~2 个元素的容量）
3. 连续成交时按 2 的幂次扩容（1→2→4→8...），每次扩容都需要复制已有元素

对于 BM_AddOrder_FullMatch（每次正好成交 1 笔），每次调用都有一次堆分配。

### 修改

```cpp
// 优化后：预分配 4 个元素容量
std::vector<Trade> OrderBook::match(Order* incoming) {
    std::vector<Trade> trades;
    trades.reserve(4);  // 绝大多数成交只产生 1-4 笔，避免第一次扩容
    // ...
}
```

### 理论分析

- `reserve(4)` 的成本：一次性分配 `4 × sizeof(Trade)` 字节（约 128 bytes）
- 覆盖 95%+ 的真实场景（普通限价单很少产生超过 4 笔同时成交）
- 对于 BM_AddOrder_SweepLevels（多档扫穿）：扫穿 N 档时产生 N 笔成交，
  `reserve(4)` 后扫穿 1-4 档零额外分配，5 档以上仅需 1 次扩容

---

## 优化 #4：撤单惰性删除（lazy deletion）

### 问题

```cpp
// 优化前（order_book_v1_deque.cpp）：直接 erase，O(n) 移动后续元素
for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
    if ((*dit)->order_id == order_id) {
        dq.erase(dit);  // deque::erase = O(n)，移动 dit 之后所有元素
        break;
    }
}
```

`deque::erase(it)` 需要将 `it` 之后的所有元素向前移动一位，对于深档位时延迟较高。

### 修改

```cpp
// 优化后：设为 nullptr 空洞，match() 中跳过，不移动任何元素
for (size_t i = level.head; i < vec.size(); ++i) {
    if (vec[i] && vec[i]->order_id == order_id) {
        vec[i] = nullptr;  // 标记为空洞（惰性删除），O(1)
        break;
    }
}
```

`match()` 中增加跳过 nullptr 的逻辑：

```cpp
// 跳过撤单留下的空洞（nullptr）
while (!ask_level.empty() && ask_level.front() == nullptr) {
    ask_level.pop_front();
}
```

### 理论分析

- `cancel_order` 时间复杂度：线性扫描 O(n) 不变，但无内存移动，实际延迟更低
- 空洞在 `match()` 中被 O(1) 清理（pop_front + head++）
- 适合**撤单是低频操作**的场景（量化交易中撤单通常 << 挂单）

---

## 面试讲解要点

1. **为什么用 vector + head 游标而非直接用 deque？**
   > deque 的分段内存布局在顺序遍历时会触发 cache miss（chunk 边界跳转），
   > vector 的连续内存配合 CPU 预取流水线效率更高。
   > head 游标使 pop_front 降为 O(1) 且无内存移动，compact 逻辑确保不无限增长。

2. **reserve(65536) 会不会浪费内存？**
   > 约 512KB，对于服务器进程可忽略不计。
   > 相比之下，每次 rehash 的 CPU 停顿（O(n) 重分配）在高频场景下代价更高。

3. **惰性删除会不会导致 match() 中大量无效遍历？**
   > 不会。撤单在真实交易系统中是相对低频的操作（远少于 add/match）。
   > match() 中用 `while (front() == nullptr) pop_front()` 一次性清理空洞，
   > 且 compact 逻辑会定期压缩 vector，防止空洞积累。

---

## 优化 #5：移除 `std::mutex`

### 问题

`OrderBook` v2 在所有公开方法（`add_order`、`cancel_order`、`best_bid`、
`best_ask`、`order_count`）上都使用 `std::lock_guard<std::mutex>`：

```cpp
// 优化前
std::vector<Trade> add_order(Order* order) {
    std::lock_guard<std::mutex> lock(mtx_);
    // ...
}
```

实际上所有调用方（`main.cpp`、benchmarks、tests）均为**纯单线程**，
mutex 从未有过竞争，但每次调用仍需付出约 **20~40 ns** 的 `futex` 快路径开销。

### 修改

```cpp
// order_book.h：删除
mutable std::mutex mtx_;

// order_book.cpp：删除全部 lock_guard
// add_order、cancel_order、best_bid、best_ask、order_count 中的 lock_guard 全部移除
```

类注释从"mutex 版本（v2）"更新为"单线程无锁版本（v3）"。

### 理论分析

| | 优化前 | 优化后 |
|---|---|---|
| 无竞争 lock/unlock | ~20~40 ns/次 | 0 |
| 多方法调用 (add+match+cancel) | 可能多次加锁 | 无 |
| SPSC 安全性 | 由 mutex 保证 | 由 SPSC 架构保证（单消费者独占） |

预期效果：所有 benchmark 均匀提升约 10~15%（与每次调用的绝对延迟相关）。

---

## 优化 #6：`std::unordered_map` → `absl::flat_hash_map`

### 问题

perf 采样（P0-A BM_MixedWorkload）显示，`order_index_` 相关函数累计占 **32%** CPU 时间：
- `operator[]`（插入/查找）：25.57%
- `_M_erase`（两处撤单/成交路径）：6.73%

根源在于 `std::unordered_map` 的**链地址法（chaining）**实现：
- 每次插入 `new` 一个链表节点，节点散落在堆上
- `find` 需要遍历链表，随机内存访问，cache miss 频繁
- `reserve` 只消除 rehash，无法改变链地址法本身的访问模式

### 修改

```cpp
// order_book.h：替换头文件和成员类型
// 前：
#include <unordered_map>
std::unordered_map<uint64_t, Order*> order_index_;

// 后：
#include "absl/container/flat_hash_map.h"
absl::flat_hash_map<uint64_t, Order*> order_index_;
```

`order_book.cpp` 和 `matching_engine.cpp` 无需任何改动（接口完全兼容）。

**CMakeLists.txt** 新增依赖：
```cmake
FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        20240722.0
)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(... abseil-cpp)

target_link_libraries(matching_engine_lib PUBLIC absl::flat_hash_map)
```

### 理论分析

`absl::flat_hash_map` 使用 **Swiss Table** 算法（open-addressing + SSE2 SIMD）：

| 特性 | `std::unordered_map` | `absl::flat_hash_map` |
|---|---|---|
| 碰撞处理 | 链地址法（heap 节点） | 开放寻址（连续内存，无堆分配） |
| `find` 探测 | 链表遍历（随机访问） | SSE2 批量 16 字节比较 |
| 插入堆分配 | 每次 1 次 `new` | 零（槽位预分配） |
| cache 局部性 | 差 | 优（桶连续排列） |

预期效果：`operator[]` 热点从 25.57% 降至 ~10%，整体 MixedWorkload 延迟下降 15~25%。

---

## Benchmark 数据对比（v0.3-after-opt）

> 运行环境：Linux，单核绑定（`taskset -c 0`），Release 编译（`-O2 -DNDEBUG`）
>
> 运行命令：`taskset -c 0 ./build/release/bench_order_book`

| Benchmark | 延迟 (ns) | 吞吐 |
|-----------|-----------|------|
| BM_AddOrder_NoMatch | _(填入实测数据)_ | _(填入)_ |
| BM_AddOrder_FullMatch | _(填入实测数据)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/1 | _(填入实测数据)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/5 | _(填入实测数据)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/10 | _(填入实测数据)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/20 | _(填入实测数据)_ | _(填入)_ |
| BM_CancelOrder | _(填入实测数据)_ | _(填入)_ |
| BM_MixedWorkload | _(填入实测数据)_ | _(填入)_ |

> **TODO**：运行 `taskset -c 0 ./build/release/bench_order_book` 后将数据填入上表。
