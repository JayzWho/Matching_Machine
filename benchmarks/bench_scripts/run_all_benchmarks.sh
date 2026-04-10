#!/usr/bin/env bash
# run_all_benchmarks.sh
#
# 一键编译（Release）并运行全部 benchmark，结果分别保存到 results/ 目录。
# 两个 benchmark 使用相同的时间戳和 tag，便于对比同一次运行的结果。
#
# 命名规则：
#   order_book_<YYYY-MM-DD>_<HH-MM-SS>[_<TAG>].json
#   spsc_ring_buffer_<YYYY-MM-DD>_<HH-MM-SS>[_<TAG>].json
#
# 用法：
#   ./benchmarks/bench_scripts/run_all_benchmarks.sh             # 无 tag
#   ./benchmarks/bench_scripts/run_all_benchmarks.sh after_opt   # 自定义 tag

set -euo pipefail

# ── 路径 ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/release"
RESULTS_DIR="${PROJECT_ROOT}/results"

# ── 参数 ─────────────────────────────────────────────────────────────────────
TAG="${1:-}"
TIMESTAMP="$(date +%Y-%m-%d_%H-%M-%S)"

if [[ -n "${TAG}" ]]; then
    OB_OUT="${RESULTS_DIR}/order_book_${TIMESTAMP}_${TAG}.json"
    SPSC_OUT="${RESULTS_DIR}/spsc_ring_buffer_${TIMESTAMP}_${TAG}.json"
else
    OB_OUT="${RESULTS_DIR}/order_book_${TIMESTAMP}.json"
    SPSC_OUT="${RESULTS_DIR}/spsc_ring_buffer_${TIMESTAMP}.json"
fi

# ── 编译全部 benchmark target ─────────────────────────────────────────────────
echo "==> Building all benchmarks (Release)..."
cmake --build "${BUILD_DIR}" --target bench_order_book bench_spsc_ring_buffer -j"$(nproc)"
echo ""

# ── 准备输出目录 ──────────────────────────────────────────────────────────────
mkdir -p "${RESULTS_DIR}"

# ── 辅助函数：带 taskset 的运行 ───────────────────────────────────────────────
run_with_taskset() {
    local cpus="$1"; shift
    if command -v taskset &>/dev/null; then
        taskset -c "${cpus}" "$@"
    else
        echo "    [warn] taskset not available, running without CPU pinning"
        "$@"
    fi
}

# ── 1. bench_order_book ───────────────────────────────────────────────────────
echo "==> [1/2] Running bench_order_book..."
echo "    Output: ${OB_OUT}"
echo ""
run_with_taskset 0 "${BUILD_DIR}/bench_order_book" \
    --benchmark_out_format=json \
    --benchmark_out="${OB_OUT}" \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true
echo ""

# ── 2. bench_spsc_ring_buffer ─────────────────────────────────────────────────
echo "==> [2/2] Running bench_spsc_ring_buffer..."
echo "    Output: ${SPSC_OUT}"
echo ""
run_with_taskset 0,1 "${BUILD_DIR}/bench_spsc_ring_buffer" \
    --benchmark_out_format=json \
    --benchmark_out="${SPSC_OUT}" \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true
echo ""

# ── 汇总 ─────────────────────────────────────────────────────────────────────
echo "============================================================"
echo "  All benchmarks complete."
echo "  Results directory: ${RESULTS_DIR}/"
echo "    $(basename "${OB_OUT}")"
echo "    $(basename "${SPSC_OUT}")"
echo "============================================================"
