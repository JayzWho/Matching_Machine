#!/usr/bin/env bash
# Rough check of the order book fixes, outside measurement mode: three builds,
# six runs each, in an order that rotates every round, one repetition per run.
#
# Usage: order_book_fix_check.sh BUILDS OUT
#   BUILDS holds base/, c1/ and c2/, each with bench_order_book and
#   bench_matching_engine built from fb5f8e6, a3444c7 and 51201a0 (Release).
#   OUT receives one JSON file per run.
set -euo pipefail
builds=$1 out=$2
mkdir -p "$out"
variants=(base c1 c2)
for round in 1 2 3 4 5 6; do
  for k in 0 1 2; do
    v=${variants[$(( (k + round) % 3 ))]}
    taskset -c 2 "$builds/$v/bench_order_book" --benchmark_filter='SweepLevels' \
      --benchmark_out="$out/${v}_r${round}_ob.json" --benchmark_out_format=json > /dev/null
    taskset -c 2,3 "$builds/$v/bench_matching_engine" --benchmark_filter='SingleThread|Pipeline_Throughput' \
      --benchmark_out="$out/${v}_r${round}_me.json" --benchmark_out_format=json > /dev/null
  done
done
