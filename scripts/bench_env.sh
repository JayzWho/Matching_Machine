#!/usr/bin/env bash
# =============================================================================
# bench_env.sh — put the machine into (and out of) a repeatable benchmark state
#
# Frequency scaling, turbo and SMT make benchmark results on this machine
# unrepeatable. This script applies a fixed, documented measurement state and
# restores the previous one afterwards, so day-to-day use is unaffected.
# Nothing is written to /etc; every change is reverted by 'restore'.
#
# Usage:
#   sudo ./scripts/bench_env.sh setup      enter measurement mode
#   sudo ./scripts/bench_env.sh restore    leave it, restoring the saved state
#        ./scripts/bench_env.sh status     show current state (no root needed)
#        ./scripts/bench_env.sh throttle   print throttle counters (no root)
#
# What setup changes, and why:
#
#   governor      -> performance   removes load-dependent frequency scaling.
#   no_turbo      -> 1             pins to base frequency. Turbo cannot be
#                                  sustained in this thermal envelope: RAPL
#                                  reports tau ~28 s, and the package throttle
#                                  counter on this machine is already in the
#                                  tens of thousands. Without this, benchmarks
#                                  early in a suite run at a different
#                                  frequency than those late in the same suite,
#                                  which is an order-dependent systematic bias
#                                  that repetition cannot remove.
#   min_perf_pct  -> 100           forbids downward P-state transitions.
#   SMT siblings  -> offline       removes L1D/L2 and execution-port contention
#                                  from OS noise landing on the sibling of a
#                                  measured core. Siblings are derived from
#                                  sysfs topology, not hardcoded.
#   perf_paranoid -> 1             allows hardware PMU sampling without root.
#
# See docs/AUDIT.md for why this matters to the measurements.
# =============================================================================

set -euo pipefail

STATE_FILE="/var/tmp/matching_machine_bench_env.state"
PSTATE_DIR="/sys/devices/system/cpu/intel_pstate"
CPU_DIR="/sys/devices/system/cpu"

# ── helpers ──────────────────────────────────────────────────────────────────

die() { echo "[!] $*" >&2; exit 1; }
need_root() { [ "$(id -u)" -eq 0 ] || die "this action needs root: sudo $0 $1"; }
rd()  { cat "$1" 2>/dev/null || echo "?"; }

online_cpus() {
    for d in "$CPU_DIR"/cpu[0-9]*; do
        local id="${d##*/cpu}"
        # cpu0 usually has no 'online' file and is always online
        if [ ! -e "$d/online" ] || [ "$(rd "$d/online")" = "1" ]; then
            echo "$id"
        fi
    done
}

# CPUs that are NOT the lowest-numbered thread of their physical core.
# Derived from topology so this works on any enumeration scheme.
sibling_cpus() {
    for d in "$CPU_DIR"/cpu[0-9]*; do
        local id="${d##*/cpu}"
        local list first
        list="$(rd "$d/topology/thread_siblings_list")"
        [ "$list" = "?" ] && continue
        first="${list%%,*}"
        first="${first%%-*}"
        [ "$id" != "$first" ] && echo "$id"
    done
}

# ── status ───────────────────────────────────────────────────────────────────

