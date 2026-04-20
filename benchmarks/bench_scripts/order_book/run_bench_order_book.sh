#!/usr/bin/env bash
# run_bench_order_book.sh
#
# 编译（Release）并运行 bench_order_book，将结果保存到 results/ 目录。
#
# 命名规则：
#   order_book_<YYYY-MM-DD>_<HH-MM-SS>[_<TAG>].json
#
# 用法：
#   ./benchmarks/bench_scripts/run_bench_order_book.sh          # 无 tag
#   ./benchmarks/bench_scripts/run_bench_order_book.sh after_opt # 自定义 tag

set -euo pipefail

# ── 路径 ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/release"
RESULTS_DIR="${PROJECT_ROOT}/results"
BINARY="${BUILD_DIR}/bench_order_book"

# ── 参数 ─────────────────────────────────────────────────────────────────────
TAG="${1:-}"                          # 可选自定义标签，如 "after_opt"
TIMESTAMP="$(date +%Y-%m-%d_%H-%M-%S)"

if [[ -n "${TAG}" ]]; then
    OUTPUT_FILE="${RESULTS_DIR}/order_book_${TIMESTAMP}_${TAG}.json"
else
    OUTPUT_FILE="${RESULTS_DIR}/order_book_${TIMESTAMP}.json"
fi

# ── 编译 ─────────────────────────────────────────────────────────────────────
echo "==> Building bench_order_book (Release)..."
cmake --build "${BUILD_DIR}" --target bench_order_book -j"$(nproc)"

# ── 准备输出目录 ──────────────────────────────────────────────────────────────
mkdir -p "${RESULTS_DIR}"

# ── 运行 ─────────────────────────────────────────────────────────────────────
echo "==> Running bench_order_book..."
echo "    Output: ${OUTPUT_FILE}"
echo ""

# taskset -c 0 将进程绑定到 CPU 0，减少调度抖动
# 若系统不支持 taskset，则直接运行
if command -v taskset &>/dev/null; then
    taskset -c 0 "${BINARY}" \
        --benchmark_out_format=json \
        --benchmark_out="${OUTPUT_FILE}" \
        --benchmark_repetitions=5 \
        --benchmark_report_aggregates_only=true
else
    echo "    [warn] taskset not available, running without CPU pinning"
    "${BINARY}" \
        --benchmark_out_format=json \
        --benchmark_out="${OUTPUT_FILE}" \
        --benchmark_repetitions=5 \
        --benchmark_report_aggregates_only=true
fi

echo ""
echo "==> Done. Results saved to: ${OUTPUT_FILE}"
