Launch scores are medians of repetitions; all times below are ns.
Spread = (max / min − 1) × 100; CV uses sample standard deviation; IQR uses inclusive quartiles divided by the median. CV requires at least two observations.
Layout counts use independent launches; repeated lines in a launch are deduplicated.

## docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-14_6111ae4_launch-variance-perf

### Benchmark

| condition | binary | benchmark | launches | repetitions | median_ns | min_ns | max_ns | spread_pct | launch_cv_pct | iqr_pct | within_cv_median_pct | within_cv_max_pct |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| profile_offset_00 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 8 | 5 | 6.09859e+06 | 5.78644e+06 | 6.51094e+06 | 12.5206 | 3.87665 | 3.89682 | 5.39351 | 8.83799 |
| profile_offset_00 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 8 | 5 | 6.29449e+07 | 6.22887e+07 | 6.57971e+07 | 5.63256 | 1.88938 | 1.4854 | 2.13694 | 3.11492 |
| profile_offset_00 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 8 | 5 | 3.18294e+07 | 3.05382e+07 | 3.32065e+07 | 8.73753 | 2.71634 | 2.88912 | 3.0343 | 4.55602 |
| profile_offset_16 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 8 | 5 | 4.79704e+06 | 4.5795e+06 | 4.87588e+06 | 6.47188 | 2.20645 | 2.92113 | 4.16909 | 5.49296 |
| profile_offset_16 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 8 | 5 | 4.98468e+07 | 4.95673e+07 | 5.16034e+07 | 4.10775 | 1.66102 | 1.26901 | 1.03878 | 2.71808 |
| profile_offset_16 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 8 | 5 | 2.48916e+07 | 2.47329e+07 | 2.57453e+07 | 4.09321 | 1.281 | 0.526128 | 1.14417 | 2.84498 |

### Layout

| condition | binary | layout_launches | unique_addresses | mod64_values | unique_mod64 | unique_mod4096 | multi_mod64_launches | multi_mod4096_launches |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| profile_offset_00 | bench_mutex_layout | 8 | 8 | 0 | 1 | 8 | 0 | 0 |
| profile_offset_16 | bench_mutex_layout | 8 | 8 | 16 | 1 | 8 | 0 | 0 |
