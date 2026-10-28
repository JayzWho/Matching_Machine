// tsc_calibrate: measure the frequency of the timestamp counter (TSC) against
// CLOCK_MONOTONIC and print it as one JSON object. capture_env.sh records it,
// so that TSC ticks in a study can be converted to time with the frequency of
// the machine and boot that recorded them, rather than an assumed one.
//
// Each interval reads the counter and the clock as a pair at its start and its
// end, sleeping in between. A pair brackets the clock read with two counter
// reads and keeps the attempt with the narrowest bracket, so the pair is known
// to within that many ticks. Counter reads are ordered with LFENCE on both
// sides. The frequency is the median over the intervals; the lowest and highest
// show how well the intervals agree.
//
// Built with the benchmarks; needs no privilege. Usage: tsc_calibrate

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#  include <x86intrin.h>
#endif

namespace {

constexpr int kIntervals = 5;
constexpr long kIntervalMs = 200;
constexpr int kAttempts = 16;

#if defined(__x86_64__) || defined(__i386__)

std::uint64_t ordered_rdtsc() {
    _mm_lfence();
    const std::uint64_t ticks = __rdtsc();
    _mm_lfence();
    return ticks;
}

std::int64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

struct Pair {
    std::int64_t ns;
    double ticks;            // middle of the bracket
    std::uint64_t bracket;   // width of the bracket in ticks
};

Pair read_pair() {
    Pair best{0, 0.0, UINT64_MAX};
    for (int i = 0; i < kAttempts; ++i) {
        const std::uint64_t before = ordered_rdtsc();
        const std::int64_t ns = monotonic_ns();
        const std::uint64_t after = ordered_rdtsc();
        if (after - before < best.bracket)
            best = {ns, (static_cast<double>(before) + static_cast<double>(after)) / 2.0, after - before};
    }
    return best;
}

#endif

}  // namespace

int main() {
#if defined(__x86_64__) || defined(__i386__)
    std::vector<double> ghz;
    std::uint64_t widest = 0;
    for (int i = 0; i < kIntervals; ++i) {
        const Pair start = read_pair();
        const timespec pause{0, kIntervalMs * 1'000'000};
        nanosleep(&pause, nullptr);
        const Pair end = read_pair();
        ghz.push_back((end.ticks - start.ticks) / static_cast<double>(end.ns - start.ns));
        widest = std::max({widest, start.bracket, end.bracket});
    }
    std::sort(ghz.begin(), ghz.end());
    std::printf("{\"ghz\": %.6f, \"ghz_min\": %.6f, \"ghz_max\": %.6f, \"intervals\": %d, "
                "\"interval_ms\": %ld, \"widest_pair_ticks\": %llu, \"reference\": \"CLOCK_MONOTONIC\"}\n",
                ghz[ghz.size() / 2], ghz.front(), ghz.back(), kIntervals, kIntervalMs,
                static_cast<unsigned long long>(widest));
    return 0;
#else
    std::printf("{\"ghz\": null, \"reference\": \"CLOCK_MONOTONIC\", \"error\": \"no TSC on this architecture\"}\n");
    return 1;
#endif
}
