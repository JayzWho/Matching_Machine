Launch scores are medians of repetitions; all times below are ns.
Spread = (max / min − 1) × 100; CV uses sample standard deviation; IQR uses inclusive quartiles divided by the median. CV requires at least two observations.
Layout counts use independent launches; repeated lines in a launch are deduplicated.

## docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-14_6111ae4_launch-variance-affinity

### Benchmark

| condition | binary | benchmark | launches | repetitions | median_ns | min_ns | max_ns | spread_pct | launch_cv_pct | iqr_pct | within_cv_median_pct | within_cv_max_pct |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| offset_00_threads_1_3 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 12 | 5 | 6.05036e+06 | 5.69562e+06 | 6.48343e+06 | 13.8319 | 3.77286 | 4.74149 | 6.32392 | 9.20492 |
| offset_00_threads_1_3 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 12 | 5 | 6.28264e+07 | 6.05507e+07 | 6.48691e+07 | 7.13199 | 2.35336 | 4.0043 | 2.24508 | 4.11521 |
| offset_00_threads_1_3 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 12 | 5 | 3.12402e+07 | 3.02953e+07 | 3.24502e+07 | 7.11303 | 2.22477 | 2.93551 | 2.7887 | 5.5469 |
| offset_00_threads_3_1 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 12 | 5 | 6.12912e+06 | 5.84355e+06 | 6.53783e+06 | 11.8811 | 3.82937 | 6.42714 | 5.05587 | 13.2567 |
| offset_00_threads_3_1 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 12 | 5 | 6.32744e+07 | 6.10361e+07 | 6.5979e+07 | 8.09818 | 2.16886 | 2.71067 | 2.49035 | 3.92649 |
| offset_00_threads_3_1 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 12 | 5 | 3.13707e+07 | 2.93774e+07 | 3.29616e+07 | 12.2003 | 3.03169 | 3.2233 | 3.45119 | 7.45531 |
| offset_00_threads_none | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 12 | 5 | 6.07928e+06 | 5.79604e+06 | 6.44295e+06 | 11.1613 | 3.5369 | 5.24485 | 4.37346 | 7.2103 |
| offset_00_threads_none | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 12 | 5 | 6.35485e+07 | 6.00349e+07 | 6.46863e+07 | 7.74776 | 2.27441 | 2.7588 | 1.98985 | 3.06783 |
| offset_00_threads_none | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 12 | 5 | 3.15392e+07 | 3.04602e+07 | 3.23434e+07 | 6.1823 | 2.08053 | 3.45639 | 3.13069 | 5.2561 |
| offset_16_threads_1_3 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 12 | 5 | 4.59788e+06 | 4.37346e+06 | 5.09934e+06 | 16.5974 | 4.58873 | 4.73513 | 4.10547 | 8.03509 |
| offset_16_threads_1_3 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 12 | 5 | 4.80271e+07 | 4.71749e+07 | 5.10355e+07 | 8.18364 | 2.99122 | 5.71943 | 1.65624 | 7.47149 |
| offset_16_threads_1_3 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 12 | 5 | 2.38273e+07 | 2.32885e+07 | 2.5171e+07 | 8.08357 | 3.03887 | 5.7603 | 2.59595 | 10.9672 |
| offset_16_threads_3_1 | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 12 | 5 | 4.77508e+06 | 4.56731e+06 | 4.977e+06 | 8.97008 | 3.26675 | 6.44406 | 3.88207 | 6.46695 |
| offset_16_threads_3_1 | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 12 | 5 | 4.85767e+07 | 4.69747e+07 | 5.04236e+07 | 7.34221 | 2.44611 | 3.81189 | 1.38976 | 2.31797 |
| offset_16_threads_3_1 | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 12 | 5 | 2.43586e+07 | 2.30429e+07 | 2.48822e+07 | 7.9819 | 2.7048 | 4.33836 | 2.07273 | 6.0866 |
| offset_16_threads_none | bench_mutex_layout | BM_MutexPipeline_Throughput/10000/iterations:3 | 12 | 5 | 4.81547e+06 | 4.49787e+06 | 4.935e+06 | 9.71858 | 3.28118 | 5.08594 | 4.12968 | 10.0956 |
| offset_16_threads_none | bench_mutex_layout | BM_MutexPipeline_Throughput/100000/iterations:3 | 12 | 5 | 4.73739e+07 | 4.647e+07 | 5.02958e+07 | 8.23287 | 2.61738 | 4.2255 | 1.84446 | 4.10911 |
| offset_16_threads_none | bench_mutex_layout | BM_MutexPipeline_Throughput/50000/iterations:3 | 12 | 5 | 2.35725e+07 | 2.31577e+07 | 2.51205e+07 | 8.47579 | 2.92168 | 4.29874 | 3.27217 | 5.81521 |

### Telemetry

| condition | binary | launches | throttle_core | throttle_package | device_irqs | thermal_irqs | suspended_s | foreign_ms_per_s_median | foreign_ms_per_s_max | foreign_above_5_ms_per_s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| offset_00_threads_1_3 | bench_mutex_layout | 12 | 0 | 0 | 0 | 0 | 0 | 3.65695 | 6.24725 | 4 |
| offset_00_threads_3_1 | bench_mutex_layout | 12 | 0 | 0 | 0 | 0 | 0 | 2.82975 | 10.3219 | 4 |
| offset_00_threads_none | bench_mutex_layout | 12 | 0 | 0 | 0 | 0 | 0 | 5.30377 | 27.9754 | 6 |
| offset_16_threads_1_3 | bench_mutex_layout | 12 | 0 | 0 | 0 | 0 | 0 | 4.88508 | 95.4796 | 6 |
| offset_16_threads_3_1 | bench_mutex_layout | 12 | 0 | 0 | 0 | 0 | 0 | 5.3705 | 25.5178 | 6 |
| offset_16_threads_none | bench_mutex_layout | 12 | 0 | 0 | 0 | 0 | 0 | 5.50841 | 58.5688 | 6 |

### Layout

| condition | binary | layout_launches | unique_addresses | mod64_values | unique_mod64 | unique_mod4096 | multi_mod64_launches | multi_mod4096_launches |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| offset_00_threads_1_3 | bench_mutex_layout | 12 | 12 | 0 | 1 | 11 | 0 | 0 |
| offset_00_threads_3_1 | bench_mutex_layout | 12 | 12 | 0 | 1 | 9 | 0 | 0 |
| offset_00_threads_none | bench_mutex_layout | 12 | 12 | 0 | 1 | 11 | 0 | 0 |
| offset_16_threads_1_3 | bench_mutex_layout | 12 | 12 | 16 | 1 | 9 | 0 | 0 |
| offset_16_threads_3_1 | bench_mutex_layout | 12 | 12 | 16 | 1 | 12 | 0 | 0 |
| offset_16_threads_none | bench_mutex_layout | 12 | 12 | 16 | 1 | 11 | 0 | 0 |
