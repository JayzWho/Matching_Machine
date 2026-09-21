#!/usr/bin/env bash
# =============================================================================
# run_anchor.sh — capture the pre-fix anchor measurement
#
# Runs the benchmark suite against the CURRENT, still-defective code on this
# machine. The result is NOT a performance claim. It is the control group.
#
# Why this exists: the environment that produced the numbers in README section 4
# (a Tencent Cloud VM) has been decommissioned, so those numbers can never be
# reproduced. Without a measurement of the unfixed code on THIS machine, every
# later comparison would confound two changes at once — the code and the
# hardware — and no improvement could be attributed to either. See docs/AUDIT.md.
#
# Usage:
#   ./scripts/run_anchor.sh [label]          # label defaults to 'v1-asis'
#   ME_REPETITIONS=3 ./scripts/run_anchor.sh # fewer repetitions (smoke run)
#   ME_DRY_RUN=1 ./scripts/run_anchor.sh     # validate wiring, discard output
#   ME_ALLOW_UNPINNED=1 ./scripts/run_anchor.sh   # run without measurement mode
#                                                 # (output is marked INVALID)
#
# Output: docs/evidence/<host-cpu>/<date>_<sha>_<label>/
#   env_before.json / env_after.json   full environment metadata
#   throttle_before.txt / _after.txt   thermal throttle counters
#   <bench>.json / <bench>.log         raw benchmark output
#   RESULT.md                          provenance, validity verdict, caveats
# =============================================================================

set -euo pipefail

LABEL="${1:-v1-asis}"
REPETITIONS="${ME_REPETITIONS:-10}"
DRY_RUN="${ME_DRY_RUN:-0}"
ALLOW_UNPINNED="${ME_ALLOW_UNPINNED:-0}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/release"
STATE_FILE="/var/tmp/matching_machine_bench_env.state"

cd "$ROOT"

die() { echo "[!] $*" >&2; exit 1; }

# ── preflight ────────────────────────────────────────────────────────────────

PINNED=1
if [ ! -f "$STATE_FILE" ]; then
    if [ "$ALLOW_UNPINNED" = "1" ] || [ "$DRY_RUN" = "1" ]; then
        PINNED=0
        echo "[!] measurement mode is NOT active — results will be marked INVALID"
    else
        die "measurement mode is not active.
    Run:  sudo ./scripts/bench_env.sh setup
    Or set ME_ALLOW_UNPINNED=1 to proceed anyway (output marked INVALID)."
    fi
fi

for b in bench_order_book bench_spsc_ring_buffer bench_matching_engine bench_baseline_mutex; do
    [ -x "$BUILD_DIR/$b" ] || die "missing $BUILD_DIR/$b — build Release first"
done

# Two distinct physical cores: in measurement mode SMT siblings are offline, so
# the lowest two online CPUs are on different cores. Derived, never hardcoded.
mapfile -t ONLINE < <(
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        id="${d##*/cpu}"
        if [ ! -e "$d/online" ] || [ "$(cat "$d/online" 2>/dev/null)" = "1" ]; then echo "$id"; fi
    done | sort -n
)
[ "${#ONLINE[@]}" -ge 2 ] || die "need at least 2 online CPUs"
CPU_SOLO="${ONLINE[0]}"
CPU_PAIR="${ONLINE[0]},${ONLINE[1]}"

GIT_SHA="$(git rev-parse --short HEAD)"
GIT_DIRTY=""
[ -n "$(git status --porcelain)" ] && GIT_DIRTY="-dirty"
CPU_TAG="$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //; s/(R)//g; s/(TM)//g; s/ CPU.*//; s/ @.*//; s/ \+/-/g; s/^-//')"
STAMP="$(date +%Y-%m-%d_%H-%M-%S)"

OUT="$ROOT/docs/evidence/${CPU_TAG}/${STAMP}_${GIT_SHA}${GIT_DIRTY}_${LABEL}"
[ "$DRY_RUN" = "1" ] && OUT="$(mktemp -d)/dryrun"
mkdir -p "$OUT"

echo "=============================================================="
echo "  anchor measurement"
echo "  label        : $LABEL"
echo "  commit       : ${GIT_SHA}${GIT_DIRTY}"
echo "  pinned       : $([ "$PINNED" = 1 ] && echo yes || echo 'NO — INVALID')"
echo "  repetitions  : $REPETITIONS"
echo "  single-thread: taskset -c $CPU_SOLO"
echo "  two-thread   : taskset -c $CPU_PAIR"
echo "  output       : $OUT"
echo "=============================================================="

./scripts/capture_env.sh "$OUT/env_before.json" "$BUILD_DIR" >/dev/null
./scripts/bench_env.sh throttle > "$OUT/throttle_before.txt"
THR_CORE_BEFORE="$(grep -oP 'SUM  core=\K[0-9]+' "$OUT/throttle_before.txt")"
THR_PKG_BEFORE="$(grep -oP 'PKG  package=\K[0-9]+' "$OUT/throttle_before.txt")"

