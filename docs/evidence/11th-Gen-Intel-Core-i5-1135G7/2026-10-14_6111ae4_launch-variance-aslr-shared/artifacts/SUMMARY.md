Launch scores are medians of repetitions; all times below are ns.
Spread = (max / min − 1) × 100; CV uses sample standard deviation; IQR uses inclusive quartiles divided by the median. CV requires at least two observations.
Layout counts use independent launches; repeated lines in a launch are deduplicated.

## docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-14_6111ae4_launch-variance-aslr-shared

### Benchmark

| condition | binary | benchmark | launches | repetitions | median_ns | min_ns | max_ns | spread_pct | launch_cv_pct | iqr_pct | within_cv_median_pct | within_cv_max_pct |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| native_aslr | bench_baseline_mutex | BM_MutexPipeline_Throughput/10000/iterations:3 | 24 | 5 | 4.98132e+06 | 4.71543e+06 | 6.51563e+06 | 38.1767 | 12.8902 | 26.5223 | 5.15638 | 7.72636 |
| native_aslr | bench_baseline_mutex | BM_MutexPipeline_Throughput/100000/iterations:3 | 24 | 5 | 4.991e+07 | 4.70345e+07 | 6.45647e+07 | 37.2711 | 14.238 | 31.5476 | 2.1714 | 4.37243 |
| native_aslr | bench_baseline_mutex | BM_MutexPipeline_Throughput/50000/iterations:3 | 24 | 5 | 2.49563e+07 | 2.308e+07 | 3.27083e+07 | 41.7169 | 14.4672 | 31.1608 | 3.35703 | 5.31686 |
| native_noaslr | bench_baseline_mutex | BM_MutexPipeline_Throughput/10000/iterations:3 | 24 | 5 | 4.86608e+06 | 4.52674e+06 | 5.0774e+06 | 12.1647 | 3.10982 | 4.72326 | 2.92186 | 5.96845 |
| native_noaslr | bench_baseline_mutex | BM_MutexPipeline_Throughput/100000/iterations:3 | 24 | 5 | 4.91445e+07 | 4.70281e+07 | 5.03847e+07 | 7.13746 | 2.48139 | 4.50899 | 1.38103 | 3.21987 |
| native_noaslr | bench_baseline_mutex | BM_MutexPipeline_Throughput/50000/iterations:3 | 24 | 5 | 2.46502e+07 | 2.32844e+07 | 2.52767e+07 | 8.55634 | 2.97661 | 5.75293 | 2.50778 | 4.63453 |

### Telemetry

| condition | binary | launches | throttle_core | throttle_package | device_irqs | thermal_irqs | suspended_s | foreign_ms_per_s_median | foreign_ms_per_s_max | foreign_above_5_ms_per_s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| native_aslr | bench_baseline_mutex | 24 | 0 | 0 | 0 | 0 | 0 | 4.84544 | 29.4034 | 12 |
| native_noaslr | bench_baseline_mutex | 24 | 0 | 0 | 0 | 0 | 0 | 2.74319 | 19.8451 | 9 |
