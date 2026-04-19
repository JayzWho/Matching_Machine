#pragma once

#include "order.h"
#include <string_view>
#include <vector>
#include <random>
#include <fstream>
#include <cstdint>

namespace me {

/**
 * @brief 模拟行情数据生成器
 *
 * 提供两种模式：
 *   1. 随机生成：生成围绕基准价格随机波动的限价单/撤单序列
 *   2. CSV 回放：从文件读取历史 tick 数据（格式：order_id,side,price,qty,type）
 *
 * 生产中的对应物：交易所行情接收器（如上交所的 Binary 协议、FAST 协议解析器），
 * 本类是其简化模拟，重点在于提供可重复的测试数据。
 */
class FeedSimulator {
public:
    /**
     * @param symbol     品种代码
     * @param base_price 基准价格（整数定价，单位：原始 × 1e6）
     * @param seed       随机种子，保证可重复性
     */
    FeedSimulator(std::string_view symbol,
                  int64_t          base_price,
                  double           cancel_ratio = 0.2,
                  uint64_t         seed         = 42);

    /// 随机生成一批订单（用于压测）
    std::vector<Order> generate_random(size_t count);

    /// 零拷贝接口：直接将随机订单字段写入调用方提供的槽位（已从内存池 allocate 的指针）
    /// 避免构造临时 Order 再拷贝，适合 producer_loop 热路径使用
    void generate_into(Order* slot);

    /// 从 CSV 文件加载订单序列
    /// CSV 格式：order_id,side,price,qty,type
    /// side: 0=BUY 1=SELL, type: 0=LIMIT 1=CANCEL
    std::vector<Order> load_csv(const std::string& filepath);

private:
    Order make_random_order(bool is_cancel = false);

    std::string symbol_;
    int64_t     base_price_;
    uint64_t    next_order_id_ = 1;

    std::mt19937_64                    rng_;
    std::normal_distribution<double>   price_dist_;   // 价格围绕 base_price 正态分布
    std::uniform_int_distribution<int> qty_dist_;     // 数量 [1, 100]
    std::bernoulli_distribution        side_dist_;    // 50% BUY / 50% SELL
    std::bernoulli_distribution        cancel_dist_;  // 撤单概率
};

} // namespace me
