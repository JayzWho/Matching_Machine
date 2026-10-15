# Control with the mutex baseline's engine aligned

Recorded on 2026-10-15 on the 11th Gen Intel Core i5-1135G7 at commit `40a57bc`.

This is the current control. The benchmark programs still contain the defects
listed in [`docs/AUDIT.md`](../../../AUDIT.md), so the numbers describe these
programs on this machine and not how fast the engine is. It replaces the
[control of 2026-10-12](../2026-10-12_15-11-09_e8870ee_control-repeatability/REPORT.md),
called the previous control below.

## What changed since the previous control

Only `bench_baseline_mutex` was rebuilt, with two changes. Its producer sets the
completion flag under the queue mutex, which removes a hang. Its engine object
is declared `alignas(64)`: the
[launch-variance investigation](../2026-10-14_13-32-00_65a2223_launch-variance/REPORT.md)
found that the mutex pipeline runs at one of two levels depending on the
object's address modulo 64, which address-space randomisation changes at every
launch. Aligned, the object sits at offset 0 in every launch, the slower level.
The other three binaries are the files of the previous control; `env.json`
records the same SHA-256 for each.

## How it was recorded

The same design as the previous control: `scripts/bench_record.py --label control`,
4 sessions of 8 rounds with 5 repetitions, 32 launches of each binary, half of
them in `shared` and half in `cleared`. A launch is represented by the median of
its repetitions. The VirtualBox VM was saved for the whole study, so its process
had exited; in the previous control it was paused. The recorder cannot change
the affinity of `systemd --user`, so in `cleared` that one task of the user
could still run on the measurement cores.

## Findings

**1. The mutex pipeline runs at one level.** CV is the coefficient of variation
of the 32 launch medians.

| Orders | Previous control, launch medians | CV | This control, launch medians | CV |
|---:|---|---:|---|---:|
| 10,000 | 4.56 to 6.92 ms | 15.3% | 6.04 to 6.84 ms | 3.0% |
| 50,000 | 23.66 to 36.11 ms | 16.6% | 30.85 to 33.62 ms | 2.3% |
| 100,000 | 47.12 to 68.46 ms | 16.1% | 61.81 to 66.23 ms | 1.7% |

The median launch at 100,000 orders takes 64.65 ms, inside the upper level of
the previous control (63.1 to 68.5 ms). In five of the six combinations of size
and condition, launches no longer differ by more than their repetitions
scatter. The exception, 50,000 orders in `cleared` (p = 0.0001), has a spread of
8.4% against 6.8% expected from scatter. As a result the throughput ratio of
the SPSC pipeline to the mutex pipeline at 100,000 orders lies between 2.13×
and 2.47× in all 32 rounds; the previous control gave 1.60× to 2.49×, depending
on the level the mutex pipeline started at.

**2. The alignment does not steady the mutex latency report.** Its P50 still
ranges from 164,269 to 3,107,485 TSC ticks between launches, as widely as in
the previous control. `docs/AUDIT.md` (C1) explains that this report measures
queueing delay rather than matching latency.

**3. The unchanged binaries reproduce the previous control.** The medians of the
eleven single-threaded benchmarks are within 0.6% of the previous control's,
and those of the SPSC pipeline within 2.2%. The five producer-to-consumer
benchmarks of `bench_spsc_ring_buffer` move between launches as much as before,
with a spread of launch medians from 42% to 279%, and remain unexplained.

## Validity

All 128 launches had 0 thermal throttle events, 0 s suspended, and 0 device and
0 thermal interrupts on the measurement cores. Launches on one core read
2400 MHz throughout, and the package stayed at 43 to 55 °C. Other tasks used a
median of 6.6 ms of CPU time per second on the launch's cores in `shared`, with
one launch at 57.5 ms per second, and 1.0 ms in `cleared`, with a maximum
of 5.4.

## Reproduce

```bash
cmake --build build/release -j"$(nproc)"
scripts/bench_record.py --label control
scripts/bench_report.py docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-15_16-04-12_40a57bc_control
```

The last command prints the full tables: per-benchmark launch statistics,
round-by-round medians, `shared` against `cleared`, thread scheduling and the
latency percentiles.
