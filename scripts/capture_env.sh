#!/usr/bin/env bash
# =============================================================================
# capture_env.sh — record everything needed to interpret a benchmark run
#
# A performance number without its environment is not evidence. This emits a
# JSON record of the commit, toolchain, build flags, CPU, frequency policy,
# thermal state and kernel settings in effect, to be archived next to the raw
# benchmark output.
#
# Usage:
#   ./scripts/capture_env.sh                        # JSON to stdout
#   ./scripts/capture_env.sh env.json               # JSON to file
#   ./scripts/capture_env.sh env.json build/release # read flags from that build
#
# Needs no root. See docs/AUDIT.md for why each field is recorded.
# =============================================================================

set -euo pipefail

OUT="${1:-}"
BUILD_DIR="${2:-build/release}"
CPU_DIR="/sys/devices/system/cpu"

rd() { cat "$1" 2>/dev/null || echo ""; }
esc() { printf '%s' "${1:-}" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/ /g'; }
kv()  { printf '    "%s": "%s"' "$1" "$(esc "${2:-}")"; }
kvn() { printf '    "%s": %s' "$1" "${2:-null}"; }

cache_get() {
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || { echo ""; return; }
    grep -m1 "^$1:" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || echo ""
}

# The flags that actually reached the compiler.
#
# CMakeCache.txt must NOT be trusted for this: CMake seeds
# CMAKE_CXX_FLAGS_<CONFIG> into the cache with its own defaults (-O3 -DNDEBUG
# for Release), and a plain set() in CMakeLists.txt shadows the cache entry
# without updating it. Reading the cache therefore reports -O3 while the
# compiler is actually invoked with -O2. compile_commands.json records the real
# command line, so it is the only reliable source.
actual_flags() {
    local cc="$BUILD_DIR/compile_commands.json"
    [ -f "$cc" ] || { echo ""; return; }
    command -v python3 >/dev/null 2>&1 || { echo ""; return; }
    python3 - "$cc" <<'PYEOF_FLAGS'
import json, sys
try:
    entries = json.load(open(sys.argv[1]))
except Exception:
    raise SystemExit(0)
for e in entries:
    f = e.get("file", "")
    if "_deps" in f or not f.endswith(".cpp"):
        continue
    cmd = e.get("command") or " ".join(e.get("arguments", []))
    keep = [t for t in cmd.split()
            if t.startswith(("-O", "-std=", "-D", "-march", "-mtune",
                             "-flto", "-fsanitize", "-fno-", "-g"))]
    print(" ".join(keep))
    break
PYEOF_FLAGS
}

online_cpus() {
    for d in "$CPU_DIR"/cpu[0-9]*; do
        local id="${d##*/cpu}"
        if [ ! -e "$d/online" ] || [ "$(rd "$d/online")" = "1" ]; then echo "$id"; fi
    done
}

# Throttle counters must be de-duplicated before aggregation:
#   core_throttle_count    is per physical core, reported by BOTH SMT siblings
#   package_throttle_count is package-wide, reported identically by EVERY cpu
# Summing either one multiplies a single event by the number of reporters.
first_sibling() {  # $1 = cpu id -> lowest-numbered thread of its physical core
    local l f
    l="$(rd "$CPU_DIR/cpu$1/topology/thread_siblings_list")"
    [ -z "$l" ] && { echo "$1"; return; }
    f="${l%%,*}"; echo "${f%%-*}"
}

throttle_core_total() {   # one sample per physical core
    local total=0 v
    for c in $(online_cpus); do
        [ "$c" = "$(first_sibling "$c")" ] || continue
        v="$(rd "$CPU_DIR/cpu$c/thermal_throttle/core_throttle_count")"
        [ -n "$v" ] && total=$((total + v))
    done
    echo "$total"
}

throttle_package() {      # a single sample for the whole package
    local v
    for c in $(online_cpus); do
        v="$(rd "$CPU_DIR/cpu$c/thermal_throttle/package_throttle_count")"
        [ -n "$v" ] && { echo "$v"; return; }
    done
    echo 0
}

# ── gather ───────────────────────────────────────────────────────────────────

GIT_SHA="$(git rev-parse HEAD 2>/dev/null || echo '')"
GIT_SHORT="$(git rev-parse --short HEAD 2>/dev/null || echo '')"
GIT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '')"
GIT_DIRTY=false
[ -n "$(git status --porcelain 2>/dev/null)" ] && GIT_DIRTY=true
GIT_DESC="$(git describe --tags --always --dirty 2>/dev/null || echo '')"

