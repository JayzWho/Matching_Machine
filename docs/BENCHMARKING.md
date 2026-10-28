# Benchmarking guide

How performance is measured in this repository: which programs are measured,
which scripts run them, what is checked, and where results are kept. The
results themselves are indexed in [`evidence/README.md`](evidence/README.md).
The review of the earlier methodology is [`AUDIT.md`](AUDIT.md).

## The benchmark programs

All four are built from `benchmarks/` by the Release configuration.

| Binary | Benchmarks | Threads | Runs on |
|---|---|---|---|
| `bench_order_book` | Add, match, sweep, cancel and mixed workload on the order book (8) | 1 | the first measurement core |
| `bench_spsc_ring_buffer` | Ring buffer push and pop (2); producer to consumer over the SPSC ring buffer (3) and over a mutex queue (2) | 1, then 2 | both measurement cores |
| `bench_matching_engine` | SPSC pipeline throughput at three sizes, a single-threaded baseline, and a latency report | 2 | both measurement cores: the producer thread on the lower-numbered, the consumer on the higher |
| `bench_baseline_mutex` | Mutex pipeline throughput at three sizes, and a latency report | 2 | as `bench_matching_engine` |

These programs still contain the defects described in `AUDIT.md`. A recording
of them in that state is called a **control**: it exists so that a later fix
can be compared with the unfixed code on the same machine.

`bench_baseline_mutex` declares its engine object `alignas(64)`. Without it,
the address-space layout of each launch chose between two performance levels
of the mutex pipeline; see [`evidence/README.md`](evidence/README.md).

## Vocabulary

| Term | Meaning |
|---|---|
| repetition | One Google Benchmark repetition inside a process. The first repetition of each benchmark in a launch is a **warm-up**: it is recorded and marked in `samples.csv`, and the reports leave it out |
| launch | One execution of one benchmark binary |
| round | One launch of each of the four binaries, in an order that rotates from round to round |
| session | One span of measurement mode, from `setup` to `restore` |
| condition | `shared`: other processes may run on the measurement cores. `cleared`: every task of the invoking user is restricted to the other cores for the round |
| study | Several sessions recorded by one invocation of `bench_record.py` |

## The scripts

`scripts/` is grouped by step: put the machine into measurement mode (`env/`),
record (`record/`), turn the records into tables (`report/`), and alter a
launch for a diagnostic condition (`diagnostics/`). Each test sits next to the
script it tests, and `ctest` runs all of them (`ctest -L scripts` only those).

| Script | Needs root | Purpose |
|---|---|---|
| `env/bench_env.sh` | for `setup` and `restore` | Enters and leaves measurement mode: frequency pinned at base with turbo off, SMT siblings offline, movable device interrupts steered to the other cores, `perf_event_paranoid` lowered. `status`, `cores` and `throttle` need no root |
| `env/install_bench_env.sh` | once | Installs a root-owned copy of `bench_env.sh` at `/usr/local/sbin/mm-bench-env`, so that one sudoers rule can allow `setup` and `restore` without a password. `--pin` fixes the measurement cores, `--check` verifies the installation |
| `env/capture_env.sh` | no | Writes a JSON record of the commit, toolchain, build flags, CPU, frequency policy, TSC frequency, thermal state and measurement cores |
| `env/tsc_calibrate.cpp` | no | Built with the benchmarks. Measures the TSC frequency against `CLOCK_MONOTONIC` for `capture_env.sh` |
| `record/bench_record.py` | no | Records a study of the four binaries. Enters and leaves measurement mode itself through the sudoers rule |
| `record/bench_diagnose.py` | no | Records a rotating matrix of focused diagnostic conditions, entering and restoring measurement mode itself |
| `report/bench_report.py` | no | Turns a study directory into Markdown tables |
| `report/diagnose_report.py` | no | Turns a diagnostic study into Markdown or CSV tables |
| `diagnostics/thread_affinity.c` | no | A preloaded library that places each new worker thread on a chosen CPU; `thread_affinity.md` describes it |
| `env/test_bench_env.sh`, `env/test_install_bench_env.sh` | no | Tests of the two scripts that touch privilege |
| `record/test_bench_diagnose.py` | no | Tests of the diagnostic runner's validation and failure cleanup |
| `report/test_bench_report.py`, `report/test_diagnose_report.py` | no | Checks of the statistical test of `bench_report.py` on synthetic launches, and of the diagnostic tables |
| `diagnostics/test_thread_affinity.py` | no | Builds the preloaded library and checks its placement on a small program |

