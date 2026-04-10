#!/usr/bin/env bash
# =============================================================================
# show_bench.sh — 将 Google Benchmark JSON 格式化为人类可读的表格
#
# 用法：
#   ./scripts/show_bench.sh <result.json> [compare_result.json]
#
# 示例：
#   ./scripts/show_bench.sh results/order_book_*.json
#   ./scripts/show_bench.sh results/before.json results/after.json
#
# 依赖：jq (sudo apt install jq)
# =============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

if ! command -v jq &>/dev/null; then
    echo "错误：需要安装 jq：sudo apt install jq"
    exit 1
fi

# ──────────────────────────────────────────────────────────────────
# 单文件展示模式
# ──────────────────────────────────────────────────────────────────
show_single() {
    local FILE="$1"

    # 打印环境信息
    echo -e "${BOLD}============================================================${NC}"
    jq -r '
      .context |
      "  文件     : '"$FILE"'",
      "  主机     : \(.host_name)",
      "  CPU      : \(.num_cpus) × \(.mhz_per_cpu) MHz",
      "  时间     : \(.date)",
      "  编译模式 : \(.library_build_type)"
    ' "$FILE"
    echo -e "${BOLD}============================================================${NC}"
    echo ""

    # 打印表头
    printf "${CYAN}${BOLD}%-45s %12s %12s %14s %6s${NC}\n" \
        "Benchmark" "cpu_time" "real_time" "throughput" "CV%"
    printf '%s\n' "$(printf '─%.0s' {1..95})"

    # 提取每个 benchmark 的 mean/stddev，计算 CV
    jq -r '
      # 构建 map: run_name -> {mean_cpu, mean_real, items, stddev_real}
      reduce .benchmarks[] as $b (
        {};
        if $b.aggregate_name == "mean" then
          .[$b.run_name] += {
            "mean_cpu":  $b.cpu_time,
            "mean_real": $b.real_time,
            "items":     ($b.items_per_second // 0),
            "time_unit": $b.time_unit
          }
        elif $b.aggregate_name == "stddev" then
          .[$b.run_name] += {"stddev_real": $b.real_time}
        else . end
      )
      | to_entries[]
      | .key as $name
      | .value
      | [
          $name,
          # cpu_time 格式化
          (if .mean_cpu >= 1e6 then ((.mean_cpu/1e3 | (.*10|round)/10 | tostring) + " μs")
           else ((.mean_cpu | (.*10|round)/10 | tostring) + " ns") end),
          # real_time 格式化
          (if .mean_real >= 1e6 then ((.mean_real/1e3 | (.*10|round)/10 | tostring) + " μs")
           else ((.mean_real | (.*10|round)/10 | tostring) + " ns") end),
          # throughput
          (if .items > 0 then
            if .items >= 1e9 then ((.items/1e9 | (.*100|round)/100 | tostring) + " G/s")
            elif .items >= 1e6 then ((.items/1e6 | (.*100|round)/100 | tostring) + " M/s")
            elif .items >= 1e3 then ((.items/1e3 | (.*10|round)/10 | tostring) + " k/s")
            else (.items | tostring) + " /s"
            end
          else "-" end),
          # CV%
          (if .stddev_real and .mean_real > 0 then
            ((.stddev_real / .mean_real * 100) | (.*10|round)/10 | tostring) + "%"
          else "-" end)
        ]
      | @tsv
    ' "$FILE" | while IFS=$'\t' read -r name cpu real tput cv; do
        # CV 高于 10% 用黄色标注
        cv_color="$NC"
        cv_float="${cv//%/}"
        if [[ "$cv" != "-" ]] && (( $(echo "$cv_float > 10" | bc -l 2>/dev/null || echo 0) )); then
            cv_color="$YELLOW"
        fi
        printf "%-45s %12s %12s %14s ${cv_color}%6s${NC}\n" \
            "$name" "$cpu" "$real" "$tput" "$cv"
    done

    echo ""
}

# ──────────────────────────────────────────────────────────────────
# 对比模式：两个 json，显示变化百分比
# ──────────────────────────────────────────────────────────────────
show_compare() {
    local FILE_A="$1"
    local FILE_B="$2"

    echo -e "${BOLD}============================================================${NC}"
    echo -e "  对比: ${CYAN}$FILE_A${NC}  vs  ${CYAN}$FILE_B${NC}"
    echo -e "${BOLD}============================================================${NC}"
    echo ""

    printf "${CYAN}${BOLD}%-45s %12s %12s %10s${NC}\n" \
        "Benchmark" "Before(ns)" "After(ns)" "变化"
    printf '%s\n' "$(printf '─%.0s' {1..85})"

    # 提取 A 的 mean
    declare -A before_cpu
    while IFS=$'\t' read -r name val; do
        before_cpu["$name"]="$val"
    done < <(jq -r '.benchmarks[] | select(.aggregate_name=="mean") | [.run_name, (.cpu_time|tostring)] | @tsv' "$FILE_A")

    # 提取 B 的 mean，对比输出
    jq -r '.benchmarks[] | select(.aggregate_name=="mean") | [.run_name, (.cpu_time|tostring)] | @tsv' "$FILE_B" \
    | while IFS=$'\t' read -r name after; do
        before="${before_cpu[$name]:-}"
        if [[ -z "$before" ]]; then
            printf "%-45s %12s %12s %10s\n" "$name" "-" "$(printf '%.1f' "$after") ns" "(新增)"
        else
            pct=$(echo "scale=1; ($after - $before) / $before * 100" | bc 2>/dev/null || echo "?")
            if (( $(echo "$pct < -5" | bc -l 2>/dev/null || echo 0) )); then
                color="$GREEN"  # 变快
            elif (( $(echo "$pct > 5" | bc -l 2>/dev/null || echo 0) )); then
                color="$RED"    # 变慢
            else
                color="$NC"
            fi
            printf "%-45s %12.1f %12.1f ${color}%+9.1f%%${NC}\n" \
                "$name" "$before" "$after" "$pct"
        fi
    done
    echo ""
}

# ──────────────────────────────────────────────────────────────────
# 主流程
# ──────────────────────────────────────────────────────────────────
if [[ $# -eq 0 ]]; then
    echo "用法: $0 <result.json> [compare_result.json]"
    echo "示例: $0 results/order_book_*.json"
    exit 1
elif [[ $# -eq 1 ]]; then
    show_single "$1"
elif [[ $# -eq 2 ]]; then
    show_single "$1"
    show_single "$2"
    show_compare "$1" "$2"
fi
