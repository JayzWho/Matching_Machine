# Whole-launch hardware and software counters

Counters cover each complete profiled process, including startup, allocation, thread creation and all three throughput sizes. They are not isolated 100,000-order counters.

Each launch processes 5 repetitions × 3 iterations × (10,000 + 50,000 + 100,000) = 2,400,000 workload orders. The per-order column divides whole-launch counts by that denominator; it retains process overhead. `cycles` are hardware core cycles.

Perf reports running percentages from 100.00% to 100.00% across 16 launches and 11 events.

| Event | profile_offset_00: launch median (n=8) | profile_offset_00: median per order | profile_offset_16: launch median (n=8) | profile_offset_16: median per order |
|---|---|---|---|---|
| `cycles` | 6,490,492,226.0 | 2704.37 | 4,961,065,456.5 | 2067.11 |
| `instructions` | 7,459,743,134.5 | 3108.23 | 6,729,207,540.0 | 2803.84 |
| `branches` | 1,559,908,160.0 | 649.962 | 1,381,053,177.0 | 575.439 |
| `branch-misses` | 23,042,706.5 | 9.60113 | 21,435,160.0 | 8.93132 |
| `cache-references` | 74,165,754.0 | 30.9024 | 60,452,344.0 | 25.1885 |
| `cache-misses` | 18,787,452.5 | 7.82811 | 18,241,965.5 | 7.60082 |
| `context-switches` | 7,086.0 | 0.0029525 | 6,757.0 | 0.00281542 |
| `cpu-migrations` | 45.5 | 1.89583e-05 | 46.0 | 1.91667e-05 |
| `page-faults` | 87,065.5 | 0.0362773 | 87,027.5 | 0.0362615 |
| `mem_load_l3_hit_retired.xsnp_fwd` | 7,173,342.5 | 2.98889 | 3,441,998.5 | 1.43417 |
| `l2_rqsts.all_rfo` | 20,193,250.5 | 8.41385 | 15,001,628.0 | 6.25068 |

## Measurement telemetry

Frequency samples are scheduler-derived readings and may include a core becoming idle. Their minimum does not independently establish the frequency while both workers are busy.

Derived process CPU and foreign CPU telemetry is omitted: perf starts the benchmark as a child, and the recorder's process-clock and wait4 accounting has not been validated for that wrapper. Frequency, interrupt and throttle snapshots remain applicable.

| Condition | Launches with frequency samples | Samples per launch | MHz min–max | Throttle events | Device IRQs | Suspended seconds |
|---|---:|---:|---:|---:|---:|---:|
| profile_offset_00 | 8/8 | 10–13 | 2399.975–2400.024 | 0 | 0 | 0.000 |
| profile_offset_16 | 8/8 | 6–9 | 1014.989–2400.028 | 0 | 0 | 0.000 |
