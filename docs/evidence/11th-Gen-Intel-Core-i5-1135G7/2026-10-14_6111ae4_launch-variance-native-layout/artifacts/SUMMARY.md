Launch scores are medians of repetitions; all times below are ns.
Spread = (max / min − 1) × 100; CV uses sample standard deviation; IQR uses inclusive quartiles divided by the median. CV requires at least two observations.
Layout counts use independent launches; repeated lines in a launch are deduplicated.

## docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-14_6111ae4_launch-variance-native-layout

### Benchmark

| condition | binary | benchmark | launches | repetitions | median_ns | min_ns | max_ns | spread_pct | launch_cv_pct | iqr_pct | within_cv_median_pct | within_cv_max_pct |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| native_layout_observation | bench_baseline_mutex | BM_MutexPipeline_Throughput/10000/iterations:3 | 16 | 5 | 5.85647e+06 | 4.80877e+06 | 6.98161e+06 | 45.185 | 14.826 | 27.3698 | 12.9875 | 23.9365 |
| native_layout_observation | bench_baseline_mutex | BM_MutexPipeline_Throughput/100000/iterations:3 | 16 | 5 | 6.11706e+07 | 4.66545e+07 | 6.51808e+07 | 39.7096 | 13.9381 | 24.9136 | 2.04123 | 4.51785 |
| native_layout_observation | bench_baseline_mutex | BM_MutexPipeline_Throughput/50000/iterations:3 | 16 | 5 | 2.98169e+07 | 2.328e+07 | 3.31129e+07 | 42.2374 | 14.6203 | 26.4632 | 2.587 | 5.58335 |

### Layout

| condition | binary | layout_launches | unique_addresses | mod64_values | unique_mod64 | unique_mod4096 | multi_mod64_launches | multi_mod4096_launches |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| native_layout_observation | bench_baseline_mutex | 16 | 16 | 0;16;32;48 | 4 | 15 | 0 | 0 |
