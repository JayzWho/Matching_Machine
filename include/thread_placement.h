#pragma once

#include <pthread.h>
#include <sched.h>

#include <exception>
#include <memory>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace me {

/// CPUs for the two pipeline threads; -1 leaves a thread to the scheduler.
struct ThreadCpus {
    int producer = -1;
    int consumer = -1;
};

/**
 * When the calling thread may run on exactly two CPUs, returns {lower, higher} as
 * {producer, consumer}; otherwise {-1, -1}. The recorder confines each two-threaded
 * benchmark to the two measurement cores with taskset, and this is how the benchmark
 * learns which they are.
 */
inline ThreadCpus allowed_cpu_pair() noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0 || CPU_COUNT(&set) != 2) return {};
    ThreadCpus cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &set)) continue;
        if (cpus.producer < 0) {
            cpus.producer = cpu;
        } else {
            cpus.consumer = cpu;
            break;
        }
    }
    return cpus;
}

/**
 * A thread pinned to a CPU from its first instruction. Setting the affinity after
 * std::thread starts lets the thread begin on its creator's core and migrate later, so
 * the affinity is set on the pthread attributes before pthread_create. With cpu < 0 it
 * behaves like std::thread and inherits the creator's CPU mask. It has the size of
 * std::thread so that replacing one keeps the enclosing object's layout.
 */
class PinnedThread {
public:
    PinnedThread() noexcept = default;

    template <class F>
    PinnedThread(int cpu, F&& body) {
        using Body = std::decay_t<F>;
        auto state = std::make_unique<State<Body>>(std::forward<F>(body));
        pthread_attr_t attr;
        int err = pthread_attr_init(&attr);
        if (err != 0) throw std::system_error(err, std::generic_category(), "pthread_attr_init");
        if (cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            err = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
        }
        if (err == 0) err = pthread_create(&id_, &attr, &run<Body>, state.get());
        pthread_attr_destroy(&attr);
        if (err != 0) {
            id_ = 0;
            throw std::system_error(err, std::generic_category(), "PinnedThread");
        }
        state.release();  // owned by the new thread from here; run() frees it
    }

    // As with std::thread, destroying or overwriting a joinable thread terminates.
    ~PinnedThread() {
        if (joinable()) std::terminate();
    }

    PinnedThread(const PinnedThread&) = delete;
    PinnedThread& operator=(const PinnedThread&) = delete;

    PinnedThread(PinnedThread&& other) noexcept : id_(std::exchange(other.id_, 0)) {}

    PinnedThread& operator=(PinnedThread&& other) noexcept {
        if (joinable()) std::terminate();
        id_ = std::exchange(other.id_, 0);
        return *this;
    }

    [[nodiscard]] bool joinable() const noexcept { return id_ != 0; }

    void join() {
        const int err = pthread_join(id_, nullptr);
        if (err != 0) throw std::system_error(err, std::generic_category(), "PinnedThread::join");
        id_ = 0;
    }

private:
    template <class Body>
    struct State {
        template <class G>
        explicit State(G&& g) : body(std::forward<G>(g)) {}
        Body body;
    };

    template <class Body>
    static void* run(void* arg) {
        std::unique_ptr<State<Body>> state(static_cast<State<Body>*>(arg));
        state->body();
        return nullptr;
    }

    // 0 means no thread: glibc's pthread_t is a descriptor address and never 0, and
    // libstdc++'s std::thread uses 0 the same way.
    pthread_t id_ = 0;
};

static_assert(sizeof(PinnedThread) == sizeof(std::thread),
              "PinnedThread must keep the layout of the objects whose std::thread members it replaces");

/**
 * Pins the calling thread to one CPU and restores its previous mask on destruction. For
 * benchmarks whose main thread is the producer: restoring the mask lets the next
 * benchmark's allowed_cpu_pair() see both CPUs again. Does nothing for cpu < 0.
 */
class ScopedPin {
public:
    explicit ScopedPin(int cpu) {
        if (cpu < 0) return;
        int err = pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_);
        if (err == 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        }
        if (err != 0) throw std::system_error(err, std::generic_category(), "ScopedPin");
        pinned_ = true;
    }

    ~ScopedPin() {
        if (pinned_) pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_);
    }

    ScopedPin(const ScopedPin&) = delete;
    ScopedPin& operator=(const ScopedPin&) = delete;

private:
    cpu_set_t saved_{};
    bool pinned_ = false;
};

} // namespace me
