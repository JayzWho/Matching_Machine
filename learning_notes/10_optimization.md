# 第十课：针对性优化——让数据说话

> 对应阶段：Week 6 / Todo 3（优化实施部分）
> 关键词：cache locality、std::deque vs vector、惰性删除、unordered_map reserve、trades 预分配、before/after benchmark

---

## 引言

上一课用 perf + FlameGraph 找到了热点，这一课落地优化。

优化的核心原则：
> **"Make it correct first, then make it fast, and always measure."**
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

## 五、优化效果：Benchmark 数据

> 运行命令（绑核，Release 模式）：
> ```bash
> taskset -c 0 ./build/release/bench_order_book
> ```

### 5.1 优化后数据（v0.2-after-opt）

| Benchmark | 延迟 (ns/op) | 吞吐 (M ops/s) |
|-----------|-------------|----------------|
| BM_AddOrder_NoMatch | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_AddOrder_FullMatch | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/1 | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/5 | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/10 | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_AddOrder_SweepLevels/20 | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_CancelOrder | _(运行 benchmark 后填入)_ | _(填入)_ |
| BM_MixedWorkload | _(运行 benchmark 后填入)_ | _(填入)_ |

> **填写方法**：运行上述命令后，将输出中 `real_time` 列的数字填入。

### 5.2 与优化前的定性对比

基于对优化内容的分析，预期改善方向（实际数字以 benchmark 为准）：

| 场景 | 主要改善来源 | 预期改善幅度 |
|------|-------------|-------------|
| BM_AddOrder_FullMatch | trades.reserve(4) 消除堆分配 | 中等（取决于 malloc 开销） |
| BM_MixedWorkload | deque→vector cache locality | 中等（高频撮合场景） |
| BM_CancelOrder | 惰性删除避免内存移动 | 对深档位改善明显 |
| BM_AddOrder_NoMatch（长时间运行） | reserve(65536) 消除 rehash 尖峰 | P99 改善明显，P50 变化小 |

---

## 六、优化方法论总结

这四项优化体现了低延迟系统设计中反复出现的几个模式：

### 6.1 避免隐性堆分配（Avoid Hidden Heap Allocations）

热路径上每次 `new`/`malloc` 都有隐性开销：
- 全局锁（`ptmalloc` 在多线程下）
- 潜在的 `mmap`（当 malloc 向内核申请新内存时）
- 首次访问（page fault）

**解决方案**：预分配（`reserve`）、内存池（`MemoryPool`，Week 4-5 已实现）

### 6.2 保持 Cache 友好（Cache-Friendly Access Patterns）

现代 CPU 的 L1 cache miss 代价约 100 cycle（约 30~50ns），
L2 miss 约 300 cycle，L3 miss 约 1000 cycle。

**原则**：让频繁访问的数据紧密排列在内存中，利用空间局部性。

- ✓ `std::vector`（连续内存）
- ✓ `struct of arrays`（比 `array of structs` 对向量化更友好）
- ✗ `std::list` / `std::deque`（散乱内存，打断预取）
- ✗ 指针追踪链（linked list / tree 节点各自独立分配）

### 6.3 延迟清理（Lazy Cleanup）

不在热路径上做不必要的内存操作，改为在合适时机批量清理：
- 惰性删除（标记 nullptr，在 `match()` 中清理）
- `PriceLevel::compact()`（每隔 N 次操作整理一次）

### 6.4 先测量，后优化

这四项优化都是基于对代码的**静态分析**推断的——
使用 perf + FlameGraph 可以验证这些推断是否对应实际热点。
如果 perf 显示 `std::_Rb_tree::*`（std::map）占了大头，
那么下一步优化方向是用 `flat_map`（排序 vector）替代 `std::map`。

---

## 七、本课小结

| 优化 | 修改位置 | 核心原理 |
|------|----------|----------|
| deque → vector + head 游标 | `order_book.h` PriceLevel | 连续内存，cache 友好 |
| `unordered_map::reserve` | `order_book.cpp` 构造函数 | 消除 rehash 延迟尖峰 |
| `trades.reserve(4)` | `order_book.cpp` match() | 消除成交时堆分配 |
| 惰性删除 | `order_book.cpp` cancel_order() | 避免 deque::erase O(n) 内存移动 |

完整对比见 `docs/optimization_log.md`，优化前快照见 `docs/snapshots/`。

---

## 下一课预告

**第十一课**（Week 7-8）：实现完整的 `MatchingEngine`——
将 `FeedSimulator` → `SPSC Ring Buffer` → `OrderBook` → `TradeLogger`
串联成端到端流水线，加入多线程压力测试，验证整体吞吐量目标。
