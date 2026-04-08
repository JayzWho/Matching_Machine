# 第三课：OrderBook 实现——数据结构、撮合逻辑与 Bug 调试

> 对应阶段：Week 1-3 / Todo 1
> 关键词：std::map、std::deque、价格-时间优先、mutex、RAII、order_index

---

## 引言

上一课我们定义好了 `Order` 结构体，知道了一笔订单长什么样。这一课我们把订单真正"放进"订单簿，并实现撮合逻辑。

`OrderBook` 是整个引擎的心脏：它维护买卖两侧的所有挂单，每来一笔新订单就判断能不能和已有挂单成交，成交就生成 Trade，不成交就留在簿里等待。这一版我们用 `std::mutex` 保证线程安全——逻辑直接、好理解、好调试，但性能不是最优的。**先做对，再做快**，这是低延迟系统开发的铁律。

这一课还记录了我们在测试中发现的两个真实 bug 以及调试过程，是理解"为什么要写测试"最好的案例。完成后，12 个 GTest 用例全部通过，第一阶段（Week 1-3）的核心功能就此完工。下一步我们会着手解决这个 mutex 版本的性能瓶颈：引入无锁的 SPSC Ring Buffer。

---

## 一、OrderBook 是什么？它要维护哪些数据？

**订单簿（Order Book）** 是交易所的核心数据结构。它维护两个列表：

```
BID（买方）                   ASK（卖方）
价格 ↓高→低                   价格 ↑低→高

102.00 │ 买单 A (qty=5)        100.00 │ 卖单 X (qty=10)
101.00 │ 买单 B (qty=3)        101.00 │ 卖单 Y (qty=7)
       │ 买单 C (qty=8)        102.00 │ 卖单 Z (qty=4)
100.50 │ 买单 D (qty=2)
```

- **Bid 侧**：按价格**从高到低**排列（出价最高的买方最优先）
- **Ask 侧**：按价格**从低到高**排列（要价最低的卖方最优先）
- 同一价格内：按**时间先后**排列（先挂单的先成交，即 FIFO）

撮合条件：**买价 ≥ 最优卖价**时，发生成交。

---

## 二、数据结构选型

### `std::map` 替代 `std::unordered_map`

```cpp
// Bid 侧：价格越高越优先（降序）
std::map<int64_t, std::deque<Order*>, std::greater<int64_t>> bid_levels_;

// Ask 侧：价格越低越优先（升序）
std::map<int64_t, std::deque<Order*>> ask_levels_;
```

**为什么用 `std::map` 而不是 `unordered_map`？**

| | `std::map` | `std::unordered_map` |
|--|--|--|
| 内部结构 | 红黑树（有序） | 哈希表（无序） |
| 查找单个价格 | O(log n) | O(1) 平均 |
| 获取最优价格（最小/最大） | **O(1)**（迭代器直接指向首尾） | O(n)，必须全部扫描 |
| 顺序遍历所有价格档位 | **O(n)，天然有序** | 无法直接有序遍历 |

撮合时需要**反复获取最优价格**（bid 的最高价、ask 的最低价），这恰好是 `std::map` 的强项：`begin()` 直接就是最优价格，O(1)。

**自定义比较器 `std::greater<int64_t>`**：让 `bid_levels_` 按价格降序排列，这样 `begin()` 是最高买价。Ask 侧默认升序，`begin()` 是最低卖价。

### `std::deque` 维护同价格的订单队列

```cpp
std::deque<Order*>
```

每个价格档位里，多个同价格订单按时间排列。`deque`（双端队列）特点：
- `push_back`：新订单追加到队尾，O(1)
- `pop_front`：成交后从队头移除，O(1)
- 天然保证 FIFO，实现时间优先

**为什么不用 `std::vector`**：`vector` 的 `erase(begin())` 是 O(n)（所有元素左移），而 `deque::pop_front()` 是 O(1)。

### `order_index_`：用于 O(1) 撤单

```cpp
std::unordered_map<uint64_t, Order*> order_index_;
```

撤单时需要根据 `order_id` 找到订单。如果每次都去 `bid_levels_` / `ask_levels_` 里线性搜索，复杂度是 O(价格档位数 × 每档位订单数)。

`order_index_` 维护 `order_id → Order*` 的映射，撤单变成 O(1) 的哈希查找。

**注意**：`order_index_` 存储的是**指针**，不是拷贝，不会有额外的内存开销。

---

## 三、撮合逻辑的核心代码（价格-时间优先）

