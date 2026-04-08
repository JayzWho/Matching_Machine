# 第四课：FeedSimulator——行情数据的模拟与回放

> 对应阶段：Week 1-3 / Todo 1
> 关键词：整数定价、正态分布、CSV 回放、可重复性、随机种子、行情接收器

---

## 引言

上一课我们完成了 `OrderBook` 的核心撮合逻辑，并用 12 个 GTest 用例验证了它的正确性。但测试用的订单都是手写进测试代码里的——每次手动构造，价格、数量都是硬编码的整数。

真实的撮合引擎需要一个**行情数据来源**：在生产环境中，这是一个行情接收器，负责解析交易所发来的 Binary/FAST 协议数据包，转换成内部订单结构。而在我们的开发和测试阶段，我们需要一个能模拟这个角色的组件——`FeedSimulator`。

`FeedSimulator` 解决两个问题：
1. **压力测试**：随机生成大量订单，测试 `OrderBook` 在高频场景下的吞吐量和延迟
2. **回归测试**：从 CSV 文件加载固定的历史数据，让每次测试的输入完全一致，便于对比优化前后的性能

这一课还涉及一个在低延迟 C++ 里处处可见的设计决策：**为什么用整数而不是浮点数表示价格**。

---

## 一、整数定价：为什么 `price = 100123456` 而不是 `100.123456`

在 `order.h` 里，价格字段是 `int64_t price`，而不是 `double`。`FeedSimulator` 的头文件注释也说明：`base_price` 的单位是"原始 × 1e6"。

### 浮点数的本质问题

```cpp
// 经典的浮点数陷阱
double a = 0.1 + 0.2;
bool eq = (a == 0.3);   // false！

// 实际上：
// a   = 0.30000000000000004
// 0.3 = 0.29999999999999999

// 在撮合引擎里：
double bid = 100.1;
double ask = 100.1;
bool can_match = (bid >= ask);   // 理论上应该 true，但可能因精度问题返回 false
```

浮点数在 IEEE 754 标准下，大多数十进制小数（0.1、0.2 等）**不能被精确表示**，只有近似值。对撮合引擎来说，价格比较出错意味着：
- 本该成交的订单没有成交（错误的拒绝）
- 本不该成交的订单成交了（错误的匹配）

这在金融场景下是灾难性的。

### 整数定价的解决方案

```cpp
// 规定：price 存储的是"真实价格 × 1,000,000"
// 100.123456 元  →  price = 100123456
// 99.999999 元   →  price = 99999999

int64_t bid = 100'100'000;   // 100.1 元
int64_t ask = 100'100'000;   // 100.1 元
bool can_match = (bid >= ask);   // true，整数比较，精确无误
```

整数比较是**精确**的，没有任何舍入误差。整数运算（加减乘）也比浮点运算快（浮点运算需要专用 FPU，且无法向量化某些场景）。

**精度选择**：`× 1,000,000` 意味着最小价格单位是 0.000001（6 位小数）。股票市场的最小变动单位（tick size）通常是 0.01 元甚至更大，6 位小数绰绰有余。期货、加密货币的精度要求各有不同，可以调整倍数。

### `int64_t` 的范围是否足够？

`int64_t` 最大值约为 9.2 × 10¹⁸。乘以 1,000,000 后，能表示的最大价格约为 9.2 × 10¹² 元（9.2 万亿元）。全球任何金融资产的价格都远远小于这个数，范围足够。

---

## 二、FeedSimulator 的两种工作模式

```cpp
class FeedSimulator {
public:
    FeedSimulator(std::string_view symbol,
                  int64_t          base_price,
                  uint64_t         seed = 42);

    // 模式一：随机生成
    std::vector<Order> generate_random(size_t count, double cancel_ratio = 0.2);

    // 模式二：CSV 回放
    std::vector<Order> load_csv(const std::string& filepath);
};
```

