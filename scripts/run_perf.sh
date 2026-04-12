#!/usr/bin/env bash
# =============================================================================
# run_perf.sh — 一键 perf profiling + FlameGraph 生成
#
# 用法：
#   ./scripts/run_perf.sh [benchmark_binary] [output_name] [bm_filter]
#
# 参数说明：
#   benchmark_binary  可执行文件路径（默认：./build/release/bench_order_book）
#   output_name       输出文件名前缀（默认：profile；有 filter 时自动从 filter 派生）
#   bm_filter         传给 --benchmark_filter 的正则（可选，不填则运行整个 benchmark）
#
# 示例：
#   # 运行整个 benchmark（全量 perf）
#   ./scripts/run_perf.sh ./build/release/bench_order_book orderbook
#
#   # 只 perf BM_MixedWorkload（输出文件自动命名为 *_BM_MixedWorkload.*）
#   ./scripts/run_perf.sh ./build/release/bench_order_book "" "BM_MixedWorkload"
#
#   # 同时 perf 多个 BM（用 | 分隔），手动指定输出名
#   ./scripts/run_perf.sh ./build/release/bench_order_book match \
#       "BM_AddOrder_FullMatch|BM_AddOrder_SweepLevels"
#
# 输出（无 filter）：
#   results/flamegraph_<output_name>.svg
#   results/perf_<output_name>.data
#   results/perf_report_<output_name>.txt
#
# 输出（有 filter，output_name 为空时自动派生）：
#   results/flamegraph_<output_name>.svg
#   results/perf_<output_name>.data
#   results/perf_report_<output_name>.txt
#   （output_name 由 filter 内容生成，特殊字符替换为 _）
#
# 依赖：
#   - perf (linux-tools-$(uname -r) 或 linux-tools-generic)
#   - FlameGraph (git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph)
#   - 需要 perf_event_paranoid <= 1：
#       echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
# =============================================================================

set -euo pipefail

# ── 参数处理 ──────────────────────────────────────────────────────────────────
BINARY="${1:-./build/release/bench_order_book}"
FILTER="${3:-}"   # 第三参数：benchmark_filter 正则（可选）

# NAME 推导规则：
#   - 若第二参数非空，直接用它
#   - 若第二参数为空 且 有 filter，从 filter 派生（去掉非字母数字字符，换成 _）
#   - 否则 fallback 到 "profile"
_RAW_NAME="${2:-}"
if [ -n "$_RAW_NAME" ]; then
    NAME="$_RAW_NAME"
elif [ -n "$FILTER" ]; then
    # 把 | / 空格 等特殊字符替换成 _ ，去掉首尾 _
    NAME=$(echo "$FILTER" | tr -cs '[:alnum:]' '_' | sed 's/^_//;s/_$//')
else
    NAME="profile"
fi

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
RESULTS_DIR="results"
PERF_FREQ=99        # 采样频率（Hz），99 而非 100 以避免与定时器对齐

# 有 filter 时用更长的 min_time（纯净信号），无 filter 用 5s
if [ -n "$FILTER" ]; then
    BENCHMARK_MIN_TIME="10s"
else
    BENCHMARK_MIN_TIME="5s"
fi

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
    echo "    binary       : $BINARY"
    echo "    output       : $PERF_DATA"
    echo "    freq         : ${PERF_FREQ} Hz"
    echo "    call-graph   : $CALL_GRAPH"
    echo "    min_time     : $BENCHMARK_MIN_TIME"
    if [ -n "$FILTER" ]; then
        echo "    filter       : $FILTER"
    else
        echo "    filter       : (全量，运行所有 BM)"
    fi
    echo ""

    # 构造 benchmark 参数（有 filter 时追加 --benchmark_filter）
    local BM_ARGS="--benchmark_min_time=${BENCHMARK_MIN_TIME}"
    if [ -n "$FILTER" ]; then
        BM_ARGS="--benchmark_filter=${FILTER} ${BM_ARGS}"
    fi

    # 绑定到核心 0，避免跨核迁移影响数据
    # perf record:
    #   -g             : 采集调用图（call graph）
    #   --call-graph   : 调用图展开方式（fp=frame pointer, dwarf=DWARF unwinding）
    #   -F             : 采样频率
    #   -o             : 输出文件
    #   taskset -c 0   : 绑核
    # shellcheck disable=SC2086
    taskset -c 0 perf record \
        -g \
        --call-graph "$CALL_GRAPH" \
        -F "$PERF_FREQ" \
        -o "$PERF_DATA" \
        -- "$BINARY" $BM_ARGS 2>/dev/null || \
    taskset -c 0 perf record \
        -g \
        --call-graph "$CALL_GRAPH" \
        -F "$PERF_FREQ" \
        -o "$PERF_DATA" \
        -- "$BINARY" $BM_ARGS

    echo "[✓] perf 数据已保存: $PERF_DATA"
}

# ── 生成 FlameGraph ────────────────────────────────────────────────────────────
generate_flamegraph() {
    local PERF_DATA="$RESULTS_DIR/perf_${NAME}.data"
    local FOLDED="$RESULTS_DIR/perf_${NAME}.folded"
    local SVG="$RESULTS_DIR/flamegraph_${NAME}.svg"

    # 火焰图标题：有 filter 时在标题中标注 BM 名
    local TITLE
    if [ -n "$FILTER" ]; then
        TITLE="Matching Engine — ${FILTER}"
    else
        TITLE="Matching Engine — ${NAME}"
    fi

    echo "[*] 生成 FlameGraph..."

    # step 1: 将 perf 数据转换为折叠格式
    perf script -i "$PERF_DATA" | \
        "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" > "$FOLDED"

    # step 2: 生成 SVG
    "$FLAMEGRAPH_DIR/flamegraph.pl" \
        --title "$TITLE" \
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
    local REPORT_TXT="$RESULTS_DIR/perf_report_${NAME}.txt"

    echo ""
    echo "[*] 热点函数摘要（Top 20）："
    echo "------------------------------------------------------------"

    # 生成完整文本报告并保存到文件
    {
        perf report -i "$PERF_DATA" \
            --stdio \
            --no-children \
            --sort=symbol \
            -n \
            2>/dev/null || \
        perf report -i "$PERF_DATA" --stdio 2>/dev/null
    } > "$REPORT_TXT"

    # 终端只显示前 40 行摘要
    head -40 "$REPORT_TXT"

    echo "[✓] 完整文本报告已保存: $REPORT_TXT"
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
    echo " 完成！输出文件前缀：${NAME}"
    echo ""
    echo " 查看 perf 报告（交互式）："
    echo "   perf report -i $RESULTS_DIR/perf_${NAME}.data"
    echo ""
    echo " 查看文本报告："
    echo "   cat $RESULTS_DIR/perf_report_${NAME}.txt"
    echo "   less $RESULTS_DIR/perf_report_${NAME}.txt"
    echo ""
    if [ "${SKIP_FLAMEGRAPH:-0}" -eq 0 ]; then
        echo " 查看火焰图："
        echo "   firefox $RESULTS_DIR/flamegraph_${NAME}.svg"
        echo "   （或任意浏览器打开 .svg 文件）"
    fi
    echo "============================================================"
}

main
