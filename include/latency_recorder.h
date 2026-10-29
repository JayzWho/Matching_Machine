#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdio>

#if defined(__x86_64__) || defined(__i386__)
#  include <x86intrin.h>   // __rdtsc
#endif

namespace me {

/**
 * @brief 高精度延迟记录器
 *
 * 使用 RDTSC（Read Time-Stamp Counter）指令读取处理器的时间戳计数器（TSC），
 * 测量代码段的执行延迟。读数的单位是 TSC ticks。
 *
 * 使用方式：
 *   LatencyRecorder rec(10000);   // 预分配 10000 个采样槽
 *
 *   for (...) {
 *       auto t0 = LatencyRecorder::now_serialized();
 *       // ... 被测代码 ...
 *       auto t1 = LatencyRecorder::now_serialized();
 *       rec.record(t1 - t0);
 *   }
 *
 *   rec.report("service");   // 输出 P50/P95/P99/P999/Max
 *
 * 注意事项：
 *   - TSC 按固定频率计数，与核心当前的频率无关：ticks 不是核心时钟周期，
 *     换算成时间要用标定出的计数器频率（录制器写在 env.json 里），不能用核心频率
 *   - 两个读数来自不同的核时（如排队时间），差值依赖各核计数器同步；
 *     内核选 tsc 作时钟源，说明它在启动时检查过同步
 *   - 用 Release 模式编译，否则编译器不优化，数据无参考价值
 */
class LatencyRecorder {
public:
    explicit LatencyRecorder(size_t reserve_count = 100'000) {
        samples_.reserve(reserve_count);
    }

    /**
     * @brief 读取 TSC，不加序列化
     *
     * RDTSC：Read Time-Stamp Counter。计数器从处理器复位起按固定频率递增。
     * 订单簿用它给没有时间戳的订单补一个；延迟的测量点用 now_serialized()。
     *
     * @return 当前的 TSC 读数（ticks）
     */
    [[nodiscard]] static uint64_t now() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        return __rdtsc();
#else
        // 非 x86 平台降级到 clock_gettime（纳秒，精度稍低）
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
#endif
    }

    /**
     * @brief 读取 TSC，读数与前后的指令保持顺序（延迟的测量点用这个）
     *
     * RDTSC 不等前面的指令执行完，后面的指令也可能先于它执行，读数会落在被测代码中间。
     * 前一条 LFENCE 让之前的指令都在本核执行完才读计数器，后一条让之后的指令
     * 等读数完成才开始；"memory" 让编译器不把内存访问移过这次读取。
     * 每次多花几十个周期，所以只用在测量点上，now() 保持原样。
     */
    [[nodiscard]] static uint64_t now_serialized() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        uint32_t lo, hi;
        asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) : : "memory");
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return now();
#endif
    }

    /**
     * @brief 记录一次延迟采样（TSC ticks）
     */
    void record(uint64_t ticks) {
        samples_.push_back(ticks);
    }

    /**
     * @brief 清空所有采样
     */
    void reset() {
        samples_.clear();
    }

    /**
     * @brief 当前采样数量
     */
    [[nodiscard]] size_t count() const noexcept { return samples_.size(); }

    /**
     * @brief 计算指定百分位的延迟（TSC ticks）
     * @param percentile 0.0 - 1.0，如 0.99 表示 P99
     * @return 对应百分位的 TSC ticks，samples_ 为空时返回 0
     */
    [[nodiscard]] uint64_t percentile(double p) {
        if (samples_.empty()) return 0;
        std::sort(samples_.begin(), samples_.end());
        const size_t idx = static_cast<size_t>(p * (samples_.size() - 1));
        return samples_[idx];
    }

    /**
     * @brief 输出完整的百分位报告，单位 TSC ticks，不换算成时间
     * @param quantity 这组延迟是什么（如 "queue"、"service"），写进标题
     *
     * 示例输出：
     *   === Latency Report: service (10000 samples, TSC ticks) ===
     *   P50  :    245 ticks
     *   P95  :    389 ticks
     *   P99  :    612 ticks
     *   P999 :   1820 ticks
     *   Max  :   4231 ticks
     */
    void report(const char* quantity) {
        if (samples_.empty()) {
            std::printf("=== Latency Report: %s, no samples ===\n", quantity);
            return;
        }

        std::sort(samples_.begin(), samples_.end());

        const uint64_t p50  = samples_[static_cast<size_t>(0.50 * (samples_.size() - 1))];
        const uint64_t p95  = samples_[static_cast<size_t>(0.95 * (samples_.size() - 1))];
        const uint64_t p99  = samples_[static_cast<size_t>(0.99 * (samples_.size() - 1))];
        const uint64_t p999 = samples_[static_cast<size_t>(0.999 * (samples_.size() - 1))];
        const uint64_t pmax = samples_.back();

        std::printf("=== Latency Report: %s (%zu samples, TSC ticks) ===\n", quantity, samples_.size());
        std::printf("P50  : %6lu ticks\n", p50);
        std::printf("P95  : %6lu ticks\n", p95);
        std::printf("P99  : %6lu ticks\n", p99);
        std::printf("P999 : %6lu ticks\n", p999);
        std::printf("Max  : %6lu ticks\n", pmax);
    }

private:
    std::vector<uint64_t> samples_;
};

} // namespace me
