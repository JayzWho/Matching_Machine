#!/usr/bin/env python3
"""Build the diagnostic program of the queue-mechanism study.

bench_queue_mechanism is generated from the unchanged
benchmarks/bench_spsc_ring_buffer.cpp and include/spsc_ring_buffer.h by exact
text replacements, each of which must match the expected number of times, and
is compiled with the flags and link inputs of the Release tree. Every added or
changed line ends in "// DIAG". It asks why the int64 ring runs in one of two
states, the producer ahead (8-9 ns per item) or the consumer caught up (57-62
ns), and what selects the state.

Memory. At start-up the program creates one memory file. Its first 64 pages
are control pages: the first page of a ring, which holds head_ (line 0),
tail_ (line 1) and the first 496 slots. Data sets follow: the 128 pages after
the control page of an int64 ring (d0 and d1), and the pages of a ring of
padded items and of an Order ring. A ring is mapped as one control page
followed by one data set, so rings can share a control page and differ in
their data pages, or the reverse. Ring j of every variant uses control page j.

Every ring benchmark pins the producer (the main thread) to the CPU given by
its last argument (0 the lower, 1 the higher of the launch's two CPUs) and
creates the consumer on the other CPU; its consumer sums into a local and N
is captured by value. Arguments: items, ring, producer CPU.

BM_S0        the original loops, no instrumentation on any path
BM_S1        retry counts on the failure paths only (the base variant);
             control pages 0-63, all others 0-7
BM_S2        BM_S1 with _mm_pause() after every failed pop
BM_S0C, BM_S1C, BM_S2C   the same with hardware counters per thread; the
             consumer starts first, because opening the counters would
             delay it and lengthen the producer's lead, and the producer's
             counters cover its push loop only
BM_S1Fwd     BM_S1 with sampled loads that another core's cache served, with
             their data address, per thread; started like BM_S1C
BM_TraceAsIs, BM_TraceProducerFirst, BM_TraceConsumerFirst
             queue depth after the first item and every 1000 items, with
             three starts: as the benchmark starts; the producer pushes 16384
             items before the consumer may pop; the consumer spins on the
             empty queue before the producer starts
BM_Hist      every 16th item, the depth the consumer sees and whether the
             producer is writing the line the consumer reads
BM_LagCtl, BM_Lag0, BM_Lag4, BM_Lag16, BM_Lag64
             the consumer pops a slot only when the producer has finished the
             slot's line and K further lines (K = 0, 4, 16, 64); LagCtl runs
             the same code with the condition "depth >= 1"; the producer
             releases the rest when it has pushed its last item
BM_Padded    int64 items padded to a cache line each, same capacity
BM_DataSwap  control page j with data set d1 instead of d0
BM_Alias     the pages of BM_S1's ring j at a second virtual address
BM_Anon      a ring in an anonymous mapping, as in the queue-regimes study
BM_OrderS1, BM_OrderS1C  the Order ring on control page j, retry counts,
             and with hardware counters per thread, started like BM_S1C
BM_OneWay    one-way handoffs of line 0 or 1 of control page j in one
             direction, in TSC ticks; arguments: page, line, writer CPU

Run as root, the program also prints the physical page of every page of the
memory file and, for lines 0-3 of every control page, the LLC slice that
serves it, from uncore_cbox_0-3 lookup counts.

Hypotheses and criteria, fixed before recording. A cell is one ring in one
producer orientation; it is slow above 20 ns per item.
1. Shared data lines amplify the caught-up state. In cells slow under
   BM_LagCtl, BM_Lag0 cuts at least half of the gap to the fast level while
   the consumer still waits before at least 1% of items: the main cause holds.
   Only BM_Lag16 or BM_Lag64 doing so: the contention reaches wider than one
   line. No K cutting a quarter of the gap: the cause is not the data line.
   In BM_Hist the share of items whose line the producer is still writing is
   higher in slow cells than in fast ones; in BM_S1Fwd the consumer's
   forwarded data loads fall on all eight slots of a line in slow cells and
   on the first slot in fast cells.
2. One state per cell, or two that sustain themselves. A start that changes
   a cell's state for the whole iteration (depth trace) means both states
   sustain themselves; no change means one stable state per cell.
3. The control page sets the state: BM_DataSwap and BM_Alias agree with BM_S1
   in at least 95% of cells, and BM_Anon has a similar share of slow cells.
4. One-way handoff times of lines 0 and 1, both directions, predict the state
   of at least 90% of BM_S1's cells with one rule; with root, the slices of
   lines 0 and 1 and the orientation predict it.
5. Counters per thread show which side's cycles grow in the slow state and
   whether load-miss stalls, a full store buffer or recovery from machine
   clears account for them; the spin style changes the slow level or not.
6. Added after the wiring check, which showed slow cells at about 42 ns when
   the consumer started late: the slow level is an average over a prefix in
   which the producer leads and the caught-up remainder, so in slow cells it
   falls as the producer's lead at the start grows (consumer first > as is >
   producer first), while fast cells keep their level.

The generated sources, commands and hashes are written beside the binary in
build/diagnostics/mechanism/. Requires an already built CMake Unix Makefiles
Release tree.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def patch(text, old, new, count=1):
    found = text.count(old)
    if found != count:
        raise SystemExit(f"expected {count} occurrence(s), found {found}:\n{old}")
    return text.replace(old, new)


def segment(text, start, end):
    """Split text into before, the part from start up to end, and after."""
    if text.count(start) != 1:
        raise SystemExit(f"segment start not unique: {start}")
    head, rest = text.split(start, 1)
    if end not in rest:
        raise SystemExit(f"segment end not found: {end}")
    body, tail = rest.split(end, 1)
    return head, start + body, end + tail


HEADER = r'''// Generated by scripts/diagnostics/build_queue_mechanism.py.
// Diagnostics of the queue-mechanism study, not part of the benchmarks: rings
// built from chosen pages of one memory file, pinned threads, start modes,
// hardware counters per thread, sampled cross-core loads, queue-depth traces,
// one-way cache-line timing and, run as root, physical pages and LLC slices.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <fcntl.h>
#include <immintrin.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>
#include <benchmark/benchmark.h>
#include "spsc_ring_buffer.h"
#include "mm_diag_lag_ring.h"

namespace mm_mech {

inline constexpr std::size_t kPage = 4096;
inline constexpr int kMain = 8;   // control pages every ring variant runs on
inline constexpr int kWide = 64;  // control pages of BM_S1 and BM_OneWay

// The memory file: kWide control pages, then the data sets.
enum Set { kD0, kD1, kPaddedSet, kOrderSet, kSets };
inline constexpr std::size_t kSetPages[kSets] = {128, 128, 1024, 256};
inline const char* const kSetNames[kSets] = {"d0", "d1", "padded", "order"};

// Where a variant builds ring j: control page j and a data set, the pages of
// kComposite j at a second address, or an anonymous region of its own.
enum Memory { kComposite, kDataSwap, kAlias, kAnon, kPadded, kOrder, kMemories };
inline const char* const kMemoryNames[kMemories] = {"composite", "data_swap", "alias", "anon", "padded", "order"};
inline constexpr Set kMemorySet[kMemories] = {kD0, kD1, kD0, kD0, kPaddedSet, kOrderSet};
inline constexpr std::size_t kAnonPages = 129;
inline constexpr std::size_t kOrderSourcePages = 160;

inline int g_fd = -1;
inline std::byte* g_pool = nullptr;
inline std::size_t g_pool_pages = 0;
inline std::size_t g_set_first[kSets];
inline std::byte* g_ring[kMemories][kWide];
inline std::byte* g_ack = nullptr;           // the line a one-way reader answers on
inline std::byte* g_order_source = nullptr;  // the Order benchmark's source array

[[noreturn]] inline void fail(const char* what) {
    std::perror(what);
    std::abort();
}

inline std::byte* anonymous(std::size_t pages) {
    void* p = mmap(nullptr, pages * kPage, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) fail("mmap anonymous");
    return static_cast<std::byte*>(p);
}

inline void map_pages(std::byte* at, std::size_t first, std::size_t pages) {
    void* p = mmap(at, pages * kPage, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_POPULATE, g_fd,
                   static_cast<off_t>(first * kPage));
    if (p != at) fail("mmap memory file");
}

// Control page j followed by the pages of one data set, at a new address.
inline std::byte* composite(int j, Set set) {
    void* p = mmap(nullptr, (1 + kSetPages[set]) * kPage, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fail("mmap reserve");
    auto* base = static_cast<std::byte*>(p);
    map_pages(base, static_cast<std::size_t>(j), 1);
    map_pages(base + kPage, g_set_first[set], kSetPages[set]);
    return base;
}

inline long perf_open(perf_event_attr* attr, pid_t pid, int cpu, int group, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group, flags);
}

inline std::uint64_t pfn(const void* address) {
    static const int fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    std::uint64_t entry = 0;
    const auto at = static_cast<off_t>(reinterpret_cast<std::uintptr_t>(address) / kPage * sizeof(entry));
    if (fd < 0 || pread(fd, &entry, sizeof(entry), at) != static_cast<ssize_t>(sizeof(entry))) return 0;
    return entry >> 63 ? entry & ((std::uint64_t{1} << 55) - 1) : 0;
}

inline long read_number(const char* path) {
    long value = -1;
    if (FILE* f = std::fopen(path, "r")) {
        if (std::fscanf(f, "%ld", &value) != 1) value = -1;
        std::fclose(f);
    }
    return value;
}

// LLC lookups per slice, counted by uncore_cbox_0-3 while one line is flushed
// and loaded again; the slice that serves the line takes the lookups.
class Slices {
public:
    static constexpr int kBoxes = 4;
    bool open(std::uint64_t config) {
        for (int b = 0; b < kBoxes; ++b) {
            char path[96];
            std::snprintf(path, sizeof path, "/sys/bus/event_source/devices/uncore_cbox_%d/type", b);
            const long type = read_number(path);
            std::snprintf(path, sizeof path, "/sys/bus/event_source/devices/uncore_cbox_%d/cpumask", b);
            const long cpu = read_number(path);
            if (type < 0 || cpu < 0) {
                error_ = ENOENT;
                return false;
            }
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = static_cast<std::uint32_t>(type);
            attr.config = config;
            attr.disabled = 1;
            fd_[b] = static_cast<int>(perf_open(&attr, -1, static_cast<int>(cpu), -1, PERF_FLAG_FD_CLOEXEC));
            if (fd_[b] < 0) {
                error_ = errno;
                close();
                return false;
            }
        }
        return true;
    }
    std::array<std::uint64_t, kBoxes> count(const std::byte* line, int loops) {
        for (int fd : fd_) {
            ioctl(fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        }
        for (int i = 0; i < loops; ++i) {
            _mm_clflush(line);
            _mm_mfence();
            (void)*reinterpret_cast<const volatile std::uint8_t*>(line);
            _mm_lfence();
        }
        std::array<std::uint64_t, kBoxes> counts{};
        for (int b = 0; b < kBoxes; ++b) {
            ioctl(fd_[b], PERF_EVENT_IOC_DISABLE, 0);
            if (read(fd_[b], &counts[b], sizeof(counts[b])) != static_cast<ssize_t>(sizeof(counts[b])))
                counts[b] = 0;
        }
        return counts;
    }
    void close() {
        for (int& fd : fd_)
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
    }
    ~Slices() { close(); }
    int error() const { return error_; }

private:
    int fd_[kBoxes] = {-1, -1, -1, -1};
    int error_ = 0;
};

// The box with most lookups, or -1 when no box takes at least half of them
// and four times the next box.
inline int dominant(const std::array<std::uint64_t, Slices::kBoxes>& counts, int loops) {
    int best = 0;
    for (int b = 1; b < Slices::kBoxes; ++b)
        if (counts[b] > counts[best]) best = b;
    std::uint64_t second = 0;
    for (int b = 0; b < Slices::kBoxes; ++b)
        if (b != best) second = std::max(second, counts[b]);
    return counts[best] * 2 >= static_cast<std::uint64_t>(loops) && second * 4 <= counts[best] ? best : -1;
}

// Run as root: the physical page of every page of the memory file and of the
// anonymous rings, then the slice of lines 0-3 of every control page. The
// event is chosen among candidate encodings by how cleanly the 64 lines of
// control page 0 each fall to one box.
inline void root_probe() {
    std::fprintf(stderr, "MM_ROOT cpu=%d\n", sched_getcpu());
    for (std::size_t p = 0; p < g_pool_pages; ++p)
        std::fprintf(stderr, "MM_PFN pool_page=%zu pfn=0x%llx\n", p,
                     static_cast<unsigned long long>(pfn(g_pool + p * kPage)));
    for (int j = 0; j < kMain; ++j)
        std::fprintf(stderr, "MM_PFN anon=%d pfn=0x%llx\n", j, static_cast<unsigned long long>(pfn(g_ring[kAnon][j])));
    static const std::uint64_t kCandidates[] = {0x8f34, 0x8834, 0x1834, 0x1f34, 0xff34, 0x4f34, 0x8134, 0x8634};
    constexpr int kLoops = 10000;
    std::uint64_t best = 0;
    int best_clean = -1;
    for (std::uint64_t config : kCandidates) {
        Slices slices;
        if (!slices.open(config)) {
            std::fprintf(stderr, "MM_SLICE_CAL config=0x%llx errno=%d\n", static_cast<unsigned long long>(config),
                         slices.error());
            continue;
        }
        int clean = 0;
        for (int line = 0; line < 64; ++line) {
            const auto c = slices.count(g_pool + 64 * line, kLoops);
            clean += dominant(c, kLoops) >= 0;
            std::fprintf(stderr, "MM_SLICE_CAL config=0x%llx line=%d counts=%llu,%llu,%llu,%llu\n",
                         static_cast<unsigned long long>(config), line, static_cast<unsigned long long>(c[0]),
                         static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]),
                         static_cast<unsigned long long>(c[3]));
        }
        std::fprintf(stderr, "MM_SLICE_SCORE config=0x%llx clean=%d/64\n", static_cast<unsigned long long>(config), clean);
        if (clean > best_clean) {
            best = config;
            best_clean = clean;
        }
    }
    if (best_clean < 0) return;
    Slices slices;
    if (!slices.open(best)) return;
    auto probe = [&](const char* what, int index, const std::byte* page) {
        for (int line = 0; line < 4; ++line) {
            const auto c = slices.count(page + 64 * line, kLoops);
            std::fprintf(stderr, "MM_SLICE config=0x%llx %s=%d line=%d pfn=0x%llx box=%d counts=%llu,%llu,%llu,%llu\n",
                         static_cast<unsigned long long>(best), what, index, line,
                         static_cast<unsigned long long>(pfn(page)), dominant(c, kLoops),
                         static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]),
                         static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]));
        }
    };
    for (int j = 0; j < kWide; ++j) probe("page", j, g_pool + j * kPage);
    for (int j = 0; j < kMain; ++j) probe("anon", j, g_ring[kAnon][j]);
}

// The memory file's pages are allocated in file order through one mapping;
// every ring is then mapped at an address of its own. Nothing is released.
inline bool allocate_all() {
    std::size_t pages = kWide;
    for (int s = 0; s < kSets; ++s) {
        g_set_first[s] = pages;
        pages += kSetPages[s];
    }
    g_fd = static_cast<int>(memfd_create("mm-mechanism", MFD_CLOEXEC));
    if (g_fd < 0) fail("memfd_create");
    if (ftruncate(g_fd, static_cast<off_t>(pages * kPage))) fail("ftruncate");
    void* view = mmap(nullptr, pages * kPage, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (view == MAP_FAILED) fail("mmap memory file");
    g_pool = static_cast<std::byte*>(view);
    g_pool_pages = pages;
    for (std::size_t p = 0; p < pages; ++p) *reinterpret_cast<volatile std::uint64_t*>(g_pool + p * kPage) = 0;
    for (int j = 0; j < kWide; ++j) g_ring[kComposite][j] = composite(j, kD0);
    for (int j = 0; j < kMain; ++j) {
        g_ring[kDataSwap][j] = composite(j, kD1);
        g_ring[kAlias][j] = composite(j, kD0);
        g_ring[kPadded][j] = composite(j, kPaddedSet);
        g_ring[kOrder][j] = composite(j, kOrderSet);
        g_ring[kAnon][j] = anonymous(kAnonPages);
    }
    g_ack = anonymous(1);
    g_order_source = anonymous(kOrderSourcePages);
    for (int m = 0; m < kMemories; ++m)
        for (int j = 0; j < kWide; ++j)
            if (g_ring[m][j])
                std::fprintf(stderr, "MM_PLACE memory=%s index=%d address=%p control_page=%d set=%s\n",
                             kMemoryNames[m], j, static_cast<void*>(g_ring[m][j]), m == kAnon ? -1 : j,
                             m == kAnon ? "anon" : kSetNames[kMemorySet[m]]);
    std::fprintf(stderr, "MM_PLACE ack=%p order_source=%p pool=%p pool_pages=%zu\n", static_cast<void*>(g_ack),
                 static_cast<void*>(g_order_source), static_cast<void*>(g_pool), g_pool_pages);
    if (geteuid() == 0) root_probe();
    return true;
}

template <class T>
inline T* order_source(std::int64_t n) {
    if (static_cast<std::size_t>(n) * sizeof(T) > kOrderSourcePages * kPage) std::abort();
    T* first = reinterpret_cast<T*>(g_order_source);
    for (std::int64_t i = 0; i < n; ++i) ::new (static_cast<void*>(first + i)) T();
    return first;
}

// The two CPUs the launch may use, lower first.
inline std::array<int, 2> launch_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set)) std::abort();
    std::array<int, 2> cpus{-1, -1};
    int found = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && found < 2; ++cpu)
        if (CPU_ISSET(cpu, &set)) cpus[found++] = cpu;
    if (found != 2 || CPU_COUNT(&set) != 2) std::abort();
    return cpus;
}

inline void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) std::abort();
}

// A thread created on one CPU, in place of std::thread, which would start
// the consumer wherever the scheduler puts it.
class Thread {
public:
    template <class F>
    Thread(int cpu, F&& body) {
        auto* fn = new std::function<void()>(std::forward<F>(body));
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_attr_setaffinity_np(&attr, sizeof(set), &set) || pthread_create(&id_, &attr, &start, fn))
            std::abort();
        pthread_attr_destroy(&attr);
    }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    void join() {
        if (pthread_join(id_, nullptr)) std::abort();
    }

private:
    static void* start(void* p) {
        std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(p));
        (*fn)();
        return nullptr;
    }
    pthread_t id_{};
};

// Hardware counters of the calling thread, user mode, as one group so that
// they count over the same time; the group must never be multiplexed.
struct CounterSpec {
    std::uint32_t type;
    std::uint64_t config;
    const char* name;
};
inline constexpr int kCounterCount = 8;
inline constexpr CounterSpec kCounterSpecs[kCounterCount] = {
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "cycles"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "instructions"},
    {PERF_TYPE_RAW, 0x04d2, "xsnp_fwd"},           // mem_load_l3_hit_retired.xsnp_fwd
    {PERF_TYPE_RAW, 0xe224, "rfo"},                // l2_rqsts.all_rfo
    {PERF_TYPE_RAW, 0x02c3, "mo_clears"},          // machine_clears.memory_ordering
    {PERF_TYPE_RAW, 0x0c000ca3, "stall_l1d_miss"}, // cycle_activity.stalls_l1d_miss
    {PERF_TYPE_RAW, 0x08a2, "stall_sb"},           // resource_stalls.sb
    {PERF_TYPE_RAW, 0x010d, "recovery"},           // int_misc.recovery_cycles
};

struct CounterTotals {
    double value[kCounterCount] = {};
    double enabled = 0, running = 0;
    int error = 0;
};

class ThreadCounters {
public:
    void open() {
        for (int i = 0; i < kCounterCount; ++i) {
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = kCounterSpecs[i].type;
            attr.config = kCounterSpecs[i].config;
            attr.disabled = i == 0;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            fd_[i] = static_cast<int>(perf_open(&attr, 0, -1, i ? fd_[0] : -1, PERF_FLAG_FD_CLOEXEC));
            if (fd_[i] < 0) {
                error_ = errno;
                close();
                return;
            }
        }
    }
    void start() {
        if (fd_[0] < 0) return;
        ioctl(fd_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(fd_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
    void stop(CounterTotals& into) {
        if (fd_[0] < 0) {
            into.error = error_ ? error_ : EBADF;
            return;
        }
        ioctl(fd_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        std::uint64_t values[3 + kCounterCount] = {};
        if (read(fd_[0], values, sizeof(values)) != static_cast<ssize_t>(sizeof(values)) ||
            values[0] != kCounterCount) {
            into.error = EIO;
            return;
        }
        into.enabled += static_cast<double>(values[1]);
        into.running += static_cast<double>(values[2]);
        for (int i = 0; i < kCounterCount; ++i) into.value[i] += static_cast<double>(values[3 + i]);
    }
    void close() {
        for (int& fd : fd_)
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
    }
    ~ThreadCounters() { close(); }

private:
    int fd_[kCounterCount] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int error_ = 0;
};

// Sampled loads of the calling thread that another core's cache served
// (mem_load_l3_hit_retired.xsnp_fwd, precise), by data address: head_ line,
// tail_ line, data slots by their position in a line, or other memory.
struct LineBins {
    double head = 0, tail = 0, data[8] = {}, other = 0, lost = 0;
    int error = 0;
};

class FwdSampler {
public:
    static constexpr std::uint64_t kPeriod = 997;
    static constexpr std::size_t kPages = 16;
    void open() {
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_RAW;
        attr.config = 0x04d2;
        attr.sample_period = kPeriod;
        attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR;
        attr.precise_ip = 2;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        fd_ = static_cast<int>(perf_open(&attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
        if (fd_ < 0) {
            error_ = errno;
            return;
        }
        void* map = mmap(nullptr, (1 + kPages) * kPage, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) {
            error_ = errno;
            ::close(fd_);
            fd_ = -1;
            return;
        }
        map_ = static_cast<unsigned char*>(map);
    }
    void start() {
        if (fd_ >= 0) ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
    }
    void stop() {
        if (fd_ >= 0) ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
    }
    // ring: the ring's first byte; slot: bytes per slot; slots: bytes of all slots.
    void drain(const std::byte* ring, std::size_t slot, std::size_t slots, LineBins& into) {
        if (!map_) {
            into.error = error_ ? error_ : EBADF;
            return;
        }
        auto* meta = reinterpret_cast<perf_event_mmap_page*>(map_);
        const std::uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
        std::uint64_t tail = meta->data_tail;
        const unsigned char* data = map_ + kPage;
        const std::uint64_t size = kPages * kPage;
        auto copy = [&](std::uint64_t at, void* out, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) static_cast<unsigned char*>(out)[i] = data[(at + i) % size];
        };
        const auto first = reinterpret_cast<std::uintptr_t>(ring);
        while (tail < head) {
            perf_event_header header;
            copy(tail, &header, sizeof(header));
            if (header.size < sizeof(header)) break;
            std::uint64_t v[2] = {0, 0};
            if (header.type == PERF_RECORD_SAMPLE && header.size >= sizeof(header) + sizeof(v)) {
                copy(tail + sizeof(header), v, sizeof(v));  // ip, addr
                const std::uint64_t addr = v[1];
                if (addr < first || addr >= first + 128 + slots) into.other += 1;
                else if (addr - first < 64) into.head += 1;
                else if (addr - first < 128) into.tail += 1;
                else into.data[((addr - first - 128) % 64) / slot] += 1;
            } else if (header.type == PERF_RECORD_LOST && header.size >= sizeof(header) + sizeof(v)) {
                copy(tail + sizeof(header), v, sizeof(v));  // id, lost
                into.lost += static_cast<double>(v[1]);
            }
            tail += header.size;
        }
        __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
    }
    ~FwdSampler() {
        if (map_) munmap(map_, (1 + kPages) * kPage);
        if (fd_ >= 0) ::close(fd_);
    }

private:
    int fd_ = -1;
    unsigned char* map_ = nullptr;
    int error_ = 0;
};

// The retries of one side during one transfer: fail(done) is called for every
// try that found the queue empty (consumer) or full (producer), with the number
// of items that side has completed.
struct Probe {
    std::int64_t fails = 0;  // tries that found the queue empty or full
    std::int64_t waits = 0;  // items before which the side had to retry
    std::int64_t last = -1;
    void fail(std::int64_t done) noexcept {
        ++fails;
        if (done != last) {
            last = done;
            ++waits;
        }
    }
};

enum Start { kAsIs, kProducerFirst, kConsumerFirst };
inline constexpr std::int64_t kHeadStart = 16384;
inline constexpr std::int64_t kTraceEvery = 1000;
inline constexpr int kTracePoints = 128;

struct alignas(64) PaddedInt {
    std::int64_t v;
};

// Variants. The defaults are the base variant BM_S1.
struct Variant {
    static constexpr Memory kMemory = kComposite;
    static constexpr bool kProbes = true;
    static constexpr bool kPause = false;
    static constexpr bool kCounters = false;
    static constexpr bool kSampling = false;
    static constexpr bool kTrace = false;
    static constexpr bool kHist = false;
    static constexpr Start kStart = kAsIs;
    static constexpr int kLag = -1;  // -1 the original ring; -2 the lag ring at depth >= 1; K >= 0 lines
    using Item = std::int64_t;
    template <std::size_t C>
    using Ring = me::SPSCRingBuffer<std::int64_t, C>;
    static const std::int64_t& item(const std::int64_t& i) noexcept { return i; }
    static std::int64_t value(std::int64_t v) noexcept { return v; }
};
// Opening counters or a sampler delays the consumer, which would lengthen the
// producer's lead and mix the two states in one iteration; these variants
// therefore start the consumer first.
struct S0 : Variant { static constexpr bool kProbes = false; };
struct S0C : S0 {
    static constexpr bool kCounters = true;
    static constexpr Start kStart = kConsumerFirst;
};
struct S1 : Variant {};
struct S1C : Variant {
    static constexpr bool kCounters = true;
    static constexpr Start kStart = kConsumerFirst;
};
struct S2 : Variant { static constexpr bool kPause = true; };
struct S2C : S2 {
    static constexpr bool kCounters = true;
    static constexpr Start kStart = kConsumerFirst;
};
struct S1Fwd : Variant {
    static constexpr bool kSampling = true;
    static constexpr Start kStart = kConsumerFirst;
};
struct TraceAsIs : Variant { static constexpr bool kTrace = true; };
struct TraceProducerFirst : TraceAsIs { static constexpr Start kStart = kProducerFirst; };
struct TraceConsumerFirst : TraceAsIs { static constexpr Start kStart = kConsumerFirst; };
struct Hist : Variant { static constexpr bool kHist = true; };
template <int K>
struct Lag : Variant {
    static constexpr int kLag = K;
    template <std::size_t C>
    using Ring = me::LagRing<std::int64_t, C>;
};
struct Padded : Variant {
    static constexpr Memory kMemory = kPadded;
    using Item = PaddedInt;
    template <std::size_t C>
    using Ring = me::SPSCRingBuffer<PaddedInt, C>;
    static PaddedInt item(std::int64_t i) noexcept { return PaddedInt{i}; }
    static std::int64_t value(const PaddedInt& v) noexcept { return v.v; }
};
struct DataSwap : Variant { static constexpr Memory kMemory = kDataSwap; };
struct Alias : Variant { static constexpr Memory kMemory = kAlias; };
struct Anon : Variant { static constexpr Memory kMemory = kAnon; };
struct OrderS1 : Variant { static constexpr Memory kMemory = kOrder; };
struct OrderS1C : OrderS1 {
    static constexpr bool kCounters = true;
    static constexpr Start kStart = kConsumerFirst;
};

template <class Ring>
struct RingTraits;
template <template <class, std::size_t> class R, class T, std::size_t C>
struct RingTraits<R<T, C>> {
    using Item = T;
    static constexpr std::size_t kCapacity = C;
};

template <class V>
struct Run;

// The consumer's state that its loop touches, kept in registers.
template <class V>
struct Hot {
    Probe probe;
    std::int64_t next = 1;            // trace: the item count to record the depth at next
    std::int32_t* trace = nullptr;    // trace: depth before item 1, then after every kTraceEvery items
    std::int64_t base = 0;            // hist: ring index of this iteration's first item
    std::int64_t mask = 0;
    std::int64_t hist[6] = {};        // hist: depth 1-7, 8-63, 64-511, >= 512; producer on the line; samples

    template <class Ring>
    void popped(const Ring& buf, std::int64_t received) noexcept {
        if constexpr (V::kTrace) {
            if (received == next) [[unlikely]] {
                trace[received / kTraceEvery] = static_cast<std::int32_t>(buf.size()) + (received == 1);
                next = received == 1 ? kTraceEvery : next + kTraceEvery;
            }
        }
        if constexpr (V::kHist) {
            if ((received & 15) == 0) [[unlikely]] {
                const std::int64_t depth = static_cast<std::int64_t>(buf.size()) + 1;
                const std::int64_t at = (base + received - 1) & mask;
                hist[depth < 8 ? 0 : depth < 64 ? 1 : depth < 512 ? 2 : 3] += 1;
                hist[4] += (at & 7) + depth <= 8;
                hist[5] += 1;
            }
        }
    }
    void failed(std::int64_t received) noexcept {
        if constexpr (V::kProbes) probe.fail(received);
        if constexpr (V::kPause) _mm_pause();
    }
};

// What the consumer thread sets up before its loop and reports after it.
template <class V>
class ConsumerSide {
public:
    explicit ConsumerSide(Run<V>& run) : run_(run) {
        run_.consumer_seen = sched_getcpu();
        if constexpr (V::kCounters) counters_.open();
        if constexpr (V::kSampling) sampler_.open();
        if constexpr (V::kStart == kProducerFirst)
            while (!run_.release.load(std::memory_order_acquire)) {}
        if constexpr (V::kCounters) counters_.start();
        if constexpr (V::kSampling) sampler_.start();
        if constexpr (V::kStart == kConsumerFirst) run_.ready.store(1, std::memory_order_release);
    }
    Hot<V> hot() {
        Hot<V> h;
        if constexpr (V::kTrace) h.trace = run_.traces.back().data();
        if constexpr (V::kHist) {
            h.base = run_.base_head;
            h.mask = static_cast<std::int64_t>(run_.mask);
        }
        return h;
    }
    void finish(const Hot<V>& hot) {
        finish();
        run_.pop = hot.probe;
        if constexpr (V::kHist)
            for (int i = 0; i < 6; ++i) run_.hist[i] += static_cast<double>(hot.hist[i]);
    }
    void finish() {
        if constexpr (V::kCounters) counters_.stop(run_.consumer_counters);
        if constexpr (V::kSampling) {
            sampler_.stop();
            sampler_.drain(run_.base, run_.slot_size, run_.slot_bytes, run_.consumer_bins);
        }
    }

private:
    Run<V>& run_;
    ThreadCounters counters_;
    FwdSampler sampler_;
};

inline std::map<std::string, int>& calls() {
    static std::map<std::string, int> counts;
    return counts;
}

// One benchmark call: pins the producer, builds the ring, runs the start
// mode, and reports per item the retries, the depth when the producer
// finished, the counters and samples of each side, and the traces.
template <class V>
struct Run {
    explicit Run(benchmark::State& state)
        : state_(state), n(state.range(0)), index(static_cast<int>(state.range(1))),
          orientation(static_cast<int>(state.range(2))) {
        const auto cpus = launch_cpus();
        producer_cpu = cpus[orientation];
        consumer_cpu_ = cpus[1 - orientation];
        if (sched_getaffinity(0, sizeof(saved_), &saved_)) std::abort();
        pin(producer_cpu);
        if constexpr (V::kCounters) producer_counters_.open();
        if constexpr (V::kSampling) producer_sampler_.open();
        if constexpr (V::kTrace) traces.reserve(64);
        key_ = state.name() + "/" + std::to_string(n) + "/" + std::to_string(index) + "/" + std::to_string(orientation);
        call = calls()[key_]++;
    }
    ~Run() { sched_setaffinity(0, sizeof(saved_), &saved_); }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    template <class Ring>
    Ring& ring() {
        std::byte* at = g_ring[V::kMemory][index];
        if (!at) std::abort();
        base = at;
        slot_size = sizeof(typename RingTraits<Ring>::Item);
        mask = RingTraits<Ring>::kCapacity - 1;
        slot_bytes = RingTraits<Ring>::kCapacity * slot_size;
        auto* r = ::new (static_cast<void*>(at)) Ring;
        if constexpr (V::kLag != -1)
            r->diag_configure(V::kLag == -2 ? 0 : ~std::size_t{0},
                              V::kLag == -2 ? 1 : static_cast<std::size_t>(V::kLag) * (64 / slot_size));
        return *r;
    }
    int consumer_cpu() const { return consumer_cpu_; }

    template <class Ring>
    void begin(Ring& buf) {
        ready.store(0, std::memory_order_relaxed);
        release.store(0, std::memory_order_relaxed);
        if constexpr (V::kLag != -1) buf.diag_release(false);
        base_head = (iterations * n) & static_cast<std::int64_t>(mask);
        if constexpr (V::kTrace) traces.emplace_back();
    }
    ConsumerSide<V> consumer_side() { return ConsumerSide<V>(*this); }

    // The producer's counters and sampler cover its push loop only.
    template <class Ring>
    std::int64_t producer_start(Ring& buf) {
        std::int64_t first = 0;
        if constexpr (V::kStart == kConsumerFirst)
            while (!ready.load(std::memory_order_acquire)) {}
        if constexpr (V::kStart == kProducerFirst) {
            for (std::int64_t i = 0; i < kHeadStart; ++i)
                while (!buf.try_push(V::item(i))) {}
            release.store(1, std::memory_order_release);
            first = kHeadStart;
        }
        if constexpr (V::kCounters) producer_counters_.start();
        if constexpr (V::kSampling) producer_sampler_.start();
        return first;
    }
    void push_failed(std::int64_t i) noexcept {
        if constexpr (V::kProbes) push.fail(i);
    }
    template <class Ring>
    void producer_end(Ring& buf) {
        depth = buf.size();
        if constexpr (V::kCounters) producer_counters_.stop(producer_counters);
        if constexpr (V::kSampling) producer_sampler_.stop();
        if constexpr (V::kLag != -1) buf.diag_release(true);
    }
    void end() {
        iterations += 1;
        pop_fails += static_cast<double>(pop.fails);
        pop_waits += static_cast<double>(pop.waits);
        push_fails += static_cast<double>(push.fails);
        push_waits += static_cast<double>(push.waits);
        pop = {};
        push = {};
        depth_end += static_cast<double>(depth) / static_cast<double>(n);
        misplaced += consumer_seen != consumer_cpu_;
        if constexpr (V::kSampling) producer_sampler_.drain(base, slot_size, slot_bytes, producer_bins);
        if constexpr (V::kTrace) start_depth += traces.back()[0];
    }

    void report() {
        if (!iterations) return;
        auto& c = state_.counters;
        const double items = static_cast<double>(iterations) * static_cast<double>(n);
        c["call"] = call;
        c["pop_fails"] = pop_fails / items;
        c["pop_waits"] = pop_waits / items;
        c["push_fails"] = push_fails / items;
        c["push_waits"] = push_waits / items;
        c["depth_end"] = depth_end / static_cast<double>(iterations);
        c["misplaced"] = misplaced + (sched_getcpu() != producer_cpu);
        c["producer_cpu"] = producer_cpu;
        if constexpr (V::kCounters) {
            for (const auto& [side, t] : {std::pair{"p_", &producer_counters}, std::pair{"c_", &consumer_counters}}) {
                for (int i = 0; i < kCounterCount; ++i) c[std::string(side) + kCounterSpecs[i].name] = t->value[i] / items;
                c[std::string(side) + "running"] = t->enabled > 0 ? t->running / t->enabled : 0;
                if (t->error) c[std::string(side) + "errno"] = t->error;
            }
        }
        if constexpr (V::kSampling) {
            const double scale = static_cast<double>(FwdSampler::kPeriod) / items;
            for (const auto& [side, b] : {std::pair{"p_fwd_", &producer_bins}, std::pair{"c_fwd_", &consumer_bins}}) {
                double data = 0;
                for (int s = 0; s < 8; ++s) {
                    c[std::string(side) + "slot" + std::to_string(s)] = b->data[s] * scale;
                    data += b->data[s];
                }
                c[std::string(side) + "head"] = b->head * scale;
                c[std::string(side) + "tail"] = b->tail * scale;
                c[std::string(side) + "data"] = data * scale;
                c[std::string(side) + "other"] = b->other * scale;
                c[std::string(side) + "lost"] = b->lost;
                if (b->error) c[std::string(side) + "errno"] = b->error;
            }
        }
        if constexpr (V::kHist) {
            const double samples = hist[5] > 0 ? hist[5] : 1;
            c["hist_1_7"] = hist[0] / samples;
            c["hist_8_63"] = hist[1] / samples;
            c["hist_64_511"] = hist[2] / samples;
            c["hist_512"] = hist[3] / samples;
            c["same_line"] = hist[4] / samples;
            c["hist_samples"] = hist[5];
        }
        if constexpr (V::kTrace) {
            c["start_depth"] = start_depth / static_cast<double>(iterations);
            for (std::size_t k = 0; k < traces.size(); ++k) {
                std::string line = "MM_TRACE " + key_ + " call=" + std::to_string(call) + " iteration=" +
                                   std::to_string(k) + " producer_cpu=" + std::to_string(producer_cpu) + " depths=";
                for (std::int64_t p = 0; p <= n / kTraceEvery && p < kTracePoints; ++p)
                    line += (p ? "," : "") + std::to_string(traces[k][p]);
                std::fprintf(stderr, "%s\n", line.c_str());
            }
        }
    }

    benchmark::State& state_;
    const std::int64_t n;
    const int index, orientation;
    int producer_cpu = -1, consumer_cpu_ = -1, call = 0;
    std::string key_;
    cpu_set_t saved_;
    std::byte* base = nullptr;
    std::size_t slot_size = 8, mask = 0, slot_bytes = 0;
    std::int64_t iterations = 0, base_head = 0;
    std::size_t depth = 0;
    alignas(64) std::atomic<int> ready{0};    // set by the consumer
    alignas(64) std::atomic<int> release{0};  // set by the producer
    alignas(64) Probe pop, push;
    int consumer_seen = -1;
    double pop_fails = 0, pop_waits = 0, push_fails = 0, push_waits = 0, depth_end = 0, start_depth = 0, misplaced = 0;
    double hist[6] = {};
    std::vector<std::array<std::int32_t, kTracePoints>> traces;
    ThreadCounters producer_counters_;
    FwdSampler producer_sampler_;
    CounterTotals producer_counters, consumer_counters;
    LineBins producer_bins, consumer_bins;
};

}  // namespace mm_mech

// One-way handoffs of one control line between the launch's two CPUs, as the
// two sides of a ring hand over an index: the writer stores its TSC and a
// sequence number into the line; the reader spins on the line, takes its own
// TSC as soon as it sees the number and answers on a line of its own, which
// the writer waits for before the next round. Arguments: control page, line
// (0 head_, 1 tail_), writer (0 the lower CPU, 1 the higher). Reports TSC
// ticks: the median one-way time with quartiles, and the writer's median round
// trip. One-way times assume the two CPUs' TSCs agree; an offset shifts the
// two directions by the same amount with opposite signs.
struct alignas(64) OneWayLine {
    std::atomic<std::uint64_t> seq;
    std::uint64_t tsc;
};
inline constexpr std::uint64_t kOneWayRounds = 4000;
inline constexpr std::uint64_t kOneWaySkip = 200;

static void BM_OneWay(benchmark::State& state) {
    auto* line = reinterpret_cast<OneWayLine*>(mm_mech::g_ring[mm_mech::kComposite][state.range(0)] + 64 * state.range(1));
    auto* ack = reinterpret_cast<std::atomic<std::uint64_t>*>(mm_mech::g_ack);
    const auto cpus = mm_mech::launch_cpus();
    const int writer = cpus[state.range(2)], reader = cpus[1 - state.range(2)];
    std::vector<std::int64_t> oneway, trip;
    std::vector<std::int64_t> d(kOneWayRounds + 1), rt(kOneWayRounds + 1);
    for (auto _ : state) {
        line->seq.store(0);
        line->tsc = 0;
        ack->store(0);
        std::atomic<int> ready{0};
        double seconds = 0;
        mm_mech::Thread r(reader, [&] {
            ready.fetch_add(1);
            while (ready.load() < 2) {}
            for (std::uint64_t k = 1; k <= kOneWayRounds; ++k) {
                while (line->seq.load(std::memory_order_acquire) != k) {}
                unsigned aux;
                const std::uint64_t t1 = __rdtscp(&aux);
                _mm_lfence();
                d[k] = static_cast<std::int64_t>(t1 - line->tsc);
                ack->store(k, std::memory_order_release);
            }
        });
        mm_mech::Thread w(writer, [&] {
            ready.fetch_add(1);
            while (ready.load() < 2) {}
            const auto start = std::chrono::steady_clock::now();
            unsigned aux;
            std::uint64_t t0 = __rdtscp(&aux);
            _mm_lfence();
            for (std::uint64_t k = 1; k <= kOneWayRounds; ++k) {
                line->tsc = t0;
                line->seq.store(k, std::memory_order_release);
                while (ack->load(std::memory_order_acquire) != k) {}
                const std::uint64_t t = __rdtscp(&aux);
                _mm_lfence();
                rt[k] = static_cast<std::int64_t>(t - t0);
                t0 = t;
            }
            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        });
        w.join();
        r.join();
        oneway.insert(oneway.end(), d.begin() + kOneWaySkip + 1, d.end());
        trip.insert(trip.end(), rt.begin() + kOneWaySkip + 1, rt.end());
        state.SetIterationTime(seconds);
    }
    auto quantile = [](std::vector<std::int64_t>& v, double q) {
        const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return static_cast<double>(v[k]);
    };
    state.counters["ow_q1"] = quantile(oneway, 0.25);
    state.counters["ow_q3"] = quantile(oneway, 0.75);
    state.counters["ow"] = quantile(oneway, 0.5);
    state.counters["rt"] = quantile(trip, 0.5);
    state.counters["writer_cpu"] = writer;
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kOneWayRounds));
}
BENCHMARK(BM_OneWay)
    ->ArgsProduct({benchmark::CreateDenseRange(0, mm_mech::kWide - 1, 1), {0, 1}, {0, 1}})
    ->Iterations(3)
    ->UseManualTime();
'''


def lag_ring(text):
    """SPSCRingBuffer as LagRing: try_pop takes a slot only when the depth
    reaches the rest of the slot's line plus a set number of slots, until the
    producer releases the ring."""
    text = patch(text, "#pragma once\n",
                 "// Generated by scripts/diagnostics/build_queue_mechanism.py from\n"
                 "// include/spsc_ring_buffer.h; lines that differ end in DIAG.\n#pragma once\n")
    text = patch(text, "class SPSCRingBuffer {\n", "class LagRing {  // DIAG\n")
    text = patch(text, "    SPSCRingBuffer() = default;\n", "    LagRing() = default;  // DIAG\n")
    text = patch(text, "    SPSCRingBuffer(const SPSCRingBuffer&) = delete;\n",
                 "    LagRing(const LagRing&) = delete;  // DIAG\n")
    text = patch(text, "    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;\n",
                 "    LagRing& operator=(const LagRing&) = delete;  // DIAG\n")
    text = patch(text,
                 "        // 检查队列是否为空（head 追上了 tail）\n"
                 "        if (head == tail_.load(std::memory_order_acquire)) {\n"
                 "            return false;  // 队列空\n"
                 "        }\n\n"
                 "        item = buffer_[head];\n",
                 "        // 检查队列是否为空（head 追上了 tail）\n"
                 "        const size_t diag_depth = (tail_.load(std::memory_order_acquire) - head) & mask_;  // DIAG\n"
                 "        if (diag_depth < (((kDiagSlots - (head & (kDiagSlots - 1))) & diag_line_mask_) + diag_extra_))"
                 " [[unlikely]] {  // DIAG\n"
                 "            if (diag_depth == 0 || !diag_released_.load(std::memory_order_acquire)) return false;  // DIAG\n"
                 "        }  // DIAG\n\n"
                 "        item = buffer_[head];\n")
    return patch(text, "    alignas(64) std::array<T, Capacity> buffer_{};\n};\n",
                 "    alignas(64) std::array<T, Capacity> buffer_{};\n"
                 "    static constexpr size_t kDiagSlots = 64 / sizeof(T) ? 64 / sizeof(T) : 1;  // DIAG\n"
                 "    alignas(64) size_t diag_line_mask_ = 0;  // DIAG\n"
                 "    size_t diag_extra_ = 1;  // DIAG\n"
                 "    std::atomic<bool> diag_released_{false};  // DIAG\n\n"
                 "public:  // DIAG\n"
                 "    void diag_configure(size_t line_mask, size_t extra) noexcept {  // DIAG\n"
                 "        diag_line_mask_ = line_mask;  // DIAG\n"
                 "        diag_extra_ = extra;  // DIAG\n"
                 "    }  // DIAG\n"
                 "    void diag_release(bool released) noexcept {  // DIAG\n"
                 "        diag_released_.store(released, std::memory_order_release);  // DIAG\n"
                 "    }  // DIAG\n"
                 "};\n")


def int_family(body, plain):
    """The int64 benchmark as a template over the variant. The plain copy keeps
    the original loops; the other has hooks on the failure paths and after
    each pop, and the start modes."""
    name = "BM_SPSC_Plain_ProducerConsumer" if plain else "BM_SPSC_ProducerConsumer"
    body = patch(body, "static void BM_SPSC_ProducerConsumer(benchmark::State& state) {\n",
                 "template <class V>  // DIAG\n"
                 f"static void {name}(benchmark::State& state) {{" + ("  // DIAG\n" if plain else "\n"))
    body = patch(body, "    SPSCRingBuffer<int64_t, kCapacity> buf;\n",
                 "    mm_mech::Run<V> diag_run(state);  // DIAG\n"
                 "    auto& buf = diag_run.template ring<typename V::template Ring<kCapacity>>();  // DIAG\n")
    body = patch(body, "        int64_t sum_consumer = 0;\n",
                 "        int64_t sum_consumer = 0;\n        diag_run.begin(buf);  // DIAG\n")
    body = patch(body, "        std::thread consumer([&] {\n",
                 "        mm_mech::Thread consumer(diag_run.consumer_cpu(), [&, N] {  // DIAG\n"
                 "            auto diag_side = diag_run.consumer_side();  // DIAG\n"
                 + ("" if plain else "            auto diag_hot = diag_side.hot();  // DIAG\n"))
    body = patch(body, "            int64_t val;\n",
                 "            typename V::Item val;  // DIAG\n            int64_t diag_sum = 0;  // DIAG\n")
    body = patch(body, "                    sum_consumer += val;\n",
                 "                    diag_sum += V::value(val);  // DIAG\n")
    loop_end = ("                    ++received;\n                }\n            }\n"
                "            done.store(true, std::memory_order_release);\n")
    push_loop = ("        for (int64_t i = 0; i < N; ++i) {\n"
                 "            while (!buf.try_push(i)) { /* 自旋等待：队列满了稍后再试 */ }\n        }\n")
    if plain:
        body = patch(body, loop_end,
                     "                    ++received;\n                }\n            }\n"
                     "            sum_consumer = diag_sum;  // DIAG\n"
                     "            diag_side.finish();  // DIAG\n"
                     "            done.store(true, std::memory_order_release);\n")
        body = patch(body, push_loop,
                     "        static_assert(V::kStart != mm_mech::kProducerFirst);  // DIAG\n"
                     "        diag_run.producer_start(buf);  // DIAG\n"
                     + push_loop + "        diag_run.producer_end(buf);  // DIAG\n")
    else:
        body = patch(body, loop_end,
                     "                    ++received;\n"
                     "                    diag_hot.popped(buf, received);  // DIAG\n"
                     "                } else [[unlikely]] {  // DIAG\n"
                     "                    diag_hot.failed(received);  // DIAG\n"
                     "                }\n            }\n"
                     "            sum_consumer = diag_sum;  // DIAG\n"
                     "            diag_side.finish(diag_hot);  // DIAG\n"
                     "            done.store(true, std::memory_order_release);\n")
        body = patch(body, push_loop,
                     "        for (int64_t i = diag_run.producer_start(buf); i < N; ++i) {  // DIAG\n"
                     "            while (!buf.try_push(V::item(i))) [[unlikely]] { diag_run.push_failed(i); }  // DIAG\n"
                     "        }\n"
                     "        diag_run.producer_end(buf);  // DIAG\n")
    body = patch(body, "        consumer.join();\n", "        consumer.join();\n        diag_run.end();  // DIAG\n")
    return patch(body, "    state.SetItemsProcessed(\n", "    diag_run.report();  // DIAG\n    state.SetItemsProcessed(\n")


def order_family(body):
    body = patch(body, "static void BM_SPSC_Order_ProducerConsumer(benchmark::State& state) {\n",
                 "template <class V>  // DIAG\nstatic void BM_SPSC_Order_ProducerConsumer(benchmark::State& state) {\n")
    body = patch(body, "    SPSCRingBuffer<Order, kCapacity> buf;\n",
                 "    mm_mech::Run<V> diag_run(state);  // DIAG\n"
                 "    auto& buf = diag_run.template ring<SPSCRingBuffer<Order, kCapacity>>();  // DIAG\n")
    body = patch(body, "    std::vector<Order> orders(N);\n",
                 "    std::span<Order> orders(mm_mech::order_source<Order>(N), static_cast<size_t>(N));  // DIAG\n")
    body = patch(body, "        int64_t received = 0;\n        int64_t sum = 0;\n",
                 "        int64_t received = 0;\n        int64_t sum = 0;\n        diag_run.begin(buf);  // DIAG\n")
    body = patch(body,
                 "        std::thread consumer([&] {\n"
                 "            Order o{};\n"
                 "            while (received < N) {\n"
                 "                if (buf.try_pop(o)) {\n"
                 "                    sum += o.price;  // 防止被优化掉\n"
                 "                    ++received;\n"
                 "                }\n"
                 "            }\n"
                 "        });\n",
                 "        mm_mech::Thread consumer(diag_run.consumer_cpu(), [&, N] {  // DIAG\n"
                 "            auto diag_side = diag_run.consumer_side();  // DIAG\n"
                 "            auto diag_hot = diag_side.hot();  // DIAG\n"
                 "            int64_t diag_received = 0, diag_sum = 0;  // DIAG\n"
                 "            Order o{};\n"
                 "            while (diag_received < N) {  // DIAG\n"
                 "                if (buf.try_pop(o)) {\n"
                 "                    diag_sum += o.price;  // DIAG\n"
                 "                    ++diag_received;  // DIAG\n"
                 "                } else [[unlikely]] {  // DIAG\n"
                 "                    diag_hot.failed(diag_received);  // DIAG\n"
                 "                }\n"
                 "            }\n"
                 "            received = diag_received;  // DIAG\n"
                 "            sum = diag_sum;  // DIAG\n"
                 "            diag_side.finish(diag_hot);  // DIAG\n"
                 "        });\n")
    body = patch(body, "        for (int64_t i = 0; i < N; ++i) {\n            while (!buf.try_push(orders[i])) {}\n        }\n",
                 "        static_assert(V::kStart != mm_mech::kProducerFirst);  // DIAG\n"
                 "        diag_run.producer_start(buf);  // DIAG\n"
                 "        for (int64_t i = 0; i < N; ++i) {\n"
                 "            while (!buf.try_push(orders[i])) [[unlikely]] { diag_run.push_failed(i); }  // DIAG\n"
                 "        }\n"
                 "        diag_run.producer_end(buf);  // DIAG\n")
    body = patch(body, "        consumer.join();\n", "        consumer.join();\n        diag_run.end();  // DIAG\n")
    return patch(body, "    state.SetItemsProcessed(\n", "    diag_run.report();  // DIAG\n    state.SetItemsProcessed(\n")


# (registered name, function template, variant, rings)
INT_VARIANTS = (
    ("BM_S0", "BM_SPSC_Plain_ProducerConsumer", "S0", "kMain"),
    ("BM_S0C", "BM_SPSC_Plain_ProducerConsumer", "S0C", "kMain"),
    ("BM_S1", "BM_SPSC_ProducerConsumer", "S1", "kWide"),
    ("BM_S1C", "BM_SPSC_ProducerConsumer", "S1C", "kMain"),
    ("BM_S2", "BM_SPSC_ProducerConsumer", "S2", "kMain"),
    ("BM_S2C", "BM_SPSC_ProducerConsumer", "S2C", "kMain"),
    ("BM_S1Fwd", "BM_SPSC_ProducerConsumer", "S1Fwd", "kMain"),
    ("BM_TraceAsIs", "BM_SPSC_ProducerConsumer", "TraceAsIs", "kMain"),
    ("BM_TraceProducerFirst", "BM_SPSC_ProducerConsumer", "TraceProducerFirst", "kMain"),
    ("BM_TraceConsumerFirst", "BM_SPSC_ProducerConsumer", "TraceConsumerFirst", "kMain"),
    ("BM_Hist", "BM_SPSC_ProducerConsumer", "Hist", "kMain"),
    ("BM_LagCtl", "BM_SPSC_ProducerConsumer", "Lag<-2>", "kMain"),
    ("BM_Lag0", "BM_SPSC_ProducerConsumer", "Lag<0>", "kMain"),
    ("BM_Lag4", "BM_SPSC_ProducerConsumer", "Lag<4>", "kMain"),
    ("BM_Lag16", "BM_SPSC_ProducerConsumer", "Lag<16>", "kMain"),
    ("BM_Lag64", "BM_SPSC_ProducerConsumer", "Lag<64>", "kMain"),
    ("BM_Padded", "BM_SPSC_ProducerConsumer", "Padded", "kMain"),
    ("BM_DataSwap", "BM_SPSC_ProducerConsumer", "DataSwap", "kMain"),
    ("BM_Alias", "BM_SPSC_ProducerConsumer", "Alias", "kMain"),
    ("BM_Anon", "BM_SPSC_ProducerConsumer", "Anon", "kMain"),
)
ORDER_VARIANTS = (("BM_OrderS1", "OrderS1"), ("BM_OrderS1C", "OrderS1C"))


def register(name, function, variant, rings, items, iterations):
    return (f"BENCHMARK_TEMPLATE({function}, mm_mech::{variant})  // DIAG\n"
            f"    ->Name(\"{name}\")  // DIAG\n"
            f"    ->ArgsProduct({{{{{items}}}, benchmark::CreateDenseRange(0, mm_mech::{rings} - 1, 1), {{0, 1}}}})  // DIAG\n"
            f"    ->Iterations({iterations})  // DIAG\n"
            "    ->UseRealTime();  // DIAG\n")


def build_source(root):
    text = (root / "benchmarks/bench_spsc_ring_buffer.cpp").read_text()
    text = patch(text, "#include <atomic>\n",
                 "#include <atomic>\n#include <span>  // DIAG\n#include \"mm_diag_mechanism.h\"  // DIAG\n")

    head, body, tail = segment(text, "static void BM_SPSC_ProducerConsumer(", "BENCHMARK(BM_SPSC_ProducerConsumer)")
    registrations = "".join(register(name, function, variant, rings, "100'000", 10)
                            for name, function, variant, rings in INT_VARIANTS)
    tail = patch(tail, "BENCHMARK(BM_SPSC_ProducerConsumer)\n    ->Arg(10'000)\n    ->Arg(100'000)\n"
                       "    ->UseRealTime();  // 多线程 benchmark 用 real time 更准确\n",
                 "\n" + int_family(body, plain=True) + "\n" + registrations)
    text = head + int_family(body, plain=False) + tail

    head, body, tail = segment(text, "static void BM_SPSC_Order_ProducerConsumer(",
                               "BENCHMARK(BM_SPSC_Order_ProducerConsumer)")
    tail = patch(tail, "BENCHMARK(BM_SPSC_Order_ProducerConsumer)\n    ->Arg(10'000)\n    ->UseRealTime();\n",
                 "".join(register(name, "BM_SPSC_Order_ProducerConsumer", variant, "kMain", "10'000", 20)
                         for name, variant in ORDER_VARIANTS))
    text = head + order_family(body) + tail

    return patch(text, "// ── 1. 单线程连续 push",
                 "static_assert(sizeof(SPSCRingBuffer<int64_t, 65536>) <= (1 + 128) * 4096);  // DIAG\n"
                 "static_assert(sizeof(me::LagRing<int64_t, 65536>) <= (1 + 128) * 4096);  // DIAG\n"
                 "static_assert(sizeof(SPSCRingBuffer<mm_mech::PaddedInt, 65536>) <= (1 + 1024) * 4096);  // DIAG\n"
                 "static_assert(sizeof(SPSCRingBuffer<Order, 16384>) <= (1 + 256) * 4096);  // DIAG\n"
                 "static_assert(sizeof(Order) * 10'000 <= mm_mech::kOrderSourcePages * 4096);  // DIAG\n"
                 "static const bool diag_memory = mm_mech::allocate_all();  // DIAG\n\n"
                 "// ── 1. 单线程连续 push")


def release_flags(build, target):
    text = (build / f"CMakeFiles/{target}.dir/flags.make").read_text()
    flags = {}
    for name in ("DEFINES", "INCLUDES", "FLAGS"):
        match = re.search(rf"^CXX_{name} = (.*)$", text, re.MULTILINE)
        if not match:
            raise SystemExit(f"CXX_{name} absent from {target} flags.make")
        flags[name] = shlex.split(match.group(1))
    link = shlex.split((build / f"CMakeFiles/{target}.dir/link.txt").read_text())
    return flags, link


def compile_and_link(build, out, target, source, binary_name):
    """Compile the generated source with the target's flags and link it in
    place of the target's own object."""
    flags, link = release_flags(build, target)
    original = f"CMakeFiles/{target}.dir/benchmarks/{target}.cpp.o"
    if link.count(original) != 1:
        raise SystemExit(f"expected one {original} in link.txt")
    obj = out / (Path(source).stem + ".o")
    commands = [[link[0], *flags["DEFINES"], f"-I{out}", *flags["INCLUDES"], *flags["FLAGS"],
                 "-o", str(obj), "-c", str(out / source)]]
    binary = out / binary_name
    linked, take_output = [], False
    for token in link:
        if take_output:
            linked.append(str(binary))
            take_output = False
        elif token == "-o":
            linked.append(token)
            take_output = True
        elif token == original:
            linked.append(str(obj))
        elif token.startswith("-Wl,--dependency-file="):
            linked.append(f"-Wl,--dependency-file={out / (binary_name + '.link.d')}")
        else:
            linked.append(token)
    commands.append(linked)
    for command in commands:
        subprocess.run(command, cwd=build, check=True)
    return binary, commands


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=root / "build/release")
    parser.add_argument("--output", type=Path, default=root / "build/diagnostics/mechanism")
    args = parser.parse_args()
    build, out = args.build.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "mm_diag_mechanism.h").write_text(HEADER)
    (out / "mm_diag_lag_ring.h").write_text(lag_ring((root / "include/spsc_ring_buffer.h").read_text()))
    source = "bench_queue_mechanism.cpp"
    (out / source).write_text(build_source(root))
    binary, commands = compile_and_link(build, out, "bench_spsc_ring_buffer", source, "bench_queue_mechanism")
    metadata = {
        "purpose": "diagnostic program of the queue-mechanism study; not a replacement benchmark",
        "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "inputs": {name: sha256(root / name) for name in ("benchmarks/bench_spsc_ring_buffer.cpp",
                                                          "include/spsc_ring_buffer.h", "include/order.h")},
        "generated": {name: sha256(out / name) for name in (source, "mm_diag_mechanism.h", "mm_diag_lag_ring.h")},
        "commands": commands,
        "binary_sha256": sha256(binary),
    }
    (out / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(binary)
    print(f"Build provenance: {out / 'build.json'}")


if __name__ == "__main__":
    main()