## Recording a study

```bash
# Once per machine. The script prints the sudoers rule to add.
sudo scripts/env/install_bench_env.sh
sudo scripts/env/install_bench_env.sh --pin "$(scripts/env/bench_env.sh cores --list)"

# Every time.
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j"$(nproc)"
scripts/record/bench_record.py --label control
scripts/report/bench_report.py docs/evidence/<cpu>/<study>
```

`bench_record.py` runs four sessions of eight rounds with six repetitions by
default, the first a warm-up, which takes about 95 minutes on the reference
machine. Rounds alternate between the two conditions, and the condition that
goes first alternates between sessions.

Save the state of virtual machines and close heavy applications before
recording. In the `cleared` condition the recorder moves the invoking user's
tasks off the measurement cores and returns each to its previous affinity
afterwards. It cannot move tasks of other users, kernel threads, or a task of
the user that holds a capability the recorder lacks, such as `systemd --user`,
so it measures them instead (see `foreign_ms` below).

`scripts/record/bench_record.py --smoke` is a one-minute wiring check whose output is
discarded. Add `--unpinned` to run it outside measurement mode.

## What is recorded for every launch

`launches.csv` has one row per launch. The columns that decide whether a
launch can be trusted:

| Column | Source | Expected |
|---|---|---|
| `throttle_core`, `throttle_package` | Thermal throttle counters in sysfs, each counted once | 0 |
| `suspended_s` | Difference between `CLOCK_BOOTTIME` and `CLOCK_MONOTONIC` | 0 |
| `device_irqs` | `/proc/interrupts`, numbered lines, on the measurement cores | 0 |
| `irq_TRM` | Thermal event interrupts on the measurement cores | 0 |
| `mhz_min`, `mhz_max` | `scaling_cur_freq` of the launch's cores, once a second | The base frequency for a core that stays busy. A launch on two cores leaves one idle during its single-threaded benchmarks, and an idle core reports a lower value |
| `foreign_ms` | CPU time on the launch's cores (`/proc/schedstat`) minus the launch's own CPU time | Close to 0 |
| `waited_ms` | Time runnable tasks waited for the launch's cores (`/proc/schedstat`) | Close to 0 |
| `involuntary_switches` | `getrusage` of the launch | Low |
| `cpu_per_wall` | The launch's CPU time divided by its wall time | 1 for one thread, up to 2 for two |

The remaining columns hold the per-CPU interrupt and softirq counts, context
switches, page faults, temperatures and the load average.

`intervals.csv` splits each launch by benchmark: wall time, CPU time and
run-queue wait between the moment the previous benchmark's results were printed
and the moment this one's were.

## Reading the report

`bench_report.py` answers one question per benchmark: do results move between
launches of unchanged binaries by more than the repetitions inside one launch
scatter? Each launch is represented by the median of its repetitions, and the
report gives four numbers about those launch medians:

| Number | Meaning |
|---|---|
| Middle half | The distance between their quartiles, relative to their median: how far a typical launch is from another |
| Spread | The largest relative to the smallest, minus one: how far the two most distant launches are apart |
| Expected from scatter | The spread that the scatter inside a launch would produce if launches did not differ |
| p (launches) | How often shuffling the repetitions between launches makes the launches differ as much as observed |

A benchmark moves between launches when p is below 0.001. The same test is
applied to the launch medians grouped by session and by condition, and a rank
correlation with recording order tests for a trend over time. Warm-up
repetitions are left out of all of these.

The test compares the rank sums of the launches (the Kruskal-Wallis statistic)
and not their spread, because a benchmark that settles on one of two levels at
each launch keeps the same spread when its repetitions are shuffled.
`scripts/report/test_bench_report.py` checks the test on synthetic launches: it stays
quiet when launches do not differ and finds both a gradual difference and a
two-level one. A single launch that is slower than the rest shows in the spread
and in the count of launches beyond scatter more reliably than in p.

