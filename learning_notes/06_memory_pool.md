# 第六课：内存池（Memory Pool）——消灭热路径上的 malloc

> 对应阶段：Week 4-5 / Todo 2
> 关键词：malloc 延迟、空闲链表、placement new、aligned_storage、RAII

---

## 引言

上一课我们解决了线程间通信的锁开销，用 SPSC Ring Buffer 实现了无锁的订单传递。但还有另一个"隐藏的慢点"：每次 `new Order()` 背后都是一次 `malloc`，而 `malloc` 的延迟不稳定，偶发时可能飙到几十微秒——直接毁掉我们好不容易压下来的 P99。

这一课我们自己实现一个**内存池**：程序启动时一次性预分配好一大块内存，之后每次"分配"和"归还"只是在这块内存上做链表操作，完全不碰系统堆，延迟稳定在个位数纳秒。

这是低延迟系统的标配手法。学完这一课，加上前面的 SPSC，我们就把热路径上的两大性能隐患（锁竞争、动态内存分配）都消灭了。接下来的第七课，我们会学如何用 RDTSC 指令**量化地测量**我们做的这些优化到底带来了多少提升。

---

## 一、为什么要自己管内存？

在普通 C++ 程序里，`new Order()` 是理所当然的。但在低延迟系统里，它是一个严重的性能问题。

### `malloc`/`new` 的延迟问题

```
常规内存分配（new / malloc）
  ↓
系统堆分配器（glibc ptmalloc / tcmalloc）
  ↓
可能触发：内存碎片整理、系统调用 brk/mmap、锁竞争（多线程堆）
  ↓
延迟：通常 50-500 ns，偶发 μs 甚至 ms 级别的尖峰
```

**P99 延迟的敌人是"尖峰"**，不是"平均值"。一次偶发的 1ms malloc 可能让你的 P99 从 1μs 变成 1ms——高出 1000 倍。

对比内存池：
```
内存池 allocate()
  ↓
从空闲链表头取出槽位（几条指令）
  ↓
延迟：< 10 ns，且稳定，无尖峰
```

---

## 二、内存池的核心数据结构：空闲链表

### 设计思路

预先分配一大块内存，切分成 N 个等大的"槽位"。用一个**空闲链表**串联所有未使用的槽位：

```
初始状态（N=5）：
free_head_ → [0] → [1] → [2] → [3] → [4] → kNull（链表结尾）

分配一个槽位后：
free_head_ → [1] → [2] → [3] → [4] → kNull
                             （槽位 0 已分配，指针交给用户）

归还槽位 0 后：
free_head_ → [0] → [1] → [2] → [3] → [4] → kNull
```

### 实现

```cpp
// 关键数据成员
std::array<StorageSlot, MaxObjects> storage_{};    // 预分配内存块
std::array<size_t, MaxObjects>      free_list_{};  // 空闲链表（存储"下一个空闲槽的索引"）
size_t free_head_ = 0;                              // 链表头
```

`free_list_[i]` 的含义是：**槽位 i 归还后，下一个空闲槽的索引**。

```cpp
// allocate O(1)：取链表头
const size_t idx = free_head_;
free_head_ = free_list_[idx];   // 头指针移向下一个空闲槽

// deallocate O(1)：插回链表头（头插法）
free_list_[idx] = free_head_;
free_head_ = idx;
```

---

## 三、`aligned_storage` 和 `placement new`

### 为什么不直接用 `std::array<T, MaxObjects>`？

```cpp
// 危险做法（直接数组）
std::array<T, MaxObjects> storage;
// 问题：数组创建时，所有 T 的构造函数立即被调用
// 分配未分配的槽位就已经有"对象"在里面了，语义混乱
```

我们需要的是："只分配内存，不构造对象"。

### `aligned_storage`：原始内存块

```cpp
using StorageSlot = std::aligned_storage_t<sizeof(T), alignof(T)>;
std::array<StorageSlot, MaxObjects> storage_{};
```