cmd_status() {
    echo "── CPU ──────────────────────────────────────────────────────────────"
    printf "  model            : %s\n" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    printf "  online CPUs      : %s\n" "$(online_cpus | tr '\n' ' ')"
    printf "  SMT siblings     : %s\n" "$(sibling_cpus | tr '\n' ' ' | sed 's/ $//;s/^$/(none online)/')"
    printf "  governor(cpu0)   : %s\n" "$(rd "$CPU_DIR/cpu0/cpufreq/scaling_governor")"
    printf "  scaling driver   : %s\n" "$(rd "$CPU_DIR/cpu0/cpufreq/scaling_driver")"
    if [ -d "$PSTATE_DIR" ]; then
        printf "  no_turbo         : %s\n" "$(rd "$PSTATE_DIR/no_turbo")"
        printf "  min/max perf pct : %s / %s\n" "$(rd "$PSTATE_DIR/min_perf_pct")" "$(rd "$PSTATE_DIR/max_perf_pct")"
    fi
    printf "  perf_paranoid    : %s\n" "$(rd /proc/sys/kernel/perf_event_paranoid)"
    echo "  current MHz      :"
    for c in $(online_cpus); do
        printf "      cpu%-2s %s MHz\n" "$c" "$(( $(rd "$CPU_DIR/cpu$c/cpufreq/scaling_cur_freq") / 1000 ))"
    done
    echo "── measurement mode ─────────────────────────────────────────────────"
    if [ -f "$STATE_FILE" ]; then
        echo "  ACTIVE  (saved state: $STATE_FILE)"
        echo "  run 'sudo $0 restore' when finished"
    else
        echo "  inactive"
    fi
}

cmd_throttle() {
    # Printed before and after a measurement. A non-zero delta invalidates the run.
    #
    # Counters are de-duplicated before aggregation: core_throttle_count is per
    # physical core and is reported by BOTH SMT siblings, while
    # package_throttle_count is package-wide and reported identically by every
    # cpu. Summing either one would multiply a single event by its reporters.
    local total_core=0 pkg="" t cc pc list first
    for c in $(online_cpus); do
        t="$CPU_DIR/cpu$c/thermal_throttle"
        [ -d "$t" ] || continue
        cc="$(rd "$t/core_throttle_count")"; pc="$(rd "$t/package_throttle_count")"
        list="$(rd "$CPU_DIR/cpu$c/topology/thread_siblings_list")"
        first="${list%%,*}"; first="${first%%-*}"
        if [ "$c" = "$first" ]; then
            printf "  cpu%-2s core=%-10s (physical core)\n" "$c" "$cc"
            [ "$cc" != "?" ] && total_core=$((total_core + cc))
        else
            printf "  cpu%-2s core=%-10s (SMT sibling, same core)\n" "$c" "$cc"
        fi
        [ -z "$pkg" ] && [ "$pc" != "?" ] && pkg="$pc"
    done
    printf "  SUM  core=%s (per-core, de-duplicated)\n" "$total_core"
    printf "  PKG  package=%s (package-wide, single counter)\n" "${pkg:-0}"
}

# ── setup ────────────────────────────────────────────────────────────────────