### 模式一：随机生成（用于压测）

```cpp
FeedSimulator sim("BTCUSD", 100'000'000 /* 100.000000 元 */, /*seed=*/42);
auto orders = sim.generate_random(100'000, /*cancel_ratio=*/0.2);
// 生成 10 万个订单，其中约 20% 是撤单
```

**适用场景**：压力测试（benchmark）——不关心具体是哪些订单，只需要足够多的订单来测试系统极限。

### 模式二：CSV 回放（用于回归测试）

```
CSV 格式：order_id,side,price,qty,type
1,0,100000000,10,0
2,1,99900000,5,0
3,0,0,0,1
```

```cpp
auto orders = sim.load_csv("data/tick_data.csv");
```

**适用场景**：回归测试——每次输入相同，输出的成交记录必须一致。优化前后的行为对比，发现优化是否引入了新 bug。

---

## 三、随机数生成器的选型与可重复性

`FeedSimulator` 内部使用三个 C++ 标准库随机分布：

```cpp
std::mt19937_64                    rng_;           // 随机数引擎
std::normal_distribution<double>   price_dist_;    // 价格偏移：正态分布
std::uniform_int_distribution<int> qty_dist_;      // 数量：均匀分布 [1, 100]
std::bernoulli_distribution        side_dist_;     // 方向：50% BUY / 50% SELL
```

### 为什么用 `std::mt19937_64`（Mersenne Twister）？

| 特性 | `std::mt19937_64` | `std::rand()` |
|------|-------------------|---------------|
| 周期 | 2¹⁹⁹³⁷-1（约 10⁶⁰⁰⁰） | 通常 2³² ≈ 40 亿 |
| 质量 | 通过了所有统计测试 | 线性同余，质量差 |
| 线程安全 | 非线程安全（我们单线程用，没问题） | 全局状态，有竞争问题 |
| 可重复性 | 固定种子 → 固定序列 | 取决于实现 |

`std::rand()` 在 C++ 中已经是不推荐使用的遗留接口，`mt19937_64` 是现代 C++ 的标准选择。

### 可重复性：固定种子的意义

```cpp
FeedSimulator sim("BTCUSD", 100'000'000, /*seed=*/42);
auto orders1 = sim.generate_random(1000);

FeedSimulator sim2("BTCUSD", 100'000'000, /*seed=*/42);  // 相同种子
auto orders2 = sim2.generate_random(1000);

// orders1 和 orders2 完全相同！
```

**为什么重要**：
- 性能优化前后，用相同种子生成相同的订单序列，测量延迟差异才有可比性
- 发现 bug 后，用相同种子复现，确保 bug 可稳定复现（而不是随机出现）
- 多次 benchmark 对比，排除随机输入差异对结果的干扰

默认种子 42 是惯例（没有特殊含义），也可以传入时间戳作为种子，生成每次不同的随机序列。

---

## 四、价格如何模拟真实市场波动？

```cpp
// 构造函数中初始化正态分布
price_dist_(0.0, static_cast<double>(base_price) * 0.005)
// 均值 = 0，标准差 = base_price 的 0.5%
```

```cpp
// make_random_order 中生成实际价格
int64_t offset = static_cast<int64_t>(price_dist_(rng_));
o.price = base_price_ + (offset / 100) * 100;   // 取整到最小变动单位 100（即 0.0001 元）
if (o.price <= 0) o.price = base_price_;          // 防止负价格
```

这模拟了真实市场的统计特征：价格在基准价附近**正态分布**地波动，大幅偏离的概率很低。

```
价格分布（base_price = 100,000,000，标准差 = 500,000）：

 68%的订单价格落在 [99,500,000, 100,500,000] 内（即 ±0.5%）
 95%的订单价格落在 [99,000,000, 101,000,000] 内（即 ±1%）
```

