# 第五课：Lock-Free SPSC Ring Buffer——无锁数据结构原理

> 对应阶段：Week 4-5 / Todo 2
> 关键词：std::atomic、memory_order、false sharing、ring buffer、lock-free

---

## 引言

上一阶段我们用 mutex 版本的 OrderBook 跑通了所有测试，还实现了 `FeedSimulator` 来模拟行情数据输入。但当 `FeedSimulator`（行情生产者）和 `OrderBook`（撮合消费者）运行在不同线程、共享一个带锁的队列时，每次传递订单都要经历加锁→竞争→解锁，在高频场景下这个开销难以接受。

这一课的目标是用一个**无锁队列**替换掉这把锁。具体来说，因为我们的架构恰好是"单一生产者、单一消费者"（SPSC），这是无锁数据结构里最简单、也最实用的场景——不需要 CAS，只需要 `std::atomic` 配合正确的内存序就能实现。

这也是量化私募面试中被问烂的考点，理解这一课的内容，你就能清楚地解释"无锁为什么比有锁快"以及"内存序到底在控制什么"。实现完 SPSC Ring Buffer 后，我们接着要解决另一个热路径性能问题：每次 `new Order()` 的 malloc 开销，也就是下一课的内存池。

---

## 一、为什么需要 Ring Buffer？

在撮合引擎的架构中，有两个并发角色：

```
FeedSimulator（生产者线程）  →  OrderBook（消费者/处理线程）
     产生 MarketDataEvent            执行撮合逻辑
```

这两个线程需要通信。最直接的方式是共享一个队列，但如何保证线程安全？

**方案 A：`std::queue` + `std::mutex`**

```cpp
std::mutex mtx;
std::queue<Order> q;

// 生产者
mtx.lock();
q.push(order);
mtx.unlock();

// 消费者
mtx.lock();
auto item = q.front(); q.pop();
mtx.unlock();
```

问题：`mutex` 的加锁/解锁在有竞争时需要**系统调用**，延迟可达几微秒甚至几十微秒。在每秒百万次操作的场景下，这是无法接受的开销。

**方案 B：Lock-Free SPSC Ring Buffer**

只有单一生产者和单一消费者时，可以用纯原子操作实现无锁通信。延迟从微秒级降到纳秒级。

---

## 二、Ring Buffer 的数据结构

Ring Buffer（环形缓冲区）是一个**首尾相连的固定大小数组**：

```
容量 = 8（索引 0-7）

  [0][1][2][3][4][5][6][7]
         ↑           ↑
        head        tail
        (消费者)    (生产者)
```

- `tail`：生产者下一次写入的位置（写完后 tail 向前移动）
- `head`：消费者下一次读取的位置（读完后 head 向前移动）
- **队列为空**：`head == tail`
- **队列已满**：`(tail + 1) % Capacity == head`

"环形"的实现：索引到达末尾后自动绕回到开头：

```cpp
next_idx = (current_idx + 1) & (Capacity - 1);  // 位运算，比取模快
// 等价于：(current_idx + 1) % Capacity
// 要求：Capacity 必须是 2 的幂
```

---

## 三、为什么 SPSC 可以无锁

关键洞察：**生产者只写 `tail`，消费者只写 `head`。**

```
生产者：读 head（判断满）→ 写数据 → 写 tail
消费者：读 tail（判断空）→ 读数据 → 写 head
```

每个变量只有一个写入方，不存在"两个线程同时写同一个变量"的竞争。这就是为什么 SPSC 比 MPMC（多生产者多消费者）简单——MPMC 需要解决多个写入方的竞争，必须用更重的同步机制（CAS 循环）。

但是，在多核 CPU 上，**写操作不一定立刻对另一个核可见**——这就引入了内存序的问题。

---

## 四、内存序（Memory Order）——最难但最关键的概念

### 为什么会有"可见性"问题？

现代 CPU 和编译器为了提高性能，会对指令进行**重排（reorder）**：

```cpp
// 你写的代码顺序
buffer_[tail] = item;   // 1. 写数据
tail_ = tail + 1;       // 2. 更新 tail

// CPU/编译器可能实际执行的顺序
tail_ = tail + 1;       // 2 先执行了！
buffer_[tail] = item;   // 1 后执行
```

如果消费者在第 2 步后、第 1 步前去读数据，它看到 `tail` 已更新，以为有新数据，但 `buffer_[tail]` 还是旧值——数据损坏！

### `std::atomic` 的内存序选项

C++ 提供了一套内存序标记，让你精确控制允许哪些重排：

| 内存序 | 含义 | 性能 |
|--------|------|------|
| `memory_order_relaxed` | 只保证原子性，不限制重排 | 最快 |
| `memory_order_acquire` | 本次读之后的指令不能往前移 | 较快 |
| `memory_order_release` | 本次写之前的指令不能往后移 | 较快 |
| `memory_order_seq_cst` | 全局一致顺序，最强保证 | 最慢（默认值） |

### SPSC 的内存序选择