Because launches differ, a comparison between two versions of the code needs
several launches of each, and compares their launch medians. The scatter of the
repetitions inside one launch says nothing about the next launch.

## Results not to compare yet

The [variance-sources study](evidence/README.md) found defects that move these
results for reasons other than the code under test. Until a pull request fixes
each, do not compare them between versions of the code:

- **Both latency reports.** They measure how long an order waits in the queue,
  not how long matching takes (`AUDIT.md`, C1).
- **The producer-to-consumer benchmarks of `bench_spsc_ring_buffer`.** In
  `BM_SPSC_ProducerConsumer` the consumer adds every item to a checksum on the
  cache line of `N`, which the producer reads for every item. All three keep
  their queue in memory that a launch draws once and leave their two threads to
  the scheduler, and these two choices together set their level; the mutex
  queue's state is also unaligned. Without the shared checksum, the int64 ring
  has two levels, 8 to 9 and about 60 ns per item. They are two states of the
  transfer: in the slower one the consumer has caught up with the producer and
  waits for it before about one item in twelve, reading the cache line the
  producer is still writing. The ring is in the slower state exactly when the
  slice of the last-level cache that serves `tail_` is the slice of the
  producer's CPU. The physical address of the queue sets the slice, so in each
  assignment of the threads to the CPUs one fresh region in four is slow, and
  no region is slow in both. The fix keeps both states as a result: it pins the
  threads, measures both assignments of them to the CPUs on many fresh memory
  regions per launch, and reports the level of each state and the share of slow
  measurements. The Order ring and the mutex queue have no such levels; they are
  reported by their median over the same measurements.

The pipeline throughput is comparable through launch medians. The first
repetition in a launch pays a one-time cost of the process, which is why the
recorders mark the first repetition of every benchmark as a warm-up.

## Known limits

- Tasks of other users and kernel threads are not kept off the measurement
  cores. Doing so needs a cpuset set up by `bench_env.sh setup`.
- The pipeline benchmarks pin each of their two threads to one measurement
  core when the thread is created, and print the placement in each benchmark's
  label. The producer-to-consumer benchmarks of `bench_spsc_ring_buffer` still
  leave their two threads to the scheduler.
- Latency is reported in ticks of the processor's timestamp counter (TSC).
  `capture_env.sh` measures the counter's frequency against `CLOCK_MONOTONIC`
  before and after every session and records it in `env.json` under `tsc`;
  the reports do not convert ticks to time yet.
- The timer tick, softirqs and per-CPU kernel threads stay on the measurement
  cores. Removing them needs kernel boot parameters.
- A study covers one boot. Whether results move across reboots or kernel
  updates is not covered by it.

## Diagnostic runs

`scripts/record/bench_diagnose.py` records a focused question instead of the four
binaries. Its input is a JSON matrix of conditions, each naming an executable
and a benchmark filter, and optionally a command prefix, environment overrides
and arguments. A diagnostic **block** launches each condition once, in an order
that rotates between blocks; `python3 scripts/record/bench_diagnose.py --help` lists
the options. The output has the layout of a study plus `matrix.json`, and
`scripts/report/diagnose_report.py` prints its tables.

- Build every executable first. The runner does not build.
- Keep the length of the arguments and of the environment fixed within a study:
  they move the stack, and the placement of an object on the stack has been
  shown to select a performance level.
- A prefix that wraps the benchmark, such as `perf` or GDB, takes over the
  recorder's accounting of the launch's CPU time. Take scores from the
  benchmark's JSON and CPU counts from the profiler.
- In `cleared`, the runner stops when a task of the user refuses the new CPU
  mask. On this machine `systemd --user` (`CAP_WAKE_ALARM`) and a running
  VirtualBox VM do. `--allow-unmovable NAME` accepts the tasks of one named
  process and records them in `launches.csv` and `env.json`. Save a VM's state
  rather than pausing it, so that its process exits.
- A launch still running after `--launch-timeout` seconds (300 by default) is
  killed and the study stops. Partial output is kept, and measurement mode and
  the saved affinities are restored before the error is reported.