```cpp
// 以买单进场为例（卖单对称）
while (!incoming->is_filled() && !ask_levels_.empty()) {

    auto& [ask_price, ask_queue] = *ask_levels_.begin();  // 最优卖价

    if (incoming->price < ask_price) break;  // 买价 < 最低卖价，无法成交

    while (!incoming->is_filled() && !ask_queue.empty()) {
        Order* resting = ask_queue.front();  // 同价格内最早的卖单

        int64_t match_qty = std::min(
            incoming->remaining_qty(),
            resting->remaining_qty()
        );

        incoming->filled_qty += match_qty;
        resting->filled_qty  += match_qty;

        // 成交记录：价格取挂单方（resting）的价格
        // 这是"价格优先"的标准含义：进场方接受挂单方的报价

        if (resting->is_filled()) {
            resting->status = OrderStatus::FILLED;
            order_index_.erase(resting->order_id);
            ask_queue.pop_front();
        } else {
            resting->status = OrderStatus::PARTIAL;  // ← 部分成交也要更新状态
        }
    }

    if (ask_queue.empty()) ask_levels_.erase(ask_levels_.begin());
}

// 撮合结束：更新 incoming 状态，且从 index 中移除（如果完全成交）
if (incoming->is_filled()) {
    incoming->status = OrderStatus::FILLED;
    order_index_.erase(incoming->order_id);  // ← 不要忘记清理
} else if (incoming->filled_qty > 0) {
    incoming->status = OrderStatus::PARTIAL;
}
```

### 双层循环的含义

- **外层循环**：遍历价格档位（102, 101, 100...），一个大单可能穿越多个价格层
- **内层循环**：在同一价格档位内，按时间顺序依次消耗挂单

### 成交价的确定

成交价取**挂单方（resting order）的价格**，不是进场方的价格。

这就是"价格优先"的含义：如果你挂了一个卖单在 100 元，来了一个愿意出 102 元的买单，成交价是 **100 元**（你的报价），买方占了便宜，但这也激励了卖方挂单（确保不会以更差的价格成交）。

---

## 四、mutex 的使用——RAII 锁

```cpp
std::vector<Trade> OrderBook::add_order(Order* order) {
    std::lock_guard<std::mutex> lock(mtx_);  // ← 进入时加锁
    // ...
}   // ← 函数返回时，lock 析构，自动释放锁
```

**RAII（Resource Acquisition Is Initialization）** 是 C++ 资源管理的核心范式：

- 资源（锁）在对象**构造时获取**
- 资源在对象**析构时释放**
- 无论函数如何退出（正常返回、异常、提前 return），析构函数**一定会被调用**

```cpp
// 危险的手动加锁（容易忘记解锁，或中间抛异常时死锁）
mtx_.lock();
// ... 如果这里抛出异常 ...
mtx_.unlock();  // ← 可能永远到不了这里

// 安全的 RAII 做法（lock_guard）
{
    std::lock_guard<std::mutex> lock(mtx_);
    // ... 即使这里抛出异常，lock 析构时也会自动 unlock
}
```

### 为什么 `mutable`？

```cpp
mutable std::mutex mtx_;
```

`best_bid()` 和 `best_ask()` 是 `const` 方法，但它们内部需要加锁。`mutable` 允许 `const` 方法修改这个成员，这是 `mutex` 作为"实现细节"（而非逻辑状态）的标准做法。

---

## 五、实战 Bug 记录：两个被测试发现的逻辑错误

### Bug 1：`order_count()` 在成交后不归零

**测试**：`ExactMatchFullFill`  
**期望**：买 10 卖 10 完全成交后，`order_count() == 0`  
**实际**：`order_count() == 1`

**根因分析**：

`add_order` 在开头把 incoming 订单注册进 `order_index_`：

```cpp
order_index_[order->order_id] = order;   // 注册
auto trades = match(order);              // 撮合
```

`match()` 里，resting 完全成交时会被 erase：

```cpp
if (resting->is_filled()) {
    order_index_.erase(resting->order_id);  // resting 被清理
}
```

但 incoming 完全成交时，**没有对应的 erase 操作**。于是成交后，resting 被清理，incoming 还留在 `order_index_` 里，`order_count()` 返回 1 而非 0。

**修复**：在 `match()` 末尾，incoming 完全成交时补充清理：

```cpp
if (incoming->is_filled()) {
    incoming->status = OrderStatus::FILLED;
    order_index_.erase(incoming->order_id);  // 补充这行
}
```

**教训**：凡是"注册进索引"的地方，都要有对称的"从索引移除"的地方。注册和清理要成对出现。

---

### Bug 2：部分成交的挂单状态不更新

**测试**：`PartialFillBuyLarger`  
**期望**：买 20 卖 10 成交后，买单状态是 `PARTIAL`  
**实际**：买单状态是 `NEW`（`0x00`）

**根因分析**：

撮合循环里，resting 订单（买单）的状态更新只处理了"完全成交"的分支：

```cpp
if (resting->is_filled()) {
    resting->status = OrderStatus::FILLED;
    // ...
}
// 部分成交时：什么都没做，status 还是 NEW！
```

**修复**：补充 `else` 分支：

```cpp
if (resting->is_filled()) {
    resting->status = OrderStatus::FILLED;
    order_index_.erase(resting->order_id);
    bid_queue.pop_front();
} else {
    resting->status = OrderStatus::PARTIAL;   // 补充这行
}
```

**教训**：状态机的每个转换路径都要显式处理。不要假设"没变化就是正确的默认值"。

---

### 用测试发现 Bug 的意义

这两个 Bug 在代码 review 时很容易忽略，但 GTest 立刻揭露了它们。这正是"**先保证正确性，再追求性能**"原则的体现：