CXX_COMPILER="$(cache_get CMAKE_CXX_COMPILER)"
[ -z "$CXX_COMPILER" ] && CXX_COMPILER="$(command -v c++ || echo '')"
CXX_VERSION="$("$CXX_COMPILER" --version 2>/dev/null | head -1 || echo '')"
BUILD_TYPE="$(cache_get CMAKE_BUILD_TYPE)"
CXX_STD="$(cache_get CMAKE_CXX_STANDARD)"
BUILD_FLAGS="$(actual_flags)"
BUILD_FLAGS_SOURCE="compile_commands.json"
if [ -z "$BUILD_FLAGS" ]; then
    case "$(printf '%s' "$BUILD_TYPE" | tr '[:lower:]' '[:upper:]')" in
        RELEASE) BUILD_FLAGS="$(cache_get CMAKE_CXX_FLAGS_RELEASE)" ;;
        DEBUG)   BUILD_FLAGS="$(cache_get CMAKE_CXX_FLAGS_DEBUG)"   ;;
        *)       BUILD_FLAGS="$(cache_get CMAKE_CXX_FLAGS)"          ;;
    esac
    BUILD_FLAGS_SOURCE="CMakeCache.txt (UNRELIABLE: may be a shadowed default)"
fi
CXX_STD_ACTUAL="$(printf '%s' "$BUILD_FLAGS" | tr ' ' '\n' | grep -m1 '^-std=' | sed 's/^-std=//' || true)"
[ -n "$CXX_STD_ACTUAL" ] && CXX_STD="$CXX_STD_ACTUAL"
ABSL_DIR="$(cache_get absl_DIR)"
ABSL_SOURCE="system"
[ -z "$ABSL_DIR" ] && ABSL_SOURCE="source-build"

CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
ONLINE="$(online_cpus | tr '\n' ',' | sed 's/,$//')"
SIBLINGS="$(for d in "$CPU_DIR"/cpu[0-9]*; do
    id="${d##*/cpu}"; l="$(rd "$d/topology/thread_siblings_list")"
    [ -n "$l" ] && printf '%s=%s;' "$id" "$l"; done)"
GOVERNOR="$(rd "$CPU_DIR/cpu0/cpufreq/scaling_governor")"
DRIVER="$(rd "$CPU_DIR/cpu0/cpufreq/scaling_driver")"
NO_TURBO="$(rd "$CPU_DIR/intel_pstate/no_turbo")"
MIN_PCT="$(rd "$CPU_DIR/intel_pstate/min_perf_pct")"
MAX_PCT="$(rd "$CPU_DIR/intel_pstate/max_perf_pct")"
FREQ_MHZ="$(for c in $(online_cpus); do
    printf '%s:%s;' "$c" "$(( $(rd "$CPU_DIR/cpu$c/cpufreq/scaling_cur_freq" || echo 0) / 1000 ))"; done)"

TEMPS="$(for z in /sys/class/thermal/thermal_zone*/; do
    t="$(rd "$z/type")"; v="$(rd "$z/temp")"
    [ -n "$v" ] && printf '%s:%s;' "$t" "$((v/1000))"; done)"

BENCH_MODE=false
STATE_FILE=/var/tmp/matching_machine_bench_env.state
[ -f "$STATE_FILE" ] && BENCH_MODE=true
state_get() { [ -f "$STATE_FILE" ] && grep -m1 "^$1=" "$STATE_FILE" | cut -d= -f2- | sed 's/^"//; s/"$//' || true; }

# Which cores a run is pinned to, and why. In measurement mode this is what
# bench_env.sh setup chose (and steered interrupts away from); otherwise the
# same ranking computed live. The ranking is recorded so an archive can show
# why those cores were used rather than merely which.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MEASURE_CPUS="$("$SCRIPT_DIR/bench_env.sh" cores --list 2>/dev/null || true)"
MEASURE_RATIONALE="$("$SCRIPT_DIR/bench_env.sh" cores --rationale 2>/dev/null || true)"
if [ "$BENCH_MODE" = true ]; then
    MEASURE_SOURCE="bench_env.sh setup: $(state_get MEASURE_SOURCE)"
else
    MEASURE_SOURCE="computed live (measurement mode inactive; interrupts not steered)"
