#!/usr/bin/env bash
# =============================================================================
# run_perf.sh — 一键 perf profiling + FlameGraph 生成
#
# 用法：
#   ./scripts/run_perf.sh [benchmark_binary] [output_name]
#
# 示例：
#   ./scripts/run_perf.sh ./build/release/bench_order_book orderbook
#   ./scripts/run_perf.sh ./build/release/matching_engine demo
#
# 依赖：
#   - perf (linux-tools-$(uname -r) 或 linux-tools-generic)
#   - FlameGraph (git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph)
#   - 需要 perf_event_paranoid <= 1：
#       echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
#
# 输出：
#   results/flamegraph_<output_name>.svg  — 可在浏览器中交互查看
#   results/perf_<output_name>.data       — 原始 perf 数据（可用 perf report 查看）
# =============================================================================

set -euo pipefail

# ── 参数处理 ──────────────────────────────────────────────────────────────────
BINARY="${1:-./build/release/bench_order_book}"
NAME="${2:-profile}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
RESULTS_DIR="results"
PERF_FREQ=99        # 采样频率（Hz），99 而非 100 以避免与定时器对齐
PERF_DURATION=10    # 最长采样时间（秒）

# ── 环境检查 ──────────────────────────────────────────────────────────────────
check_deps() {
    echo "[*] 检查依赖..."

    if ! command -v perf &>/dev/null; then
        echo "[!] 未找到 perf。请安装："
        echo "    sudo apt install linux-tools-generic linux-tools-$(uname -r)"
        exit 1
    fi

    PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "99")
    if [ "$PARANOID" -gt 1 ]; then
        echo "[!] perf_event_paranoid = $PARANOID，需要 <= 1 才能采集调用栈"
        echo "    临时解决："
        echo "    echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid"
        echo ""
        echo "    如果无 sudo 权限，可以用 --call-graph=dwarf（较慢但无需权限）"
        echo "    尝试继续（使用 dwarf 模式）..."
        CALL_GRAPH="dwarf"
    else
        CALL_GRAPH="fp"  # frame pointer（更快，需要 -fno-omit-frame-pointer 编译）
    fi

    if [ ! -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]; then
        echo "[!] 未找到 FlameGraph。请克隆："
        echo "    git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph"
        echo ""
        echo "    跳过 SVG 生成，只保存原始 perf 数据..."
        SKIP_FLAMEGRAPH=1
    else
        SKIP_FLAMEGRAPH=0
    fi

    if [ ! -f "$BINARY" ]; then
        echo "[!] 未找到可执行文件: $BINARY"
        echo "    请先编译 Release 版本："
        echo "    cmake --build build/release -j\$(nproc)"
        exit 1
    fi
}

# ── 运行 perf record ──────────────────────────────────────────────────────────
run_perf() {
    mkdir -p "$RESULTS_DIR"
    local PERF_DATA="$RESULTS_DIR/perf_${NAME}.data"

    echo "[*] 开始 perf profiling..."
    echo "    binary : $BINARY"
    echo "    output : $PERF_DATA"
    echo "    freq   : ${PERF_FREQ} Hz"
    echo "    call-graph: $CALL_GRAPH"
    echo ""

    # 绑定到核心 0，避免跨核迁移影响数据
    # perf record:
    #   -g             : 采集调用图（call graph）
    #   --call-graph   : 调用图展开方式（fp=frame pointer, dwarf=DWARF unwinding）
    #   -F             : 采样频率
    #   -o             : 输出文件
    #   taskset -c 0   : 绑核
    taskset -c 0 perf record \
        -g \
        --call-graph "$CALL_GRAPH" \
        -F "$PERF_FREQ" \
        -o "$PERF_DATA" \
        -- "$BINARY" --benchmark_min_time=5 2>/dev/null || \
    taskset -c 0 perf record \
        -g \
        --call-graph "$CALL_GRAPH" \
        -F "$PERF_FREQ" \
        -o "$PERF_DATA" \
        -- "$BINARY"

    echo "[✓] perf 数据已保存: $PERF_DATA"
}

# ── 生成 FlameGraph ────────────────────────────────────────────────────────────
generate_flamegraph() {
    local PERF_DATA="$RESULTS_DIR/perf_${NAME}.data"
    local FOLDED="$RESULTS_DIR/perf_${NAME}.folded"
    local SVG="$RESULTS_DIR/flamegraph_${NAME}.svg"

    echo "[*] 生成 FlameGraph..."

    # step 1: 将 perf 数据转换为折叠格式
    perf script -i "$PERF_DATA" | \
        "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" > "$FOLDED"

    # step 2: 生成 SVG
    "$FLAMEGRAPH_DIR/flamegraph.pl" \
        --title "Matching Engine — $NAME" \
        --width 1600 \
        --colors hot \
        "$FOLDED" > "$SVG"

    rm -f "$FOLDED"  # 清理中间文件

    echo "[✓] FlameGraph 已生成: $SVG"
    echo "    在浏览器中打开查看（支持点击缩放）"
}

# ── perf report 摘要 ─────────────────────────────────────────────────────────
show_report() {
    local PERF_DATA="$RESULTS_DIR/perf_${NAME}.data"

    echo ""
    echo "[*] 热点函数摘要（Top 20）："
    echo "------------------------------------------------------------"
    perf report -i "$PERF_DATA" \
        --stdio \
        --no-children \
        --sort=symbol \
        -n \
        2>/dev/null | head -40 || \
    perf report -i "$PERF_DATA" --stdio 2>/dev/null | head -40
}

# ── 主流程 ────────────────────────────────────────────────────────────────────
main() {
    echo "============================================================"
    echo " Matching Engine — perf Profiling Script"
    echo "============================================================"
    echo ""

    check_deps
    run_perf

    if [ "${SKIP_FLAMEGRAPH:-0}" -eq 0 ]; then
        generate_flamegraph
    fi

    show_report

    echo ""
    echo "============================================================"
    echo " 完成！"
    echo ""
    echo " 查看 perf 报告（交互式）："
    echo "   perf report -i $RESULTS_DIR/perf_${NAME}.data"
    echo ""
    if [ "${SKIP_FLAMEGRAPH:-0}" -eq 0 ]; then
        echo " 查看火焰图："
        echo "   firefox $RESULTS_DIR/flamegraph_${NAME}.svg"
        echo "   （或任意浏览器打开 .svg 文件）"
    fi
    echo "============================================================"
}

main