**`(offset / 100) * 100` 的作用**：把连续的随机偏移量"对齐"到最小变动单位 100（对应 0.0001 元）。在实际交易所中，价格只能是 tick size 的整数倍，这里模拟了这个约束。

---

## 五、撤单的生成逻辑

```cpp
bool is_cancel = cancel_dist(rng_) && (next_order_id_ > 1);
```

```cpp
if (is_cancel) {
    o.type  = OrderType::CANCEL;
    // 撤销一个已存在的订单（随机选择一个更早的 ID）
    std::uniform_int_distribution<uint64_t> id_dist(1, next_order_id_ - 2);
    o.order_id = id_dist(rng_);
}
```

两个关键细节：

1. **`next_order_id_ > 1` 的保护**：如果一个订单都还没生成，无法生成撤单（没有合法的订单 ID 可以撤）
2. **`next_order_id_ - 2` 的上界**：`next_order_id_` 是下一个**将要**分配的 ID，所以当前最大合法 ID 是 `next_order_id_ - 2`（因为当前这个位置已经 `++` 了，要排除自己）

这一逻辑会产生**随机撤单**——被撤的订单可能已经成交，`OrderBook::cancel_order` 需要能优雅处理"撤单目标不存在"的情况（返回 false 而不是崩溃）。

---

## 六、CSV 回放的解析实现

```cpp
// 格式：order_id,side,price,qty,type
// side: 0=BUY 1=SELL, type: 0=LIMIT 1=CANCEL
while (std::getline(file, line)) {
    std::istringstream ss(line);
    std::string token;

    Order o{};
    std::getline(ss, token, ','); o.order_id = std::stoull(token);
    std::getline(ss, token, ','); o.side     = (token == "0") ? Side::BUY : Side::SELL;
    std::getline(ss, token, ','); o.price    = std::stoll(token);
    std::getline(ss, token, ','); o.quantity = std::stoll(token);
    std::getline(ss, token, ','); o.type     = (token == "0") ? OrderType::LIMIT : OrderType::CANCEL;

    std::strncpy(o.symbol, symbol_.c_str(), 7);
    orders.push_back(o);
}
```

### 注意：`std::strncpy` 的用法

```cpp
std::strncpy(o.symbol, symbol_.c_str(), 7);
// symbol 字段：char symbol[8]，最多写 7 个字符，第 8 字节保留为 '\0'
```

`strncpy(dst, src, n)` 最多写 `n` 个字节，不保证末尾有 `'\0'`（如果源长度 ≥ n）。这里显式写 `7`，剩下的第 8 个字节因为 `Order o{}` 已经被值初始化为 0，所以字符串一定是 null-terminated 的。

---

## 七、在整体架构中的位置

```
FeedSimulator
      │
      │  generate_random() / load_csv()
      │  返回 std::vector<Order>
      ↓
 [生产者线程]
      │
      │  spsc.try_push(order)
      ↓
 SPSC Ring Buffer  ←─── 下一课的主角
      │
      │  spsc.try_pop(order)
      ↓
 [消费者线程]
      │
      │  book.add_order(order)
      ↓
   OrderBook（撮合）
      │
      ↓
   std::vector<Trade>
```

`FeedSimulator` 是整个 pipeline 的**起点**，负责产生原始订单数据。它本身不涉及多线程——生成订单是单线程的批量操作，生成好之后交给生产者线程，逐条压入 SPSC Ring Buffer，再由消费者线程取出送入 `OrderBook`。

**这也是为什么下一课要学 SPSC Ring Buffer**：`FeedSimulator` 和 `OrderBook` 在不同线程，需要一个高性能的通信信道。

---

## 八、当前实现的局限

