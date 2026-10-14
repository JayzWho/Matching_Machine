Launch scores are medians of repetitions; all times below are ns.
Spread = (max / min − 1) × 100; CV uses sample standard deviation; IQR uses inclusive quartiles divided by the median. CV requires at least two observations.
Layout counts use independent launches; repeated lines in a launch are deduplicated.

## docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-14_6111ae4_launch-variance-layout

### Benchmark

| condition | binary | benchmark | launches | repetitions | median_ns | min_ns | max_ns | spread_pct | launch_cv_pct | iqr_pct | within_cv_median_pct | within_cv_max_pct |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| offset_00 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 16 | 5 | 6.20249e+06 | 5.94542e+06 | 6.79723e+06 | 14.3272 | 3.74755 | 4.54479 | 5.74572 | 9.49967 |
| offset_00 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 16 | 5 | 6.27365e+07 | 6.07257e+07 | 6.77821e+07 | 11.6201 | 2.63826 | 1.99093 | 2.33334 | 3.9848 |
| offset_00 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 16 | 5 | 3.16599e+07 | 2.99601e+07 | 3.53672e+07 | 18.0476 | 3.77429 | 2.84002 | 3.28321 | 4.3159 |
| offset_16 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 16 | 5 | 4.77471e+06 | 4.5041e+06 | 5.08019e+06 | 12.7903 | 3.15744 | 3.50718 | 4.35594 | 7.01874 |
| offset_16 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 16 | 5 | 4.76856e+07 | 4.66961e+07 | 5.02849e+07 | 7.68551 | 2.63878 | 4.55532 | 1.85828 | 3.24092 |
| offset_16 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 16 | 5 | 2.39479e+07 | 2.31637e+07 | 2.54396e+07 | 9.82562 | 3.12319 | 5.59644 | 2.8905 | 5.5131 |
| offset_32 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 16 | 5 | 4.64401e+06 | 4.42895e+06 | 5.16056e+06 | 16.5189 | 5.02905 | 6.28283 | 3.9743 | 6.17319 |
| offset_32 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 16 | 5 | 4.76023e+07 | 4.71678e+07 | 5.25144e+07 | 11.3353 | 2.81645 | 2.96819 | 1.2881 | 2.77079 |
| offset_32 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 16 | 5 | 2.38198e+07 | 2.31833e+07 | 2.53135e+07 | 9.1886 | 2.33359 | 2.79326 | 2.32034 | 4.9527 |
| offset_48 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 16 | 5 | 6.24581e+06 | 5.70759e+06 | 6.54211e+06 | 14.6212 | 3.35835 | 2.91077 | 5.81049 | 12.1259 |
| offset_48 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 16 | 5 | 6.42082e+07 | 6.27798e+07 | 6.72414e+07 | 7.10675 | 1.99427 | 2.91539 | 2.30761 | 4.5462 |
| offset_48 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 16 | 5 | 3.19767e+07 | 3.08839e+07 | 3.31372e+07 | 7.29608 | 2.32446 | 3.20258 | 3.22346 | 5.63101 |

### Telemetry

| condition | binary | launches | throttle_core | throttle_package | device_irqs | thermal_irqs | suspended_s | foreign_ms_per_s_median | foreign_ms_per_s_max | foreign_above_5_ms_per_s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| offset_00 | bench_mutex_layout | 16 | 0 | 0 | 0 | 0 | 0 | 3.53848 | 18.7157 | 5 |
| offset_16 | bench_mutex_layout | 16 | 0 | 0 | 0 | 0 | 0 | 8.64825 | 27.0089 | 9 |
| offset_32 | bench_mutex_layout | 16 | 0 | 0 | 0 | 0 | 0 | 8.20265 | 41.2321 | 8 |
| offset_48 | bench_mutex_layout | 16 | 0 | 0 | 0 | 0 | 0 | 5.75617 | 61.7316 | 9 |

### Layout

| condition | binary | layout_launches | unique_addresses | mod64_values | unique_mod64 | unique_mod4096 | multi_mod64_launches | multi_mod4096_launches |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| offset_00 | bench_mutex_layout | 16 | 16 | 0 | 1 | 15 | 0 | 0 |
| offset_16 | bench_mutex_layout | 16 | 16 | 16 | 1 | 12 | 0 | 0 |
| offset_32 | bench_mutex_layout | 16 | 16 | 32 | 1 | 15 | 0 | 0 |
| offset_48 | bench_mutex_layout | 16 | 16 | 48 | 1 | 15 | 0 | 0 |
