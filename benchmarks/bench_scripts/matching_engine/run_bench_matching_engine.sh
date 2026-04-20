#!/usr/bin/env bash
# run_bench_matching_engine.sh
#
# 编译（Release）并运行 bench_matching_engine，将结果保存到 results/ 目录。
# BM_Pipeline_LatencyReport 的延迟百分位报告同时输出到终端和日志文件。
#
# 命名规则：
#   matching_engine_<YYYY-MM-DD>_<HH-MM-SS>[_<TAG>].json
#   matching_engine_<YYYY-MM-DD>_<HH-MM-SS>[_<TAG>].log   （含延迟报告）
#
# 用法：
#   ./benchmarks/bench_scripts/matching_engine/run_bench_matching_engine.sh           # 无 tag
#   ./benchmarks/bench_scripts/matching_engine/run_bench_matching_engine.sh after_opt # 自定义 tag
#
# 注意：
#   流水线 benchmark 含生产者/消费者双线程，建议绑定两个物理核心（taskset -c 0,1），
#   确保两线程不竞争同一物理核心，以减少调度抖动。
#   BM_Pipeline_Throughput 和 BM_Pipeline_LatencyReport 在代码中已通过 Iterations()
#   固定迭代次数，脚本不再叠加 --benchmark_repetitions，避免与代码内设定冲突。

set -euo pipefail

# ── 路径 ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build/release"
RESULTS_DIR="${PROJECT_ROOT}/results"
BINARY="${BUILD_DIR}/bench_matching_engine"

# ── 参数 ─────────────────────────────────────────────────────────────────────
TAG="${1:-}"
TIMESTAMP="$(date +%Y-%m-%d_%H-%M-%S)"

if [[ -n "${TAG}" ]]; then
    OUTPUT_JSON="${RESULTS_DIR}/matching_engine_${TIMESTAMP}_${TAG}.json"
    OUTPUT_LOG="${RESULTS_DIR}/matching_engine_${TIMESTAMP}_${TAG}.log"
else
    OUTPUT_JSON="${RESULTS_DIR}/matching_engine_${TIMESTAMP}.json"
    OUTPUT_LOG="${RESULTS_DIR}/matching_engine_${TIMESTAMP}.log"
fi

# ── 编译 ─────────────────────────────────────────────────────────────────────
echo "==> Building bench_matching_engine (Release)..."
cmake --build "${BUILD_DIR}" --target bench_matching_engine -j"$(nproc)"

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

# ── 运行 ─────────────────────────────────────────────────────────────────────
echo "==> Running bench_matching_engine..."
echo "    JSON  : ${OUTPUT_JSON}"
echo "    Log   : ${OUTPUT_LOG}"
echo ""

# tee 同时将全部输出（含 BM_Pipeline_LatencyReport 打印的延迟百分位）
# 写到终端和 .log 文件；JSON 结果由 --benchmark_out 单独写入。
run_with_taskset 0,1 "${BINARY}" \
    --benchmark_out_format=json \
    --benchmark_out="${OUTPUT_JSON}" \
    2>&1 | tee "${OUTPUT_LOG}"

echo ""
echo "==> Done."
echo "    JSON results : ${OUTPUT_JSON}"
echo "    Full log     : ${OUTPUT_LOG}"