| 局限 | 说明 | 生产中的对应做法 |
|------|------|--------------|
| 价格模型过于简单 | 正态分布不符合实际市价波动（实际更接近对数正态，且有厚尾） | 使用历史数据回放，而非随机生成 |
| 无时间戳间隔控制 | 所有订单瞬间产生，不模拟真实的到达速率 | 加入泊松过程或真实 tick 时间戳 |
| CSV 解析效率一般 | `std::istringstream` + `std::getline` 对于超大文件较慢 | 使用 mmap + 手写解析器，或专用二进制格式 |
| 无行情消息聚合 | 只模拟订单，不模拟快照（order book snapshot）等复合消息 | 生产行情接收器需处理多种消息类型 |

---

## 九、怎么验证 FeedSimulator 是对的

`FeedSimulator` 本身没有独立测试文件（它的正确性通过集成测试间接验证），但可以做一个简单的冒烟测试：

```cpp
#include "feed_simulator.h"
#include "order_book.h"
#include <iostream>

int main() {
    me::FeedSimulator sim("BTCUSD", 100'000'000LL, 42);
    auto orders = sim.generate_random(10, 0.0);   // 生成 10 个纯限价单

    for (auto& o : orders) {
        std::cout << "id=" << o.order_id
                  << " side=" << (o.side == me::Side::BUY ? "BUY" : "SELL")
                  << " price=" << o.price
                  << " qty=" << o.quantity << "\n";
    }

    me::OrderBook book("BTCUSD");
    for (auto& o : orders) {
        auto trades = book.add_order(&o);
        if (!trades.empty()) {
            std::cout << "  => " << trades.size() << " trade(s) generated\n";
        }
    }
    return 0;
}
```

关注点：
- 输出的 `price` 是否在 `base_price ± 几个标准差` 内（不出现离谱的价格）
- 相同种子运行两次，输出是否完全一致
- `load_csv` 后再送入 `OrderBook`，成交记录是否与预期一致

---

## 十、面试中如何讲这部分

> "FeedSimulator 是撮合引擎的行情数据入口，模拟生产中的行情接收器。随机模式用正态分布生成围绕基准价格波动的限价单，固定种子保证可重复性，方便基准测试对比；CSV 回放模式加载真实 tick 数据做回归测试。价格用 `int64_t × 1e6` 的整数定价，彻底避免浮点比较精度问题——这是撮合引擎的标准做法，所有知名交易所的内部系统都用整数表示价格。"

---

## 十一、课后问题

- [ ] 为什么撤单生成时选择的 `order_id` 上界是 `next_order_id_ - 2` 而不是 `next_order_id_ - 1`？
    - 因为生成新单时next_order_id已经++了，比如当前单号为9，则函数中next_order_id已经变成10，为了保证撤单号有效，单号必须比9小，所以选择-2.

- [ ] `std::normal_distribution` 理论上可以生成负数偏移，代码中用 `if (o.price <= 0) o.price = base_price_` 做了保护，但这会破坏价格分布的统计特性吗？在实际场景中这个问题有多严重？
    - 这个保护确实会破坏正态分布（引入截断和偏移），但在 base_price 远大于波动范围时几乎不会触发，因此在压测场景中影响可以忽略；只有在高波动或小基准价格下才会变得显著。

- [ ] CSV 解析使用 `std::stoll` / `std::stoull`，如果 CSV 文件里有一行格式错误（比如 price 字段是空字符串），会发生什么？如何让解析更健壮？
    - stoll 遇到空字符串或无效字符串会抛异常，而健壮系统必须避免异常传播，应使用 from_chars 或显式校验来保证解析安全和高性能。

- [ ] 如果要让 `FeedSimulator` 模拟真实的"到达速率"（比如每秒 100 万个订单），应该在哪里加入速率控制逻辑？
    - 应该在generate_random函数中（调用make_random_order）的函数中按一定时间间隔生成随机订单。
    - 速率控制可以用sleep定时，也可以为了避免sleep精度太低，使用token bucket生成。


**下一课预告**：`SPSC Ring Buffer`——无锁数据结构的原理，`std::atomic` 的内存序，以及如何用 `alignas(64)` 消除 false sharing。
