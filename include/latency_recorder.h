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
 * Collects latency samples in TSC ticks and reports their percentiles.
 *
 * The TSC counts at a fixed rate independent of the core's current frequency, so ticks
 * are not core cycles; converting them to time needs the calibrated TSC frequency that
 * the recorder stores in env.json. A difference between readings from two cores, such
 * as queue time, also relies on the cores' counters being synchronized, which the kernel
 * checks at boot before it uses the TSC as its clock source.
 */
class LatencyRecorder {
public:
    explicit LatencyRecorder(size_t reserve_count = 100'000) {
        samples_.reserve(reserve_count);
    }

    /// Unordered TSC read: it may drift into the surrounding code. Fine for stamping an
    /// order; measurement points use now_serialized().
    [[nodiscard]] static uint64_t now() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        return __rdtsc();
#else
        // Elsewhere, nanoseconds from CLOCK_MONOTONIC.
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
#endif
    }

    /// TSC read that stays in place: the first LFENCE waits for earlier instructions to
    /// complete, the second keeps later ones from starting before the read, and the
    /// "memory" clobber stops the compiler from moving memory accesses across it. Costs
    /// tens of cycles per read.
    [[nodiscard]] static uint64_t now_serialized() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        uint32_t lo, hi;
        asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) : : "memory");
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return now();
#endif
    }

    void record(uint64_t ticks) {
        samples_.push_back(ticks);
    }

    void reset() {
        samples_.clear();
    }

    [[nodiscard]] size_t count() const noexcept { return samples_.size(); }

    /// `p` in [0, 1]; returns 0 when there are no samples. Sorts the samples.
    [[nodiscard]] uint64_t percentile(double p) {
        if (samples_.empty()) return 0;
        std::sort(samples_.begin(), samples_.end());
        const size_t idx = static_cast<size_t>(p * (samples_.size() - 1));
        return samples_[idx];
    }

    /// Prints P50, P95, P99, P99.9 and the maximum in TSC ticks under a title naming
    /// `quantity`. The measurement scripts parse this output.
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
