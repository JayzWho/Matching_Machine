Launch scores are medians of repetitions; all times below are ns.
Spread = (max / min − 1) × 100; CV uses sample standard deviation; IQR uses inclusive quartiles divided by the median. CV requires at least two observations.
Layout counts use independent launches; repeated lines in a launch are deduplicated.

## docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-14_6111ae4_launch-variance-other-dual

### Benchmark

| condition | binary | benchmark | launches | repetitions | median_ns | min_ns | max_ns | spread_pct | launch_cv_pct | iqr_pct | within_cv_median_pct | within_cv_max_pct |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| queues_aslr | bench_spsc_ring_buffer | BM_Mutex_ProducerConsumer/100000/real_time | 8 | 5 | 2.97383e+07 | 2.5055e+07 | 3.72091e+07 | 48.5095 | 13.0391 | 15.9414 | 2.15272 | 5.31116 |
| queues_aslr | bench_spsc_ring_buffer | BM_SPSC_ProducerConsumer/100000/real_time | 8 | 5 | 4.81108e+06 | 3.55507e+06 | 6.99529e+06 | 96.769 | 27.2383 | 40.7838 | 10.1614 | 22.346 |
| queues_noaslr | bench_spsc_ring_buffer | BM_Mutex_ProducerConsumer/100000/real_time | 8 | 5 | 3.09127e+07 | 2.39963e+07 | 4.57154e+07 | 90.51 | 26.8288 | 44.5628 | 2.29669 | 24.794 |
| queues_noaslr | bench_spsc_ring_buffer | BM_SPSC_ProducerConsumer/100000/real_time | 8 | 5 | 6.95526e+06 | 5.51413e+06 | 8.66709e+06 | 57.1798 | 12.9358 | 8.03594 | 3.12478 | 4.26755 |
| spsc_pipeline_aslr | bench_matching_engine | BM_Pipeline_Throughput/10000/iterations:3 | 8 | 5 | 2.38603e+06 | 2.31336e+06 | 2.54132e+06 | 9.85394 | 3.07585 | 3.30176 | 16.8705 | 19.3591 |
| spsc_pipeline_aslr | bench_matching_engine | BM_Pipeline_Throughput/100000/iterations:3 | 8 | 5 | 2.85261e+07 | 2.69585e+07 | 2.95058e+07 | 9.44867 | 2.96333 | 4.14376 | 2.06479 | 2.53832 |
| spsc_pipeline_aslr | bench_matching_engine | BM_Pipeline_Throughput/50000/iterations:3 | 8 | 5 | 1.33223e+07 | 1.29898e+07 | 1.38019e+07 | 6.25183 | 2.22415 | 3.0383 | 2.6921 | 5.43052 |
| spsc_pipeline_noaslr | bench_matching_engine | BM_Pipeline_Throughput/10000/iterations:3 | 8 | 5 | 2.40576e+06 | 2.3216e+06 | 2.74077e+06 | 18.0553 | 5.51932 | 3.70707 | 18.1621 | 30.3198 |
| spsc_pipeline_noaslr | bench_matching_engine | BM_Pipeline_Throughput/100000/iterations:3 | 8 | 5 | 2.81664e+07 | 2.70996e+07 | 2.90013e+07 | 7.01757 | 2.35416 | 3.24477 | 1.65173 | 3.01167 |
| spsc_pipeline_noaslr | bench_matching_engine | BM_Pipeline_Throughput/50000/iterations:3 | 8 | 5 | 1.31161e+07 | 1.24055e+07 | 1.35748e+07 | 9.42557 | 2.95586 | 3.16839 | 2.19319 | 3.0091 |

### Telemetry

| condition | binary | launches | throttle_core | throttle_package | device_irqs | thermal_irqs | suspended_s | foreign_ms_per_s_median | foreign_ms_per_s_max | foreign_above_5_ms_per_s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| queues_aslr | bench_spsc_ring_buffer | 8 | 0 | 0 | 0 | 0 | 0 | 1.07049 | 3.20612 | 0 |
| queues_noaslr | bench_spsc_ring_buffer | 8 | 0 | 0 | 0 | 0 | 0 | 0.773802 | 2.05959 | 0 |
| spsc_pipeline_aslr | bench_matching_engine | 8 | 0 | 0 | 0 | 0 | 0 | 0.928725 | 7.33626 | 1 |
| spsc_pipeline_noaslr | bench_matching_engine | 8 | 0 | 0 | 0 | 0 | 0 | 1.49523 | 2.49243 | 0 |
