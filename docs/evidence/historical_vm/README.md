# Historical evidence — Tencent Cloud VM era

Raw benchmark output from the decommissioned measurement environment.

**This environment no longer exists and these measurements cannot be reproduced.**
They are retained as a historical record, not as support for current performance
claims. See `docs/AUDIT.md` for the methodology review.

## Environment

```
host          VM-0-3-ubuntu (Tencent Cloud, Ubuntu)
cpu           2 vCPU @ 2494 MHz
scaling       disabled (cpu_scaling_enabled: false)
caches        L1d 32 KB · L1i 32 KB · L2 4 MB · L3 37.4 MB (shared by 2 vCPU)
pmu           unavailable (virtualized)
build         Release, -O2 -DNDEBUG
```

## Files

### `2026-04-08_bench_order_book_after_opt.json`

Recovered from git history: added in `31198d0`, deleted in `1c35b37`
(`chore: untrack results/ and learning_notes/ from git`). This is the only raw
benchmark file that ever entered version control.

It does **not** correspond to the numbers published in `README.md` §4.1. It is an
earlier generation of the benchmark, and it is internally inconsistent:

| Benchmark | real_time | cpu_time |
|---|---|---|
| `BM_AddOrder_NoMatch/iterations:100000` | 277.2 ns | 243.4 ns |
| `BM_AddOrder_FullMatch/iterations:100000` | 275.9 ns | 275.4 ns |
| `BM_AddOrder_SweepLevels/1` | 12,824.5 ns | 12,776.1 ns |
| `BM_AddOrder_SweepLevels/5` | 11,020.7 ns | 11,040.3 ns |
| `BM_AddOrder_SweepLevels/10` | 12,153.3 ns | 12,141.1 ns |
| `BM_AddOrder_SweepLevels/20` | 11,825.1 ns | 11,843.2 ns |
| `BM_CancelOrder/iterations:50000` | 902,399.3 ns | 567,939.6 ns |
| `BM_MixedWorkload/iterations:20` | 34,991,553.9 ns | 17,390,542.8 ns |

Sweeping 20 price levels cannot cost less than sweeping 1. The `SweepLevels` family is
flat within noise across a 20× workload range, which means the timed region was
dominated by setup rather than by the sweep. Every entry also carries an explicit
`iterations:N` suffix that the current benchmark sources no longer set, confirming that
benchmark semantics changed between this run and the published table.

## Missing evidence

The files cited by `learning_notes/10_optimization.md` as the source of every figure in
`README.md` §4.1 were never committed and are not recoverable:

```
results/order_book_2026-04-14_23-35-17_baseline.json
results/order_book_2026-04-14_23-00-20_opt-v3.json
```

The pipeline and mutex-baseline figures in §4.2 and §4.3 cite
`results/matching_engine_2026-04-20_22-25-47_pipeline.json`, also not recoverable. The
reported values survive only as transcriptions inside `learning_notes/11_pipeline.md`.