1. 测试覆盖了所有分支（完全成交、部分成交、撤单、价格优先、时间优先）
2. 测试失败信息精确指出了断言位置和实际值
3. 修复后立刻重新运行，确认 12/12 通过

**写测试不是负担，是发现 bug 最便宜的方式。**

---

## 六、怎么运行测试，看到哪些输出

这一课的成果直接体现在测试通过率上。跑测试只需要两条命令：

```bash
# 编译（Debug 模式，开启 Sanitizer）
cmake --build build/debug -j$(nproc)

# 运行测试（任选其一）
./build/debug/test_order_book          # 直接运行，看详细输出
cd build/debug && ctest --output-on-failure  # 用 ctest 统一管理
```

正常通过时的输出（12 个测试全绿）：

```
[==========] Running 12 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 12 tests from OrderBookTest
[ RUN      ] OrderBookTest.EmptyBook
[       OK ] OrderBookTest.EmptyBook (0 ms)
[ RUN      ] OrderBookTest.ExactMatchFullFill
[       OK ] OrderBookTest.ExactMatchFullFill (0 ms)
...
[----------] 12 tests from OrderBookTest (0 ms total)
[==========] 12 tests ran. (0 ms total)
[  PASSED  ] 12 tests.
```

如果有 Bug 没修复，失败的测试会显示 `[ FAILED ]` 并打印期望值 vs 实际值，精确定位到断言行。Debug 模式下还会在 Sanitizer 发现内存问题时直接中止并打印调用栈，比 gdb 手动调试省力得多。

---

## 七、这个版本的性能局限（为后续优化埋下伏笔）

当前 mutex 版本的主要瓶颈：

| 问题 | 原因 | 后续解决方案 |
|-----|------|------------|
| 每次 `add_order` 都加锁 | `std::mutex` 在有竞争时需要系统调用 | SPSC ring buffer 解耦线程，OrderBook 单线程无锁 |
| `std::map` 树遍历 | 红黑树节点分散在堆上，Cache Miss 多 | 后续可考虑 array-based price level 结构 |
| 每次成交都 `malloc` 一个 Trade | `std::vector<Trade>` 的动态扩容 | MemoryPool 预分配 |

**现阶段不优化这些——先保证逻辑正确，再有数据支持地优化。**

---

## 八、面试中如何讲这部分

> "OrderBook 用 `std::map<price, deque<Order*>>` 维护买卖两侧，map 的有序性保证 `begin()` 直接是最优价格（O(1)），deque 的 FIFO 特性保证同价格内时间优先。额外维护 `unordered_map<order_id, Order*>` 的索引支持 O(1) 撤单。当前用 mutex 保证线程安全，是功能验证的基线版本，后续会用 lock-free SPSC 解耦生产消费，让 OrderBook 在单线程上无锁运行。"

---

## 九、课后问题

- [ ] 为什么 `bid_levels_` 用 `std::greater<int64_t>` 作比较器？如果不加，会发生什么？
    - 根据时间-价格优先规则，bid作为买方应该满足出价高者有限，只有使用greater才能在set中满足有序。set默认使用`less<T>`比较器。

- [ ] `std::lock_guard` 和 `std::unique_lock` 的区别是什么？什么时候需要用 `unique_lock`？
    - `lock_guard` 不能中途解锁，`unique_lock` 可以。
    - `lock_guard` 是更轻量级的RAII锁。`unique_lock`则是更灵活的锁。
    - 什么时候用：一般来说就用lock_guard.当需要手动解锁、转移锁的所有权、配合条件变量（必须使用`unique_lock`）、延迟加锁、尝试加锁的时候，才需要使用`unique_lock`。



- [ ] 双层撮合循环（外层遍历价格，内层遍历同价格队列）的最坏时间复杂度是多少？在什么场景下会触发最坏情况？
    - 设$P=$被扫过的价格档位数，$N=$参与此次撮合的订单总数。
    - 最坏的复杂度为$O(P+N)，最坏时扫描当前所有价格档，消耗全部的订单。
    - 最坏情况场景：
        - (1) 价格档位碎片化，每个档位都只有一笔订单，档位数极多。P很大，N约等于P，P主导。
        - (2) 大额单横扫盘口，一笔极高价大容量买单，消耗所有现有单。
        - (3) 集中于某价位，某价位深度极大，聚集了成千上万笔小额订单。P=1，N极大，复杂度由队列深度主导。
    - 本质上，由于$P \le N$，而且每个订单最多被处理一次，可以认为时间复杂度为$O(N)$。
    - 实际交易所会用最大成交笔数限制来防止无限地扫穿大量订单。


- [ ] 如果撤单时不从 `order_index_` erase，会有什么后果？
    - 悬挂指针，崩溃与未定义行为：Order*指针可能已经被移除，索引还保留着，就成了野指针，访问会出错。
    - 状态不一致与误判：无法准确获得订单数、重复撤单等问题。

**下一课预告**：`FeedSimulator`——行情数据的模拟与回放，整数定价的原理，正态分布模拟价格波动，以及 CSV 回放保证测试可重复性。
