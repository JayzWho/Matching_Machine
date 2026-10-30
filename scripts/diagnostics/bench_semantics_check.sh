#!/usr/bin/env bash
# Rough check of the single-threaded benchmark changes, outside measurement
# mode: master and the branch, six runs each, alternating, one repetition per
# run. It covers the benchmarks whose work the changes do not mean to alter:
# BM_AddOrder_NoMatch (BM_AddOrder_NoMatch_V3 on the branch), the
# single-threaded baseline and the SPSC pipeline throughput.
#
# Usage: bench_semantics_check.sh BUILDS OUT
#   BUILDS holds master/ and new/, each with bench_order_book and
#   bench_matching_engine built in Release from master and from the branch.
#   OUT receives one JSON file per run.
set -euo pipefail
builds=$1 out=$2
mkdir -p "$out"
variants=(master new)
for round in 1 2 3 4 5 6; do
  for k in 0 1; do
    v=${variants[$(( (k + round) % 2 ))]}
    taskset -c 2 "$builds/$v/bench_order_book" --benchmark_filter='BM_AddOrder_NoMatch($|_V3)' \
      --benchmark_out="$out/${v}_r${round}_ob.json" --benchmark_out_format=json > /dev/null
    taskset -c 2,3 "$builds/$v/bench_matching_engine" --benchmark_filter='SingleThread|Pipeline_Throughput' \
      --benchmark_out="$out/${v}_r${round}_me.json" --benchmark_out_format=json > /dev/null
  done
done
