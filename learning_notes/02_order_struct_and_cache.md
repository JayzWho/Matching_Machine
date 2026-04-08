# 第二课：Order 结构体设计 & CPU Cache 原理

> 对应阶段：Week 1-3 / Todo 1
> 关键词：alignas、cache line、false sharing、整数定价、static_assert

---

## 引言

上一课我们搭好了 CMake 骨架，现在开始写第一个核心数据结构：`Order`（订单）。

`Order` 是整个系统里流动频率最高的对象。每一笔行情进来，都会产生一个 `Order`，它要被放进队列、传给撮合引擎、比较价格、更新状态、最终被销毁。在每秒百万订单的场景下，一个 `Order` 对象的内存布局设计得好不好，会直接影响 CPU 访问它的速度。

这一课的核心问题是：**怎么设计一个"对 CPU 友好"的结构体**？我们会学到 Cache Line 对齐、整数定价、`static_assert` 编译期守卫这几个低延迟 C++ 的基础手法。学完之后，我们就可以着手实现 `OrderBook`——把 `Order` 对象真正组织成一个可以撮合的订单簿。

---

## 一、从一个问题出发

你的 `order.h` 里有这样一行：

```cpp
struct alignas(64) Order { ... };
```

为什么是 64？这个数字来自哪里？删掉它会怎样？

要回答这个问题，需要先理解 CPU 是怎么访问内存的。

---

## 二、CPU Cache 的工作原理

### 内存访问速度的现实

CPU 和内存之间速度差距巨大：

| 存储层级 | 访问延迟 | 典型大小 |
|---------|---------|---------|
| CPU 寄存器 | < 1 ns | 几十个 |
| L1 Cache | ~1 ns | 32-64 KB |
| L2 Cache | ~4 ns | 256 KB - 1 MB |
| L3 Cache | ~10-40 ns | 4-32 MB |
| 内存（DRAM） | ~100 ns | GB 级别 |
| SSD | ~100 μs | TB 级别 |

**核心结论：从内存读一次数据，相当于 L1 Cache 读 100 次的时间。**  
在每秒处理百万订单的系统里，内存访问次数直接决定性能上限。

### Cache Line：内存传输的最小单位

CPU 不会每次只从内存读"你需要的那几个字节"，而是**一次性读取 64 字节**，放入 Cache，称为一个 **Cache Line**。

```
内存：
┌────┬────┬────┬────┬────┬────┬────┬────┐
│byte│byte│byte│byte│byte│byte│byte│byte│  ← 连续 64 字节 = 1 Cache Line
│ 0  │ 1  │ 2  │ 3  │ ...│ 60 │ 61 │ 63 │
└────┴────┴────┴────┴────┴────┴────┴────┘
              CPU 一次性全部读走
```

**类比**：你去书店找一本书，图书馆员工不会只给你那一页，而是把整本书拿过来（因为你很可能接着要看后面的页）。

### 局部性原理（Locality）

Cache 的设计基于两个假设，通常在程序中成立：

- **时间局部性**：你刚访问过的数据，很快还会再访问（循环变量）
- **空间局部性**：你访问了某个地址，很可能马上访问它旁边的地址（数组遍历）

---

## 三、`alignas(64)` 解决什么问题

### 没有对齐时的隐患：跨 Cache Line

假设 `Order` 结构体没有对齐，分配在内存地址 48 处：

```
Cache Line 0 (bytes  0-63)   Cache Line 1 (bytes 64-127)
┌──────────────────────────┐  ┌──────────────────────────┐
│  ...  │ Order 的前 16 字节│  │Order 的后 48 字节│  ...  │
└──────────────────────────┘  └──────────────────────────┘
```

访问这一个 `Order` 对象，CPU 需要**加载两条 Cache Line**。在高频场景下，这意味着多一倍的内存带宽消耗。

### `alignas(64)` 的效果

强制编译器把 `Order` 的起始地址对齐到 64 的整数倍：