cmd_setup() {
    need_root setup
    [ -f "$STATE_FILE" ] && die "measurement mode already active ($STATE_FILE). Run 'restore' first."
    [ -d "$PSTATE_DIR" ] || die "intel_pstate not present; this script targets intel_pstate systems"

    echo "[*] saving current state -> $STATE_FILE"
    {
        echo "# saved by bench_env.sh at $(date -Is)"
        echo "SAVED_PARANOID=$(rd /proc/sys/kernel/perf_event_paranoid)"
        echo "SAVED_NO_TURBO=$(rd "$PSTATE_DIR/no_turbo")"
        echo "SAVED_MIN_PCT=$(rd "$PSTATE_DIR/min_perf_pct")"
        echo "SAVED_MAX_PCT=$(rd "$PSTATE_DIR/max_perf_pct")"
        echo "SAVED_GOVERNORS=\"$(for c in $(online_cpus); do printf '%s:%s ' "$c" "$(rd "$CPU_DIR/cpu$c/cpufreq/scaling_governor")"; done)\""
        echo "SAVED_OFFLINED=\"$(sibling_cpus | tr '\n' ' ')\""
    } > "$STATE_FILE"

    echo "[*] governor -> performance"
    for c in $(online_cpus); do
        echo performance > "$CPU_DIR/cpu$c/cpufreq/scaling_governor" 2>/dev/null || true
    done

    echo "[*] no_turbo -> 1 (pin to base frequency)"
    echo 1 > "$PSTATE_DIR/no_turbo"

    echo "[*] min_perf_pct -> 100 (forbid downward P-states)"
    echo 100 > "$PSTATE_DIR/min_perf_pct"

    local sibs
    sibs="$(sibling_cpus | tr '\n' ' ')"
    if [ -n "${sibs// /}" ]; then
        echo "[*] offlining SMT siblings:$(printf ' cpu%s' $sibs)"
        for c in $sibs; do echo 0 > "$CPU_DIR/cpu$c/online"; done
    else
        echo "[*] no SMT siblings online, nothing to offline"
    fi

    echo "[*] perf_event_paranoid -> 1"
    echo 1 > /proc/sys/kernel/perf_event_paranoid

    echo
    echo "[*] verifying..."
    local fail=0
    [ "$(rd "$PSTATE_DIR/no_turbo")" = "1" ]     || { echo "  [!] no_turbo not applied"; fail=1; }
    [ "$(rd "$PSTATE_DIR/min_perf_pct")" = "100" ] || { echo "  [!] min_perf_pct not applied"; fail=1; }
    [ "$(rd "$CPU_DIR/cpu0/cpufreq/scaling_governor")" = "performance" ] || { echo "  [!] governor not applied"; fail=1; }
    [ "$(rd /proc/sys/kernel/perf_event_paranoid)" = "1" ] || { echo "  [!] paranoid not applied"; fail=1; }
    [ -z "$(sibling_cpus | tr -d '\n ')" ]        || { echo "  [!] SMT siblings still online"; fail=1; }
    [ "$fail" -eq 0 ] && echo "  all settings applied" || die "some settings failed; run 'restore'"

    echo
    cmd_status
    echo
    echo "[i] Measurement mode is ACTIVE. Remember: sudo $0 restore"
}

# ── restore ──────────────────────────────────────────────────────────────────

cmd_restore() {
    need_root restore
    [ -f "$STATE_FILE" ] || die "no saved state at $STATE_FILE — nothing to restore"
    # shellcheck disable=SC1090
    source "$STATE_FILE"

    echo "[*] bringing offlined CPUs back online:${SAVED_OFFLINED:- (none)}"
    for c in ${SAVED_OFFLINED:-}; do
        [ -e "$CPU_DIR/cpu$c/online" ] && echo 1 > "$CPU_DIR/cpu$c/online"
    done

    echo "[*] restoring intel_pstate limits"
    # max before min, so an invalid intermediate range is never requested
    [ "${SAVED_MAX_PCT:-?}" != "?" ] && echo "$SAVED_MAX_PCT" > "$PSTATE_DIR/max_perf_pct"
    [ "${SAVED_MIN_PCT:-?}" != "?" ] && echo "$SAVED_MIN_PCT" > "$PSTATE_DIR/min_perf_pct"
    [ "${SAVED_NO_TURBO:-?}" != "?" ] && echo "$SAVED_NO_TURBO" > "$PSTATE_DIR/no_turbo"

    echo "[*] restoring governors"
    for pair in ${SAVED_GOVERNORS:-}; do
        local c="${pair%%:*}" g="${pair##*:}"
        [ -e "$CPU_DIR/cpu$c/cpufreq/scaling_governor" ] && echo "$g" > "$CPU_DIR/cpu$c/cpufreq/scaling_governor" 2>/dev/null || true
    done

    echo "[*] restoring perf_event_paranoid -> ${SAVED_PARANOID:-4}"
    echo "${SAVED_PARANOID:-4}" > /proc/sys/kernel/perf_event_paranoid

    rm -f "$STATE_FILE"
    echo
    cmd_status
}

# ── dispatch ─────────────────────────────────────────────────────────────────

case "${1:-}" in
    setup)    cmd_setup    ;;
    restore)  cmd_restore  ;;
    status)   cmd_status   ;;
    throttle) cmd_throttle ;;
    *)
        sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