fi
IRQ_MOVED="$(state_get IRQ_MOVED)"
IRQ_REFUSED="$(state_get IRQ_REFUSED)"
IRQ_RESIDUAL="$(state_get IRQ_RESIDUAL)"
# device (numbered) interrupts per online CPU since boot; header-mapped columns
DEVICE_IRQS="$(awk 'NR == 1 { for (i = 1; i <= NF; i++) { c = $i; sub(/^CPU/, "", c); col[i] = c } n = NF; next }
    $1 ~ /^[0-9]+:$/ { for (i = 1; i <= n; i++) { v = $(i + 1); if (v ~ /^[0-9]+$/) t[col[i]] += v } }
    END { for (k in t) printf "%s:%s;", k, t[k] }' /proc/interrupts)"

# ── emit ─────────────────────────────────────────────────────────────────────

{
printf '{\n'
printf '  "captured_at": "%s",\n' "$(date -Is)"
printf '  "git": {\n'
kv  commit "$GIT_SHA";      printf ',\n'
kv  short "$GIT_SHORT";     printf ',\n'
kv  branch "$GIT_BRANCH";   printf ',\n'
kv  describe "$GIT_DESC";   printf ',\n'
kvn dirty "$GIT_DIRTY";     printf '\n  },\n'
printf '  "toolchain": {\n'
kv  compiler "$CXX_COMPILER";   printf ',\n'
kv  compiler_version "$CXX_VERSION"; printf ',\n'
kv  cmake "$(cmake --version 2>/dev/null | head -1)"; printf ',\n'
kv  build_type "$BUILD_TYPE";   printf ',\n'
kv  cxx_standard "$CXX_STD";    printf ',\n'
kv  build_flags "$BUILD_FLAGS"; printf ',\n'
kv  build_flags_source "$BUILD_FLAGS_SOURCE"; printf ',\n'
kv  build_dir "$BUILD_DIR"; printf ',\n'
kv  abseil "$ABSL_SOURCE";      printf ',\n'
kv  abseil_dir "$ABSL_DIR";     printf '\n  },\n'
printf '  "os": {\n'
kv  kernel "$(uname -r)";  printf ',\n'
kv  uname "$(uname -a)";   printf ',\n'
kv  distro "$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"; printf '\n  },\n'
printf '  "cpu": {\n'
kv  model "$CPU_MODEL";         printf ',\n'
kv  online "$ONLINE";           printf ',\n'
kv  thread_siblings "$SIBLINGS";printf ',\n'
kvn nproc "$(nproc)";           printf '\n  },\n'
printf '  "frequency": {\n'
kv  governor "$GOVERNOR";  printf ',\n'
kv  driver "$DRIVER";      printf ',\n'
kv  no_turbo "$NO_TURBO";  printf ',\n'
kv  min_perf_pct "$MIN_PCT"; printf ',\n'
kv  max_perf_pct "$MAX_PCT"; printf ',\n'
kv  current_mhz "$FREQ_MHZ"; printf '\n  },\n'
printf '  "thermal": {\n'
kvn core_throttle_total "$(throttle_core_total)";   printf ',\n'
kvn package_throttle "$(throttle_package)";         printf ',\n'
kv  zones_celsius "$TEMPS"; printf '\n  },\n'
printf '  "kernel_settings": {\n'
kv  perf_event_paranoid "$(rd /proc/sys/kernel/perf_event_paranoid)"; printf ',\n'
kv  transparent_hugepage "$(rd /sys/kernel/mm/transparent_hugepage/enabled)"; printf ',\n'
kv  loadavg "$(cut -d' ' -f1-3 /proc/loadavg)"; printf ',\n'
kvn bench_env_active "$BENCH_MODE"; printf '\n  },\n'
printf '  "measurement": {\n'
kv  cores "$MEASURE_CPUS";                printf ',\n'
kv  selection_source "$MEASURE_SOURCE";   printf ',\n'
kv  core_ranking "$MEASURE_RATIONALE";    printf ',\n'
kv  irq_steered_moved "$IRQ_MOVED";       printf ',\n'
kv  irq_steered_refused "$IRQ_REFUSED";   printf ',\n'
kv  irq_residual_on_cores "$IRQ_RESIDUAL"; printf ',\n'
kv  device_irqs_by_cpu "$DEVICE_IRQS";    printf '\n  }\n'
printf '}\n'
} > "${OUT:-/dev/stdout}"

[ -n "$OUT" ] && echo "[+] environment captured -> $OUT" >&2
exit 0