```
Cache Line N (bytes 64N - 64N+63)
┌──────────────────────────────────────────────┐
│              完整的 Order 对象（64字节）        │
└──────────────────────────────────────────────┘
```

**一个 `Order` 对象，恰好一条 Cache Line，绝不跨越边界。**

### `static_assert` 是防护网

```cpp
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes (one cache line)");
```

这行代码在**编译期**就检查 `Order` 的大小是否恰好是 64 字节。如果你后来加了字段导致结构体变大，编译直接报错，不会等到运行时才发现对齐失效。

**这是低延迟 C++ 代码的常见防御性编程习惯。**

---

## 四、False Sharing（伪共享）——多线程的隐性性能杀手

### 问题描述

假设两个线程分别操作两个**不同**的变量，但这两个变量恰好在**同一条 Cache Line** 上：

```
Cache Line（64 字节）
┌─────────────────────┬─────────────────────┐
│  变量 A（线程 1 写） │  变量 B（线程 2 写） │
└─────────────────────┴─────────────────────┘
```

**发生了什么**：
1. 线程 1 修改变量 A → 该 Cache Line 被标记为"已修改"
2. 线程 2 要修改变量 B → CPU 缓存一致性协议（MESI）强制线程 2 先从内存重新加载这条 Cache Line
3. 线程 1 再修改 A → 又要等线程 2 写完……

两个线程在操作**逻辑上完全无关**的变量，却互相等待，性能可能下降 **5-10 倍**。这就是 **False Sharing**。

### 在本项目中如何消除

在 `spsc_ring_buffer.h` 中（Week 4-5 会实现），你会看到：

```cpp
alignas(64) std::atomic<size_t> head_{0};  // 生产者使用
alignas(64) std::atomic<size_t> tail_{0};  // 消费者使用
```

`head_` 和 `tail_` 各自独占一条 Cache Line，生产者和消费者永远不会争同一条 Cache Line。

---

## 五、整数定价（Fixed-Point Integer）

### 为什么不用 `double`？

```cpp
// 危险的浮点数比较
double price1 = 0.1 + 0.2;   // 实际值：0.30000000000000004
double price2 = 0.3;
price1 == price2;  // false！二进制浮点数无法精确表示 0.1
```

在撮合引擎里，价格比较是核心操作。如果用 `double`，可能出现"两个理论上相等的价格被判断为不相等"，导致撮合错误——这是严重的金融 bug。

### 整数定价方案

```cpp
int64_t price = 0;   // 实际价格 × 1,000,000
```

| 实际价格 | 存储值 |
|---------|-------|
| 100.000000 | 100000000 |
| 100.123456 | 100123456 |
| 99.999999 | 99999999 |

```cpp
// 现在比较完全精确
int64_t p1 = 100123456;
int64_t p2 = 100123456;
p1 == p2;  // true，整数比较，无精度问题
```

**精度**：6位小数（精确到 0.000001 元），对大多数金融品种足够。

**整数运算也更快**：现代 CPU 的整数乘法比浮点乘法便宜（在某些 CPU 上延迟减半）。

### 业界实践

这不是我们自己发明的——几乎所有交易所和高频交易系统都采用整数定价：
- 上交所、深交所的 Level 2 数据：价格单位是"分"（整数）
- FIX 协议：价格字段通常是 `int64` 配合精度字段
- CME（芝加哥商业交易所）的 MDP 3.0 协议：`int64` + price exponent

---

## 六、`Order` 结构体字段设计解析

