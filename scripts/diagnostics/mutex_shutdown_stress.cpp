// Runs the mutex pipeline of a given bench_baseline_mutex.cpp back to back and
// reports a run that stops making progress, which is how a lost wake-up at the
// producer's completion shows. The engine class is used unchanged.
//
// Build with build_mutex_shutdown_stress.sh. Usage:
//   mutex_shutdown_stress ORDERS SECONDS
// Prints "OK" and exits 0 when no run stalled for two seconds within SECONDS,
// otherwise prints "HANG" and exits 2.
#include BENCH_SOURCE

#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s ORDERS SECONDS\n", argv[0]);
        return 1;
    }
    const size_t orders = std::strtoul(argv[1], nullptr, 10);
    const long seconds = std::strtol(argv[2], nullptr, 10);
    std::atomic<unsigned long> runs{0};
    std::thread watchdog([&] {
        const auto start = std::chrono::steady_clock::now();
        unsigned long last = ~0UL;
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const unsigned long now = runs.load();
            if (now == last) {
                std::printf("orders=%zu HANG after %lu completed runs\n", orders, now);
                std::fflush(stdout);
                std::_Exit(2);
            }
            last = now;
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(seconds)) {
                std::printf("orders=%zu OK %lu runs in %ld s\n", orders, now, seconds);
                std::fflush(stdout);
                std::_Exit(0);
            }
        }
    });
    while (true) {
        MutexMatchingEngine engine;
        engine.start("BTCUSD", 100'000'000LL, orders, 0.2);
        engine.stop();
        runs.fetch_add(1);
    }
}
