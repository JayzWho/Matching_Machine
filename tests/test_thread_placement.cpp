/**
 * thread_placement.h: PinnedThread runs only on its CPU from creation and otherwise
 * behaves like std::thread; allowed_cpu_pair() and ScopedPin.
 */

#include "thread_placement.h"
#include <gtest/gtest.h>

#include <system_error>
#include <vector>

using namespace me;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static cpu_set_t own_mask() {
    cpu_set_t set;
    CPU_ZERO(&set);
    EXPECT_EQ(sched_getaffinity(0, sizeof(set), &set), 0);
    return set;
}

static std::vector<int> cpus_in(const cpu_set_t& set) {
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
    return cpus;
}

static cpu_set_t mask_of(std::initializer_list<int> cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus) CPU_SET(cpu, &set);
    return set;
}

// Runs the test thread under the given CPU mask until destroyed.
class ScopedMask {
public:
    explicit ScopedMask(const cpu_set_t& set) : saved_(own_mask()) {
        EXPECT_EQ(sched_setaffinity(0, sizeof(set), &set), 0);
    }
    ~ScopedMask() { sched_setaffinity(0, sizeof(saved_), &saved_); }

private:
    cpu_set_t saved_;
};

struct Seen {
    int cpu = -1;
    cpu_set_t mask{};
};

static Seen run_and_watch(int cpu) {
    Seen seen;
    PinnedThread thread(cpu, [&seen] {
        seen.cpu = sched_getcpu();
        pthread_getaffinity_np(pthread_self(), sizeof(seen.mask), &seen.mask);
    });
    thread.join();
    return seen;
}

// ─── Placement ───────────────────────────────────────────────────────────────

TEST(PinnedThreadTest, RunsOnlyOnItsCpu) {
    for (int cpu : cpus_in(own_mask())) {
        const Seen seen = run_and_watch(cpu);
        EXPECT_EQ(seen.cpu, cpu);
        EXPECT_EQ(CPU_COUNT(&seen.mask), 1);
        EXPECT_TRUE(CPU_ISSET(cpu, &seen.mask));
    }
}

TEST(PinnedThreadTest, UnpinnedThreadInheritsCreatorMask) {
    const cpu_set_t creator = own_mask();
    const Seen seen = run_and_watch(-1);
    EXPECT_TRUE(CPU_EQUAL(&seen.mask, &creator));
}

// ─── std::thread semantics ───────────────────────────────────────────────────

TEST(PinnedThreadTest, JoinableLikeStdThread) {
    PinnedThread empty;
    EXPECT_FALSE(empty.joinable());

    int ran = 0;
    PinnedThread thread(-1, [&ran] { ran = 1; });
    EXPECT_TRUE(thread.joinable());

    PinnedThread moved(std::move(thread));
    EXPECT_FALSE(thread.joinable());
    EXPECT_TRUE(moved.joinable());

    moved.join();
    EXPECT_FALSE(moved.joinable());
    EXPECT_EQ(ran, 1);
}

TEST(PinnedThreadTest, UnavailableCpuThrows) {
    const int outside = CPU_SETSIZE - 1;  // no such CPU on this machine
    const cpu_set_t mask = own_mask();
    ASSERT_FALSE(CPU_ISSET(outside, &mask));
    bool ran = false;
    EXPECT_THROW(PinnedThread(outside, [&ran] { ran = true; }), std::system_error);
    EXPECT_FALSE(ran);
}

// ─── 3. allowed_cpu_pair ─────────────────────────────────────────────────────

TEST(AllowedCpuPairTest, ExactlyTwoCpusGiveLowerAndHigher) {
    const std::vector<int> cpus = cpus_in(own_mask());
    if (cpus.size() < 2) GTEST_SKIP() << "needs two CPUs";
    const int lower = cpus.front(), higher = cpus.back();
    ScopedMask two(mask_of({higher, lower}));
    const ThreadCpus pair = allowed_cpu_pair();
    EXPECT_EQ(pair.producer, lower);
    EXPECT_EQ(pair.consumer, higher);
}

TEST(AllowedCpuPairTest, OtherCountsLeaveThreadsUnpinned) {
    const std::vector<int> cpus = cpus_in(own_mask());
    {
        ScopedMask one(mask_of({cpus.front()}));
        const ThreadCpus pair = allowed_cpu_pair();
        EXPECT_EQ(pair.producer, -1);
        EXPECT_EQ(pair.consumer, -1);
    }
    if (cpus.size() >= 3) {
        ScopedMask three(mask_of({cpus[0], cpus[1], cpus[2]}));
        const ThreadCpus pair = allowed_cpu_pair();
        EXPECT_EQ(pair.producer, -1);
        EXPECT_EQ(pair.consumer, -1);
    }
}

// ─── 4. ScopedPin ────────────────────────────────────────────────────────────

TEST(ScopedPinTest, PinsCallingThreadAndRestoresItsMask) {
    const cpu_set_t before = own_mask();
    const int cpu = cpus_in(before).back();
    {
        ScopedPin pin(cpu);
        const cpu_set_t during = own_mask();
        EXPECT_EQ(CPU_COUNT(&during), 1);
        EXPECT_TRUE(CPU_ISSET(cpu, &during));
        EXPECT_EQ(sched_getcpu(), cpu);
    }
    const cpu_set_t after = own_mask();
    EXPECT_TRUE(CPU_EQUAL(&before, &after));
}

TEST(ScopedPinTest, NegativeCpuLeavesMaskAlone) {
    const cpu_set_t before = own_mask();
    {
        ScopedPin pin(-1);
        const cpu_set_t during = own_mask();
        EXPECT_TRUE(CPU_EQUAL(&before, &during));
    }
    const cpu_set_t after = own_mask();
    EXPECT_TRUE(CPU_EQUAL(&before, &after));
}

TEST(ScopedPinTest, UnavailableCpuThrowsAndLeavesMaskAlone) {
    const int outside = CPU_SETSIZE - 1;  // no such CPU on this machine
    const cpu_set_t before = own_mask();
    ASSERT_FALSE(CPU_ISSET(outside, &before));
    EXPECT_THROW(ScopedPin pin(outside), std::system_error);
    const cpu_set_t after = own_mask();
    EXPECT_TRUE(CPU_EQUAL(&before, &after));
}