```cpp
struct alignas(64) Order {
    uint64_t    order_id      = 0;   // 8 字节：全局唯一 ID，用于撤单
    uint64_t    timestamp_ns  = 0;   // 8 字节：时间戳，保证时间优先
    int64_t     price         = 0;   // 8 字节：整数定价
    int64_t     quantity      = 0;   // 8 字节：总数量
    int64_t     filled_qty    = 0;   // 8 字节：已成交，剩余 = quantity - filled_qty
    char        symbol[8]     = {};  // 8 字节：品种代码，定长避免动态内存
    Side        side          = {};  // 1 字节：BUY/SELL
    OrderType   type          = {};  // 1 字节：LIMIT/CANCEL
    OrderStatus status        = {};  // 1 字节：NEW/PARTIAL/FILLED/CANCELLED
    uint8_t     _pad[13]      = {};  // 13 字节：填充到 64 字节
    // 合计：8+8+8+8+8+8+1+1+1+13 = 64 字节 ✓
};
```

**关键设计决策**：

- **`symbol[8]` 而非 `std::string`**：`std::string` 在堆上分配内存，访问时要解引用指针（额外内存跳转）。固定 8 字节的 `char` 数组是 POD 类型，直接内嵌在结构体里，零开销。
- **`filled_qty` 而非 `remaining_qty`**：存储"已成交量"而非"剩余量"，因为累加比递减更直观，且 `remaining_qty()` 可以实时计算。
- **`enum class` 而非 `bool`**：`bool is_buy` 语义不够扩展，`enum class Side` 可以未来加 `SHORT`、`COVER` 等方向。

---

## 七、怎么验证这一课的成果

这一课没有独立的测试文件，验证的方式是**让编译器替你检查**：最核心的那行 `static_assert` 就是验证手段。

```bash
# Debug 模式编译，static_assert 失败会在编译期直接报错
cmake --build build/debug -j$(nproc)
```

如果编译通过，说明 `Order` 的大小恰好是 64 字节，对齐没有问题。如果你想进一步确认内存布局，可以在 `main.cpp` 或测试里临时加一行打印：

```cpp
#include <iostream>
#include "order.h"
int main() {
    std::cout << "sizeof(Order) = " << sizeof(me::Order) << "\n";
    std::cout << "alignof(Order) = " << alignof(me::Order) << "\n";
}
```

期望输出：
```
sizeof(Order) = 64
alignof(Order) = 64
```

---

## 八、面试中如何讲这部分

> "Order 结构体用 `alignas(64)` 强制对齐到 CPU Cache Line，确保单个订单对象不跨越 Cache Line 边界。价格使用整数定价（`int64_t`，精度 1e-6），避免浮点数精度问题。结构体大小用 `static_assert` 在编译期锁定为 64 字节，防止字段变更时静默破坏对齐。"

追问可能来的方向：
- "什么是 False Sharing？你怎么避免的？" → SPSC 里 head/tail 分离到不同 Cache Line
- "为什么不用 `double` 表示价格？" → 浮点精度问题 + 整数运算更快
- "`static_assert` 和运行时 assert 的区别？" → 编译期检查，零运行时开销

---

## 九、课后问题

- [ ] 如果在 `Order` 里加一个 `uint64_t extra_field`，`_pad` 应该改成几字节？为什么？
    - 5字节，13-8=5。
    - 但要注意成员变量的内存对齐关系，不能把8字节的新变量加在原先1字节的变量后面，会造成8字节变量内存不对齐

- [ ] `alignas(64)` 只是保证对象起始地址对齐，如果你创建了一个 `Order` 数组 `Order arr[1000]`，每个元素还是对齐的吗？为什么？（提示：`sizeof(Order) == 64` 是关键）
    - 对齐

- [ ] False Sharing 和 True Sharing（真正的数据竞争）有什么区别？分别用什么方法解决？
    - False Sharing是不同线程操作不同变量，但恰好在同一个cache line；这回引发共享数据的误判，造成cache miss和额外的同步开销。解决方法是内存布局优化，比如alignas(64),padding,变量分离等。
    - True Sharing是不同线程操作相同变量，这会引发真正的竞争，造成死锁或数据不一致。解决方法是加锁，比如mutex,atomic。

**下一课预告**：`OrderBook` 的实现——`std::map` 的数据结构选择、`mutex` 锁的使用、撮合循环的逻辑，以及两个 bug 的调试过程。