`std::aligned_storage_t<size, align>` 是一块**原始内存**：
- 大小为 `size` 字节
- 对齐要求为 `align` 字节（保证满足 T 的对齐要求）
- **没有任何构造函数被调用**，只是一块字节序列

### `placement new`：在指定地址构造对象

```cpp
T* allocate() {
    const size_t idx = free_head_;
    free_head_ = free_list_[idx];
    
    return new (&storage_[idx]) T{};   // ← placement new
    //          ↑ 在这个地址上构造 T
}
```

`new (ptr) T{}` 的含义："在 `ptr` 指向的地址上构造 T 对象"，**不分配内存**，只调用构造函数。

### 显式析构：与 `placement new` 配对

```cpp
void deallocate(T* ptr) {
    ptr->~T();    // ← 显式析构（与 placement new 配对）
    // 归还槽位到链表...
}
```

普通的 `delete ptr` 会做两件事：调用析构 + 释放内存。  
用了 `placement new` 后，内存是我们自己管理的，**只需要调用析构，不能调用 delete**。  
所以要显式调用析构函数 `ptr->~T()`。

**成对关系**：
```
malloc/new     ↔  free/delete
placement new  ↔  显式析构 + 手动归还内存
```

---

## 四、为什么 allocate 之后对象是"干净的"？

```cpp
return new (&storage_[idx]) T{};   // {} 触发值初始化
```

`T{}` 的 `{}` 触发**值初始化（value initialization）**：如果 T 是 POD 类型或有默认构造函数，所有成员被零初始化或默认构造。

这意味着从池里取出的 `Order` 对象，`order_id = 0, filled_qty = 0, status = NEW`——正是我们想要的"全新订单"的初始状态。

测试验证了这一点：

```cpp
TEST(MemoryPoolTest, AllocateFreeAllocateReusesMemory) {
    MemoryPool<Order, 1> pool;
    Order* p1 = pool.allocate();
    p1->order_id = 1;
    pool.deallocate(p1);       // 析构，归还

    Order* p2 = pool.allocate();  // 从同一槽位再次分配
    EXPECT_EQ(p2->order_id, 0u);  // placement new 重新构造，order_id = 0 ✓
}
```

---

## 五、与 Order 结构体的配合

```cpp
MemoryPool<Order, 10000> order_pool;

// 生产者收到行情，从池中获取 Order
Order* o = order_pool.allocate();
o->order_id = next_id++;
o->price = 100'000'000;
o->quantity = 10;
o->side = Side::BUY;

// 发送给 OrderBook 处理
spsc_buffer.try_push(o);

// OrderBook 处理完成后，归还给池
order_pool.deallocate(o);
```

整个流程**零 malloc**：Order 对象的生命周期完全在池的预分配内存内。

---

## 六、当前实现的局限（诚实的自我评估）

| 局限 | 说明 | 后续改进思路 |
|------|------|------------|
| 非线程安全 | 适合单线程或 SPSC 架构下由单一线程管理 | 若需多线程，可用 `std::atomic` 的 CAS 实现无锁版本 |
| 容量固定 | 超出 MaxObjects 时返回 nullptr，需要调用方处理 | 生产中会监控 `available()`，提前扩容或告警 |
| 只支持单一类型 | 每种对象需要独立的池 | 可以用 `void*` + 固定块大小实现通用版本 |
| `std::aligned_storage` 在 C++23 被弃用 | C++23 推荐用 `std::byte` 数组 + 手动对齐 | 目前对 C++20 完全可用，暂无影响 |

---

## 七、怎么验证内存池是对的

内存池的测试有两个重点：功能正确性（分配/归还结果符合预期）和"内存不泄漏、不重复释放"（Sanitizer 负责）。

```bash
# Debug 模式编译（开启 AddressSanitizer）
cmake --build build/debug -j$(nproc)

# 运行内存池测试
./build/debug/test_memory_pool
# 或
cd build/debug && ctest -R memory_pool --output-on-failure
```