# ── warm-up ──────────────────────────────────────────────────────────────────
# Discarded. Brings caches, branch predictors and — more importantly — the
# thermal state into a steady condition before any sample is kept.
echo
echo "[*] warm-up (discarded)..."
taskset -c "$CPU_SOLO" "$BUILD_DIR/bench_order_book" \
    --benchmark_filter='BM_AddOrder_NoMatch' \
    --benchmark_min_time=5s >/dev/null 2>&1 || true

# ── collect ──────────────────────────────────────────────────────────────────

run_bench() {  # $1=binary  $2=cpus  $3=extra args...
    local bin="$1" cpus="$2"; shift 2
    echo "[*] $bin  (taskset -c $cpus)"
    taskset -c "$cpus" "$BUILD_DIR/$bin" \
        --benchmark_repetitions="$REPETITIONS" \
        --benchmark_out_format=json \
        --benchmark_out="$OUT/${bin}.json" \
        "$@" 2>&1 | tee "$OUT/${bin}.log" | tail -3
}

echo
run_bench bench_order_book       "$CPU_SOLO"
echo
run_bench bench_spsc_ring_buffer "$CPU_PAIR"
echo
run_bench bench_matching_engine  "$CPU_PAIR"
echo
run_bench bench_baseline_mutex   "$CPU_PAIR"

# ── postflight ───────────────────────────────────────────────────────────────

echo
./scripts/bench_env.sh throttle > "$OUT/throttle_after.txt"
./scripts/capture_env.sh "$OUT/env_after.json" "$BUILD_DIR" >/dev/null
THR_CORE_AFTER="$(grep -oP 'SUM  core=\K[0-9]+' "$OUT/throttle_after.txt")"
THR_PKG_AFTER="$(grep -oP 'PKG  package=\K[0-9]+' "$OUT/throttle_after.txt")"
D_CORE=$((THR_CORE_AFTER - THR_CORE_BEFORE))
D_PKG=$((THR_PKG_AFTER - THR_PKG_BEFORE))

VERDICT="VALID"
REASONS=""
if [ "$PINNED" != "1" ]; then
    VERDICT="INVALID"; REASONS="${REASONS}- Machine was not in measurement mode (frequency unpinned, SMT active).\n"
fi
if [ "$D_CORE" -ne 0 ] || [ "$D_PKG" -ne 0 ]; then
    VERDICT="INVALID"; REASONS="${REASONS}- Thermal throttling occurred during the run (core +$D_CORE, package +$D_PKG).\n"
fi
[ -n "$GIT_DIRTY" ] && REASONS="${REASONS}- Working tree was dirty; the binaries may not match commit $GIT_SHA.\n"

{
echo "# Anchor measurement — $LABEL"
echo
echo "**Verdict: $VERDICT**"
echo
if [ -n "$REASONS" ]; then echo "Reasons:"; printf "%b" "$REASONS"; echo; fi
cat <<MD
## What this is

Benchmark output from the **unfixed** code, measured on this machine. It is a
control group, not a performance claim.

The environment that produced the figures in README section 4 (a Tencent Cloud
VM, 2 vCPU @ 2494 MHz) has been decommissioned. Those numbers cannot be
reproduced, so they cannot serve as a baseline for anything measured here.
Comparing a future fix directly against them would confound a code change with
a hardware change. This run exists so that later fixes can be compared against
the same code on the same machine, isolating the code as the only variable.

## What is wrong with these numbers

Every defect documented in \`docs/AUDIT.md\` is present:

- \`BM_MixedWorkload\` and \`BM_AddOrder_FullMatch\` stop doing meaningful work
  after their first pass, because order state is never reset between iterations.
- \`BM_CancelOrder\` and \`BM_MixedWorkload\` include \`~OrderBook()\` in the
  timed region.
- \`PauseTiming\`/\`ResumeTiming\` adds a pedestal of roughly 800 ns per
  iteration to \`BM_AddOrder_FullMatch\` and \`BM_AddOrder_SweepLevels\`.
- Pipeline end-to-end latency measures queueing delay, not service latency.
- \`items_per_second\` in the multithreaded JSON is normalised by CPU time and
  is meaningless; derive throughput from \`real_time\`.
- TSC values are reported with a hardcoded 2.494 GHz conversion from the
  decommissioned VM.

**Do not quote any figure in this directory as a performance result.**

## Provenance
MD
echo "- commit: \`${GIT_SHA}${GIT_DIRTY}\`"
echo "- date: $STAMP"
echo "- repetitions: $REPETITIONS"
echo "- single-thread affinity: \`taskset -c $CPU_SOLO\`"
echo "- two-thread affinity: \`taskset -c $CPU_PAIR\`"
echo "- throttle delta: core +$D_CORE, package +$D_PKG"
echo "- environment: see \`env_before.json\` / \`env_after.json\`"
} > "$OUT/RESULT.md"

echo "=============================================================="
echo "  verdict       : $VERDICT"
echo "  throttle delta: core +$D_CORE  package +$D_PKG"
echo "  output        : $OUT"
echo "=============================================================="
[ "$DRY_RUN" = "1" ] && { echo "[i] dry run — output discarded at $OUT"; }
exit 0