```cpp
// ── 生产者 try_push ──
const size_t tail = tail_.load(memory_order_relaxed);   // 只有自己写，无需同步
const size_t next = (tail + 1) & mask_;

if (next == head_.load(memory_order_acquire)) { ... }   // acquire：确保看到消费者最新的 head

buffer_[tail] = item;                                    // 写数据

tail_.store(next, memory_order_release);                 // release：确保数据写完再更新 tail
//                                                          消费者 acquire 读 tail 时，data 已可见

// ── 消费者 try_pop ──
const size_t head = head_.load(memory_order_relaxed);   // 只有自己写

if (head == tail_.load(memory_order_acquire)) { ... }   // acquire：配对生产者的 release

item = buffer_[head];                                    // 读数据

head_.store((head + 1) & mask_, memory_order_release);  // release：通知生产者 head 已更新
```

**acquire-release 配对**是无锁编程的核心模式：
- 生产者的 `release` 写：**此前的所有写操作，在消费者执行配对的 `acquire` 读之后都可见**
- 相当于一个"内存屏障"，告诉 CPU 不要越过这个点乱序执行

---

## 五、Capacity 必须是 2 的幂——为什么？

```cpp
static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
```

这个约束有两个原因：

**原因 1：位运算比取模快**

```cpp
// 取模（可能触发除法指令，几个时钟周期）
next_idx = (current_idx + 1) % Capacity;

// 位运算（1 个时钟周期）
next_idx = (current_idx + 1) & (Capacity - 1);
// 当 Capacity = 8（二进制 1000），Capacity-1 = 0111
// 任何数 & 0111 的结果都在 [0, 7] 范围内
```

**原因 2：数学性质保证**

`(Capacity - 1)` 是全 1 的位掩码，当且仅当 Capacity 是 2 的幂时成立。如果 Capacity = 6，`Capacity - 1 = 5 = 101b`，掩码不能覆盖所有索引，导致回绕计算错误。

---

## 六、"一个槽位不使用"的设计——区分空和满

为什么 `capacity()` 返回 `Capacity - 1`？

如果 head 和 tail 相同时既可能是"空"，也可能是"绕了一圈变满了"——无法区分。

解决方案：**永远保留一个槽位不存储数据**，规定：
- `head == tail` → 队列**空**
- `(tail + 1) % Capacity == head` → 队列**满**（tail 追上了 head 的前一位）

所以实际可存储元素数是 `Capacity - 1`，而非 `Capacity`。

---

## 七、消除 False Sharing

```cpp
alignas(64) std::atomic<size_t> head_{0};   // 消费者操作
alignas(64) std::atomic<size_t> tail_{0};   // 生产者操作
alignas(64) std::array<T, Capacity> buffer_{};  // 数据区
```

三个区域各自独占不同的 Cache Line：

```
Cache Line A  [         head_          ]  ← 消费者频繁读写
Cache Line B  [         tail_          ]  ← 生产者频繁读写
Cache Line C+ [    buffer_ 数据区      ]  ← 读写都有
```

如果 `head_` 和 `tail_` 在同一 Cache Line 上，生产者修改 `tail_` 会导致消费者的 `head_` 所在的 Cache Line 失效，消费者需要重新从内存加载——即使 `head_` 的值根本没变。这就是 False Sharing。

---

## 八、测试策略与验证方式

```
单线程测试（基础正确性）：
  - 初始为空 ✓
  - push/pop 基础功能 ✓
  - FIFO 顺序 ✓
  - 满时 push 失败 ✓
  - 空时 pop 失败 ✓
  - 环形回绕（WrapAround）✓

并发测试（内存序正确性）：
  - 10000 元素生产者-消费者，验证顺序无损 ✓
  - 1000 元素求和，验证无数据丢失 ✓
```

**为什么要单独测试 WrapAround**：最容易出 bug 的地方就是索引绕回时。写到 7 后变成 0，如果位运算有误，这里会出现索引越界或死锁。

运行这些测试：

```bash
# 编译（Debug 模式，Sanitizer 能抓并发问题）
cmake --build build/debug -j$(nproc)

# 运行（测试名视你的 CMakeLists.txt 中的 add_executable 名称而定）
./build/debug/test_spsc_ring_buffer
# 或
cd build/debug && ctest -R spsc --output-on-failure
```

并发测试（生产者-消费者各一个线程）在 Debug+Sanitizer 下跑，如果内存序写错，ThreadSanitizer (TSan) 会直接报 data race；如果逻辑错，元素总数或顺序校验的 `EXPECT_EQ` 会失败。

---

## 九、面试中如何讲这部分

> "SPSC Ring Buffer 利用生产者只写 tail、消费者只写 head 的特性，实现无锁通信。head 和 tail 用 acquire-release 内存序配对，保证数据写完后才更新索引，消费者读到索引更新后数据已可见。两个 atomic 各自对齐到独立的 Cache Line，消除 False Sharing。"

---

## 十、课后问题

- [ ] 为什么 `tail_.load(memory_order_relaxed)` 安全？生产者读自己的 `tail_` 为什么不需要 acquire？
  - acquire/release 是为跨线程同步服务的；单线程自读自写的原子变量，用 relaxed 即可。

- [ ] 如果把所有 memory_order 都改成 `memory_order_seq_cst`，功能上还正确吗？性能上会有什么影响？
  - 功能正确，性能下降。seq_cst会给指令加上额外的内存屏障指令，带来额外开销。

- [ ] SPSC 在什么条件下不再适用，需要升级为 MPSC 或 MPMC？
- [ ] Ring Buffer 的容量为什么要在编译期确定（模板参数），而不是运行时传入？

**下一课预告**：内存池（MemoryPool）——如何用空闲链表实现 O(1) 分配归还，`aligned_storage` 的作用，`placement new` 与析构的配对使用。
