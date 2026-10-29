# Order book fixes: rough check of 2026-10-29

The check looks for a large cost of the two fixes on branch
`fix/order-book-empty-levels`: a price level skips cancelled slots when it pops
its front order, and `OrderBook::clear()` keeps the order index's slots.

## Design

This is not a recording. It ran outside measurement mode (`powersave`
governor, turbo on, SMT siblings online, device interrupts not steered) on an
otherwise idle machine, from 14:23 to 14:26.
`scripts/diagnostics/order_book_fix_check.sh` runs three builds six times each,
in an order that rotates every round, one repetition per run:
`bench_order_book` filtered to `SweepLevels` on CPU 2, and
`bench_matching_engine` filtered to `SingleThread|Pipeline_Throughput` on CPUs 2
and 3, where the pipeline pins its producer to CPU 2 and its consumer to CPU 3.

| Build | Commit |
|---|---|
| base | `fb5f8e6`, master before the fixes |
| c1 | `a3444c7`, levels skip cancelled slots |
| c2 | `51201a0`, and `clear()` keeps the index's slots |

i5-1135G7, kernel 7.0.0-34-generic, GCC 15.2.0 with `-O2 -DNDEBUG -std=c++20`.
`env.json` was captured after the runs. `raw.tar.gz` holds one JSON file
per run; `scripts/diagnostics/order_book_fix_check.py raw.tar.gz` prints the
table below.

## Results

Median of six runs: base in ns per iteration, c1 and c2 relative to base.

| Benchmark | base (ns) | c1 | c2 | CV of base |
|---|---:|---:|---:|---:|
| `BM_AddOrder_SweepLevels/1` | 259 | +4.2% | +2.9% | 0.9% |
| `BM_AddOrder_SweepLevels/5` | 410 | -1.6% | +4.1% | 0.6% |
| `BM_AddOrder_SweepLevels/10` | 566 | -0.7% | -7.2% | 0.9% |
| `BM_AddOrder_SweepLevels/20` | 956 | -2.1% | -1.8% | 0.4% |
| `BM_SingleThread_Baseline` | 805,374 | +1.6% | -10.7% | 0.4% |
| `BM_Pipeline_Throughput/10000` | 2,591,402 | -10.3% | -20.5% | 13.2% |
| `BM_Pipeline_Throughput/50000` | 7,569,017 | -1.4% | -0.9% | 1.2% |
| `BM_Pipeline_Throughput/100000` | 15,627,367 | -0.0% | -1.9% | 0.7% |

## Conclusion

- The pipeline at 50,000 and 100,000 orders moves by at most 2%. At 10,000
  orders a run is too short to read outside measurement mode (CV 13%).
- `BM_SingleThread_Baseline` is 10.7% faster with c2. It clears its book before
  every iteration, and before the fix each iteration regrew the order index
  from 1 to 2,047 slots inside the timed region.
- `SweepLevels` also clears its book every iteration. Before the fix its index
  stayed at 3 to 31 slots after the first clear, because absl keeps a table
  under 128 slots; with c2 it keeps the reserved 131,071. It moves by −7% to
  +4%, in both directions.
