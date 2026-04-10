#!/usr/bin/env bash
# run_bench_spsc_ring_buffer.sh
#
# 编译（Release）并运行 bench_spsc_ring_buffer，将结果保存到 results/ 目录。
#
# 命名规则：
#   spsc_ring_buffer_<YYYY-MM-DD>_<HH-MM-SS>[_<TAG>].json
#
# 用法：
#   ./benchmarks/bench_scripts/run_bench_spsc_ring_buffer.sh           # 无 tag
#   ./benchmarks/bench_scripts/run_bench_spsc_ring_buffer.sh baseline  # 自定义 tag
#
# 注意：
#   双线程 benchmark 建议绑定两个物理核心（taskset -c 0,1），
#   确保生产者与消费者不共享同一物理核心。

set -euo pipefail

# ── 路径 ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/release"
RESULTS_DIR="${PROJECT_ROOT}/results"
BINARY="${BUILD_DIR}/bench_spsc_ring_buffer"

# ── 参数 ─────────────────────────────────────────────────────────────────────
TAG="${1:-}"
TIMESTAMP="$(date +%Y-%m-%d_%H-%M-%S)"

if [[ -n "${TAG}" ]]; then
    OUTPUT_FILE="${RESULTS_DIR}/spsc_ring_buffer_${TIMESTAMP}_${TAG}.json"
else
    OUTPUT_FILE="${RESULTS_DIR}/spsc_ring_buffer_${TIMESTAMP}.json"
fi

# ── 编译 ─────────────────────────────────────────────────────────────────────
echo "==> Building bench_spsc_ring_buffer (Release)..."
cmake --build "${BUILD_DIR}" --target bench_spsc_ring_buffer -j"$(nproc)"

# ── 准备输出目录 ──────────────────────────────────────────────────────────────
mkdir -p "${RESULTS_DIR}"

# ── 运行 ─────────────────────────────────────────────────────────────────────
echo "==> Running bench_spsc_ring_buffer..."
echo "    Output: ${OUTPUT_FILE}"
echo ""

# 双线程 benchmark 绑定 0,1 两个核心；若无 taskset 则直接运行
if command -v taskset &>/dev/null; then
    taskset -c 0,1 "${BINARY}" \
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
