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

/// 两个工作线程各自钉在哪个 CPU 上；-1 表示不钉，由调度器摆放（原来的行为）。
struct ThreadCpus {
    int producer = -1;
    int consumer = -1;
};

/**
 * @brief 调用线程能用的 CPU 正好两个时，返回 {编号小的, 编号大的}，用作 {生产者, 消费者}
 *
 * 录制器用 taskset 把双线程基准限定在两个测量核上，基准由此得知该把两个线程放在哪里。
 * 能用的 CPU 不是正好两个时返回 {-1, -1}：不钉核，与原来相同。
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
 * @brief 创建时就钉在指定 CPU 上的线程，用法同 std::thread
 *
 * std::thread 不能在创建时指定 CPU。先创建、再用 pthread_setaffinity_np 改亲和性，
 * 新线程会先在创建者所在的核上起步，之后才被迁走。这里在 pthread_create 之前用
 * pthread_attr_setaffinity_np 设好亲和性，线程从第一条指令起就在目标 CPU 上。
 * cpu < 0 时不设亲和性，与 std::thread 相同：继承创建者的 CPU 掩码。
 *
 * 只保存一个 pthread_t，大小与 std::thread 相同（见文件末尾的断言），
 * 替换 std::thread 成员后，所在对象的内存布局不变。
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
        state.release();  // 新线程接管，在 run() 结束时释放
    }

    // 与 std::thread 相同：仍可 join 的线程被析构或覆盖，直接终止进程
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

    // 0 表示没有线程：glibc 的 pthread_t 是线程描述符的地址，不会是 0，
    // libstdc++ 的 std::thread 也用 0 表示"不可 join"
    pthread_t id_ = 0;
};

static_assert(sizeof(PinnedThread) == sizeof(std::thread),
              "PinnedThread must keep the layout of the objects whose std::thread members it replaces");

} // namespace me
