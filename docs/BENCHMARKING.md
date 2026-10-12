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
| `bench_matching_engine` | SPSC pipeline throughput at three sizes, a single-threaded baseline, and a latency report | 2 | both measurement cores |
| `bench_baseline_mutex` | Mutex pipeline throughput at three sizes, and a latency report | 2 | both measurement cores |

These programs still contain the defects described in `AUDIT.md`. A recording
of them in that state is called a **control**: it exists so that a later fix
can be compared with the unfixed code on the same machine.

## Vocabulary

| Term | Meaning |
|---|---|
| repetition | One Google Benchmark repetition inside a process |
| launch | One execution of one benchmark binary |
| round | One launch of each of the four binaries, in an order that rotates from round to round |
| session | One span of measurement mode, from `setup` to `restore` |
| condition | `shared`: other processes may run on the measurement cores. `cleared`: every task of the invoking user is restricted to the other cores for the round |
| study | Several sessions recorded by one invocation of `bench_record.py` |

## The scripts

| Script | Needs root | Purpose |
|---|---|---|
| `scripts/bench_env.sh` | for `setup` and `restore` | Enters and leaves measurement mode: frequency pinned at base with turbo off, SMT siblings offline, movable device interrupts steered to the other cores, `perf_event_paranoid` lowered. `status`, `cores` and `throttle` need no root |
| `scripts/install_bench_env.sh` | once | Installs a root-owned copy of `bench_env.sh` at `/usr/local/sbin/mm-bench-env`, so that one sudoers rule can allow `setup` and `restore` without a password. `--pin` fixes the measurement cores, `--check` verifies the installation |
| `scripts/capture_env.sh` | no | Writes a JSON record of the commit, toolchain, build flags, CPU, frequency policy, thermal state and measurement cores |
| `scripts/bench_record.py` | no | Records a study. Enters and leaves measurement mode itself through the sudoers rule |
| `scripts/bench_report.py` | no | Turns a study directory into Markdown tables |
| `scripts/test_bench_env.sh`, `scripts/test_install_bench_env.sh` | no | Tests of the two scripts above that touch privilege |
| `scripts/run_perf.sh` | — | Profiling helper from the cloud VM period. It has the defects listed in `AUDIT.md` and is not part of this workflow |

## Recording a study

```bash
# Once per machine. The script prints the sudoers rule to add.
sudo scripts/install_bench_env.sh
sudo scripts/install_bench_env.sh --pin "$(scripts/bench_env.sh cores --list)"

# Every time.
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j"$(nproc)"
scripts/bench_record.py --label control
scripts/bench_report.py docs/evidence/<cpu>/<study>
```

`bench_record.py` runs four sessions of eight rounds with five repetitions by
default, which takes about 80 minutes on the reference machine. Rounds
alternate between the two conditions, and the condition that goes first
alternates between sessions.

Pause virtual machines and close heavy applications before recording. In the
`cleared` condition the recorder moves the invoking user's tasks off the
measurement cores and returns each to its previous affinity afterwards; it
cannot move tasks of other users or kernel threads, so it measures them
instead (see `foreign_ms` below).

`scripts/bench_record.py --smoke` is a one-minute wiring check whose output is
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
| `mhz_min`, `mhz_max` | `scaling_cur_freq` of each busy core, once a second | The base frequency |
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
scatter? It reports the spread of the launch medians (largest relative to
smallest) next to the spread that the scatter inside a launch would produce by
itself. It then shuffles the repetitions between launches many times and marks
the benchmark as moving when fewer than one shuffle in a thousand reaches the
observed spread. The same is done for sessions, for a trend over time, and for
the difference between the two conditions.

## Known limits

- Tasks of other users and kernel threads are not kept off the measurement
  cores. Doing so needs a cpuset set up by `bench_env.sh setup`.
- The two threads of a two-threaded benchmark are placed by the scheduler
  within the two measurement cores; the benchmark code does not pin them.
- Latency is reported in ticks of the processor's timestamp counter (TSC). The
  counter's frequency has not been calibrated, so ticks are not converted to
  time.
- A study covers one boot. Whether results move across reboots or kernel
  updates is not covered by it.
