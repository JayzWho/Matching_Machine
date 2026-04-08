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
 * 使用 RDTSC（Read Time-Stamp Counter）指令读取 CPU 时钟周期计数，
 * 测量代码段的执行延迟。
 *
 * 使用方式：
 *   LatencyRecorder rec(10000);   // 预分配 10000 个采样槽
 *
 *   for (...) {
 *       auto t0 = LatencyRecorder::now();
 *       // ... 被测代码 ...
 *       auto t1 = LatencyRecorder::now();
 *       rec.record(t1 - t0);
 *   }
 *
 *   rec.report();   // 输出 P50/P95/P99/P999/Max
 *
 * 注意事项（见笔记 06_latency_measurement.md）：
 *   - 必须绑核（taskset -c 0）后使用，否则跨核 TSC 不同步
 *   - 测量结果是 CPU 时钟周期，转换为纳秒需要除以 CPU 频率（GHz）
 *   - 用 Release 模式编译，否则编译器不优化，数据无参考价值
 */
class LatencyRecorder {
public:
    explicit LatencyRecorder(size_t reserve_count = 100'000) {
        samples_.reserve(reserve_count);
    }

    /**
     * @brief 读取当前 CPU 时间戳（时钟周期数）
     *
     * RDTSC：Read Time-Stamp Counter
     * 每个 CPU 核心有一个单调递增的 64 位计数器，
     * 从 CPU 启动时开始计数，每个时钟周期加 1。
     *
     * @return 当前时钟周期数
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
     * @brief 记录一次延迟采样（时钟周期数）
     */
    void record(uint64_t cycles) {
        samples_.push_back(cycles);
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
     * @brief 计算指定百分位的延迟（时钟周期数）
     * @param percentile 0.0 - 1.0，如 0.99 表示 P99
     * @return 对应百分位的时钟周期数，samples_ 为空时返回 0
     */
    [[nodiscard]] uint64_t percentile(double p) {
        if (samples_.empty()) return 0;
        std::sort(samples_.begin(), samples_.end());
        const size_t idx = static_cast<size_t>(p * (samples_.size() - 1));
        return samples_[idx];
    }

    /**
     * @brief 输出完整的百分位报告
     * @param cpu_ghz CPU 频率（GHz），用于将时钟周期转换为纳秒
     *                如果为 0，只输出时钟周期数
     *
     * 示例输出：
     *   === Latency Report (10000 samples) ===
     *   P50  :    245 cycles /    82 ns
     *   P95  :    389 cycles /   130 ns
     *   P99  :    612 cycles /   204 ns
     *   P999 :   1820 cycles /   607 ns
     *   Max  :   4231 cycles /  1410 ns
     */
    void report(double cpu_ghz = 0.0) {
        if (samples_.empty()) {
            std::printf("=== Latency Report: no samples ===\n");
            return;
        }

        std::sort(samples_.begin(), samples_.end());

        const uint64_t p50  = samples_[static_cast<size_t>(0.50 * (samples_.size() - 1))];
        const uint64_t p95  = samples_[static_cast<size_t>(0.95 * (samples_.size() - 1))];
        const uint64_t p99  = samples_[static_cast<size_t>(0.99 * (samples_.size() - 1))];
        const uint64_t p999 = samples_[static_cast<size_t>(0.999 * (samples_.size() - 1))];
        const uint64_t pmax = samples_.back();

        std::printf("=== Latency Report (%zu samples) ===\n", samples_.size());

        if (cpu_ghz > 0.0) {
            auto to_ns = [cpu_ghz](uint64_t cycles) -> double {
                return static_cast<double>(cycles) / cpu_ghz;
            };
            std::printf("P50  : %6lu cycles / %6.0f ns\n", p50,  to_ns(p50));
            std::printf("P95  : %6lu cycles / %6.0f ns\n", p95,  to_ns(p95));
            std::printf("P99  : %6lu cycles / %6.0f ns\n", p99,  to_ns(p99));
            std::printf("P999 : %6lu cycles / %6.0f ns\n", p999, to_ns(p999));
            std::printf("Max  : %6lu cycles / %6.0f ns\n", pmax, to_ns(pmax));
        } else {
            std::printf("P50  : %6lu cycles\n", p50);
            std::printf("P95  : %6lu cycles\n", p95);
            std::printf("P99  : %6lu cycles\n", p99);
            std::printf("P999 : %6lu cycles\n", p999);
            std::printf("Max  : %6lu cycles\n", pmax);
        }
    }

private:
    std::vector<uint64_t> samples_;
};

} // namespace me