主要验证的用例：

| 测试名 | 验证内容 |
|--------|---------|
| `AllocateAndDeallocate` | 分配到的指针非空，归还后 `available()` 恢复 |
| `AllocateFreeAllocateReusesMemory` | 归还后再分配拿到同一块内存，且 `placement new` 重新初始化字段 |
| `PoolExhaustion` | 超出 `MaxObjects` 时返回 `nullptr` 而非崩溃 |
| `MultipleObjects` | 连续分配多个，地址互不重叠 |

AddressSanitizer 在你双重释放或越界写时会直接 `abort` 并打印报告，这是验证 `placement new` + 显式析构配对是否正确的最快手段。

---

## 八、面试中如何讲这部分

> "内存池用 `aligned_storage` 预分配固定数量的对象槽位，通过空闲链表实现 O(1) 分配和归还，用 `placement new` 在指定地址构造对象并与显式析构配对。核心收益是消除热路径上的 `malloc` 调用，把内存分配延迟从不可控的几百纳秒降到稳定的几纳秒，消除 P99 尖峰。"

---

## 九、课后问题

- [ ] 为什么 `deallocate` 要先调用析构函数，再把槽位还给链表？如果顺序反过来会有什么问题？
  - 在本项目SPSC，仅有单线程操作内存池的情况下，是不会有问题的。
  - 顺序反过来的话，是先记录回收，再真正析构资源，这是不合理的。记录回收后实际上已经可以通过allocate分配该位置的资源。合理的做法是先析构再记录回收，这样语义清晰。
  - 多线程情况下，这样肯定有问题，因为内存池已经记录了该节点资源可分配，然而却实际上没有析构。但是那暂时不是本项目的情况。

- [ ] `std::aligned_storage_t<sizeof(T), alignof(T)>` 中，为什么需要同时指定 `sizeof(T)` 和 `alignof(T)`？如果只用 `sizeof(T)` 会有什么问题？
  - sizeof保证单元内存大小足够存放T，alignof保证内存对齐。两者的值不是一样的，后者由最严格的成员决定。
  - 不加alignof(T)，则分配内存时不保证对齐。8字节的对象可能分配到1字节对齐的地址上，这会带来性能损失或直接出错。
  - 这里不按alignof(64B)，是因为这里单个节点只会被单线程访问，而之前环形队列里按alignas(64)是为了解决False Sharing，让两个频繁被不同线程访问的变量不要挤在同一个Cache Line。


- [ ] 内存池的空闲链表和数据结构课上学的单向链表有什么区别？（提示：这里没有 `next` 指针）
  - 本项目中的空闲链表使用一个数组，存放下标索引来表示，本节点的下一个空闲节点在哪里。普通的单向链表使用一个带指针的node节点结构体串联接起来，并用指向下一个节点的节点来遍历访问。
  - 这样的好处是，传统链表的节点分散在堆内存上，可能碎片化，而使用数组是内存上连续的。
  - cache友好：使用数组index访问，数组连续，cache miss低。
  - 内存利用率：只存index，本来的节点每个都有一个8字节的指针。
  - 数据解耦：我们并不需要在节点存储数据，只需要知道下一个节点在哪里即可。所以用数组记录即可。



- [ ] 如果 T 的析构函数会抛异常，`deallocate` 里的 `ptr->~T()` 会发生什么？应该怎么处理？
  - 一般来说，析构函数不该抛异常，如果真的抛异常，而deallocate没捕获，程序会直接崩溃。
  - 首先，析构函数就该是noexcept的，只做不可能失败的操作。对于可能失败的操作，放到别的地方。
  - 其次，如果会抛异常，就必须在deallocate中捕获。

**下一课预告**：`LatencyRecorder` 与 RDTSC——如何用 CPU 时间戳指令测量纳秒级延迟，P50/P99 统计原理，以及多核环境下 TSC 的陷阱。
