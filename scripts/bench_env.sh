#!/usr/bin/env bash
# =============================================================================
# bench_env.sh — put the machine into (and out of) a repeatable benchmark state
#
# Frequency scaling, turbo, SMT and interrupt placement make benchmark results
# on this machine unrepeatable. This script applies a fixed, documented
# measurement state and restores the previous one afterwards, so day-to-day use
# is unaffected. Nothing is written to /etc; a reboot reverts everything.
#
# Usage:
#   sudo ./scripts/bench_env.sh setup              enter measurement mode
#        ./scripts/bench_env.sh setup --dry-run    show what setup would do
#   sudo ./scripts/bench_env.sh setup --cpus 1,3   override core selection
#   sudo ./scripts/bench_env.sh restore            restore the saved state
#        ./scripts/bench_env.sh status             current state
#        ./scripts/bench_env.sh throttle           thermal throttle counters
#        ./scripts/bench_env.sh cores              measurement-core ranking
#        ./scripts/bench_env.sh cores --list       selected cores, best first
#        ./scripts/bench_env.sh cores --rationale  ranking as one line
#
# What setup changes, and why:
#
#   governor      -> performance   removes load-dependent frequency scaling.
#   no_turbo      -> 1             pins to base frequency. Turbo cannot be
#                                  sustained in this thermal envelope (RAPL
#                                  tau ~28 s, package throttle count already in
#                                  the tens of thousands), so without this,
#                                  benchmarks early in a suite run at a
#                                  different frequency from those late in it:
#                                  an order-dependent bias repetition cannot fix.
#   min_perf_pct  -> 100           forbids downward P-state transitions.
#   SMT siblings  -> offline       removes L1D/L2 and execution-port contention
#                                  from OS noise on a measured core's sibling.
#                                  Derived from sysfs topology, never hardcoded.
#   perf_paranoid -> 1             allows hardware PMU sampling without root.
#   IRQ affinity  -> housekeeping  every movable device interrupt is steered
#                                  off the two measurement cores, and new IRQs
#                                  default away from them. Done AFTER siblings
#                                  go offline, because offlining migrates their
#                                  interrupts onto surviving cores — possibly
#                                  onto a measured one.
#
# How the measurement cores are chosen:
#
#   Physical cores are ranked by thermal throttle history (core_throttle_count
#   since boot), then by device-interrupt load, and the best two are used.
#   Never simply "the lowest two CPU numbers": on the machine this was written
#   for, cpu0 is the effective target of the ACPI interrupt (1.68M deliveries
#   against 0 on cpu1) and has throttled 13,915 times against cpu1's 17, and
#   with irqbalance not running none of that corrects itself.
#
#   This ranking is a heuristic. Throttle history resets on reboot. What
#   decides whether a measurement is valid is the throttle delta checked by
#   run_anchor.sh across the run itself, not this ranking.
#
# See docs/AUDIT.md for why this matters to the measurements.
# =============================================================================

set -euo pipefail

STATE_FILE="/var/tmp/matching_machine_bench_env.state"
IRQ_FILE="/var/tmp/matching_machine_bench_env.irq"
PSTATE_DIR="/sys/devices/system/cpu/intel_pstate"
CPU_DIR="/sys/devices/system/cpu"

# ── helpers ──────────────────────────────────────────────────────────────────

die()  { echo "[!] $*" >&2; exit 1; }
warn() { echo "  [!] $*" >&2; }
need_root() { [ "$(id -u)" -eq 0 ] || die "this action needs root: sudo $0 $1"; }
rd()   { cat "$1" 2>/dev/null || echo "?"; }

state_get() {  # read KEY from the saved state without sourcing it
    [ -f "$STATE_FILE" ] || return 0
    grep -m1 "^$1=" "$STATE_FILE" | cut -d= -f2- | sed 's/^"//; s/"$//'
}

online_cpus() {
    for d in "$CPU_DIR"/cpu[0-9]*; do
        local id="${d##*/cpu}"
        # cpu0 usually has no 'online' file and is always online
        if [ ! -e "$d/online" ] || [ "$(rd "$d/online")" = "1" ]; then
            echo "$id"
        fi
    done | sort -n
}

# "0,4" | "0-3" | "0-1,4-5"  ->  one id per line
expand_list() {
    local part a b i
    local IFS=','
    for part in $1; do
        if [[ "$part" == *-* ]]; then
            a="${part%-*}"; b="${part#*-}"
            for ((i = a; i <= b; i++)); do echo "$i"; done
        elif [ -n "$part" ]; then
            echo "$part"
        fi
    done
}

first_sibling() {  # lowest-numbered thread of cpu $1's physical core
    local l
    l="$(rd "$CPU_DIR/cpu$1/topology/thread_siblings_list")"
    [ "$l" = "?" ] && { echo "$1"; return; }
    l="${l%%,*}"; echo "${l%%-*}"
}

# CPUs that are NOT the lowest-numbered thread of their physical core.
sibling_cpus() {
    local c
    for c in $(online_cpus); do
        [ "$c" != "$(first_sibling "$c")" ] && echo "$c"
    done
    return 0
}

# Device (numbered) interrupt totals per online CPU: "<cpu> <count>".
# Columns are mapped from the header because offline CPUs have no column.
device_irqs_by_cpu() {
    awk 'NR == 1 { for (i = 1; i <= NF; i++) { c = $i; sub(/^CPU/, "", c); col[i] = c } n = NF; next }
         $1 ~ /^[0-9]+:$/ { for (i = 1; i <= n; i++) { v = $(i + 1); if (v ~ /^[0-9]+$/) s[col[i]] += v } }
         END { for (k in s) print k, s[k] }' /proc/interrupts | sort -n
}

# Active IRQs (those with a handler): "<irq> <name>"
active_irqs() {
    awk 'NR > 1 && $1 ~ /^[0-9]+:$/ { sub(":", "", $1); print $1, $NF }' /proc/interrupts
}

# Active IRQs whose EFFECTIVE delivery target is one of the given CPUs.
# The allowed mask (smp_affinity_list) is not enough: the kernel delivers to
# one CPU inside it, e.g. ACPI here is allowed 0-7 but delivered only to 0.
irqs_hitting() {
    local irq name eff c m
    while read -r irq name; do
        eff="$(cat "/proc/irq/$irq/effective_affinity_list" 2>/dev/null || true)"
        [ -z "$eff" ] && continue
        for c in $(expand_list "$eff"); do
            for m in "$@"; do
                if [ "$c" = "$m" ]; then echo "$irq:$name@cpu$c"; continue 3; fi
            done
        done
    done < <(active_irqs)
}

# One line per physical core, best measurement candidate first:
#   "<core_throttle_count> <device_irqs> <cpu> <siblings>"
rank_cores() {
    declare -A irq=()
    local c n s sibs thr total
    while read -r c n; do irq[$c]=$n; done < <(device_irqs_by_cpu)
    for c in $(online_cpus); do
        [ "$c" = "$(first_sibling "$c")" ] || continue
        sibs="$(rd "$CPU_DIR/cpu$c/topology/thread_siblings_list")"
        thr="$(rd "$CPU_DIR/cpu$c/thermal_throttle/core_throttle_count")"
        [ "$thr" = "?" ] && thr=0
        total=0
        for s in $(expand_list "$sibs"); do total=$(( total + ${irq[$s]:-0} )); done
        echo "$thr $total $c $sibs"
    done | sort -n -k1,1 -k2,2
}

rank_rationale() {
    rank_cores | awk '{ printf "%scpu%s[%s] throttle=%s irqs=%s", (NR > 1 ? "; " : ""), $3, $4, $1, $2 }
                      END { print "" }'
}

# Two measurement cores, best first, one per line. $1 = optional override list.
select_cores() {
    local override="${1:-}" c picked=()
    if [ -n "$override" ]; then
        for c in $(expand_list "$override"); do
            [ -d "$CPU_DIR/cpu$c" ] || die "--cpus: cpu$c does not exist"
            [ "$c" = "$(first_sibling "$c")" ] \
                || die "--cpus: cpu$c is an SMT sibling and would be offlined; use cpu$(first_sibling "$c")"
            picked+=("$c")
        done
        [ "${#picked[@]}" -eq 2 ] || die "--cpus needs exactly two cores, got '${override}'"
        [ "${picked[0]}" != "${picked[1]}" ] || die "--cpus: the two cores must differ"
        printf '%s\n' "${picked[@]}"
        return
    fi
    rank_cores | head -2 | awk '{ print $3 }'
}

cpus_to_mask() {
    local m=0 c
    for c in "$@"; do m=$(( m | (1 << c) )); done
    printf '%x\n' "$m"
}

# ── read-only commands ───────────────────────────────────────────────────────

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
        local m
        m="$(state_get MEASURE_CPUS)"
        echo "  ACTIVE  (saved state: $STATE_FILE)"
        echo "  measurement cores : $m  ($(state_get MEASURE_SOURCE))"
        echo "  housekeeping cores: $(state_get HOUSEKEEPING)"
        echo "  IRQs steered      : moved=$(state_get IRQ_MOVED) refused=$(state_get IRQ_REFUSED)"
        # shellcheck disable=SC2086
        echo "  IRQs still on measurement cores now: $(irqs_hitting $m | tr '\n' ' ' | sed 's/^$/(none)/')"
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
    local total_core=0 pkg="" t cc pc
    for c in $(online_cpus); do
        t="$CPU_DIR/cpu$c/thermal_throttle"
        [ -d "$t" ] || continue
        cc="$(rd "$t/core_throttle_count")"; pc="$(rd "$t/package_throttle_count")"
        if [ "$c" = "$(first_sibling "$c")" ]; then
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

cmd_cores() {
    local mode="${1:-}"
    if [ -f "$STATE_FILE" ]; then
        # In measurement mode the live ranking is distorted (siblings offline,
        # interrupts steered), so report what setup selected, not a recomputation.
        case "$mode" in
            --list)      state_get MEASURE_CPUS | tr ' ' ','; return ;;
            --rationale) state_get MEASURE_RATIONALE; return ;;
        esac
        echo "measurement mode ACTIVE — cores selected at setup:"
        echo "  cores   : $(state_get MEASURE_CPUS)  ($(state_get MEASURE_SOURCE))"
        echo "  ranking : $(state_get MEASURE_RATIONALE)"
        return
    fi
    case "$mode" in
        --list)      select_cores | paste -sd, -; return ;;
        --rationale) rank_rationale; return ;;
        "") ;;
        *) die "unknown option for cores: $mode" ;;
    esac
    echo "physical cores, best measurement candidate first:"
    printf "  %-6s %-10s %-10s %s\n" CPU SIBLINGS THROTTLE DEVICE_IRQS
    rank_cores | awk '{ printf "  cpu%-3s %-10s %-10s %s\n", $3, $4, $1, $2 }'
    echo "selected: $(select_cores | paste -sd, -)"
    echo "(throttle = core_throttle_count since boot; device IRQs summed over both SMT threads)"
}

# ── setup ────────────────────────────────────────────────────────────────────

cmd_setup() {
    local dry=0 override=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --dry-run) dry=1 ;;
            --cpus)    override="${2:-}"; [ -n "$override" ] || die "--cpus needs a value"; shift ;;
            *)         die "unknown setup option: $1" ;;
        esac
        shift
    done
    [ "$dry" = 1 ] || need_root setup
    [ -f "$STATE_FILE" ] && die "measurement mode already active ($STATE_FILE). Run 'restore' first."
    [ -d "$PSTATE_DIR" ] || die "intel_pstate not present; this script targets intel_pstate systems"

    # Choose measurement cores BEFORE changing anything: the ranking reads
    # interrupt columns that disappear from /proc/interrupts once siblings go
    # offline, and interrupt counts that steering is about to change.
    local measure source rationale sibs housekeeping="" c
    measure="$(select_cores "$override" | tr '\n' ' ' | sed 's/ $//')"
    [ "$(wc -w <<< "$measure")" -eq 2 ] || die "could not select two measurement cores"
    if [ -n "$override" ]; then source="override (--cpus $override)"; else source="ranked"; fi
    rationale="$(rank_rationale)"
    sibs="$(sibling_cpus | tr '\n' ' ' | sed 's/ $//')"
    for c in $(online_cpus); do
        case " $sibs "    in *" $c "*) continue ;; esac
        case " $measure " in *" $c "*) continue ;; esac
        housekeeping="$housekeeping $c"
    done
    housekeeping="${housekeeping# }"

    echo "[*] measurement cores : $measure  ($source)"
    echo "    ranking           : $rationale"
    echo "[*] housekeeping cores: ${housekeeping:-(none)}"
    echo "[*] SMT siblings      : ${sibs:-(none)}"

    if [ "$dry" = 1 ]; then
        echo "[*] active IRQs currently delivered to the measurement cores:"
        # shellcheck disable=SC2086
        irqs_hitting $measure | sed 's/^/      /'
        echo "[i] dry run: nothing changed"
        return
    fi

    echo "[*] saving current state -> $STATE_FILE, $IRQ_FILE"
    {
        echo "# saved by bench_env.sh at $(date -Is)"
        echo "SAVED_PARANOID=$(rd /proc/sys/kernel/perf_event_paranoid)"
        echo "SAVED_NO_TURBO=$(rd "$PSTATE_DIR/no_turbo")"
        echo "SAVED_MIN_PCT=$(rd "$PSTATE_DIR/min_perf_pct")"
        echo "SAVED_MAX_PCT=$(rd "$PSTATE_DIR/max_perf_pct")"
        echo "SAVED_GOVERNORS=\"$(for c in $(online_cpus); do printf '%s:%s ' "$c" "$(rd "$CPU_DIR/cpu$c/cpufreq/scaling_governor")"; done)\""
        echo "SAVED_OFFLINED=\"$sibs\""
        echo "SAVED_DEFAULT_IRQ_MASK=$(rd /proc/irq/default_smp_affinity)"
        echo "MEASURE_CPUS=\"$measure\""
        echo "MEASURE_SOURCE=\"$source\""
        echo "MEASURE_RATIONALE=\"$rationale\""
        echo "HOUSEKEEPING=\"$housekeeping\""
    } > "$STATE_FILE"
    # IRQ affinities are saved BEFORE siblings go offline: offlining can rewrite
    # the mask of an interrupt whose only allowed CPUs disappear.
    for d in /proc/irq/[0-9]*; do
        [ -e "$d/smp_affinity_list" ] && echo "${d##*/} $(cat "$d/smp_affinity_list")"
    done > "$IRQ_FILE"
    chmod 0644 "$STATE_FILE" "$IRQ_FILE"

    echo "[*] governor -> performance"
    for c in $(online_cpus); do
        echo performance > "$CPU_DIR/cpu$c/cpufreq/scaling_governor" 2>/dev/null || true
    done

    echo "[*] no_turbo -> 1 (pin to base frequency)"
    echo 1 > "$PSTATE_DIR/no_turbo"

    echo "[*] min_perf_pct -> 100 (forbid downward P-states)"
    echo 100 > "$PSTATE_DIR/min_perf_pct"

    if [ -n "$sibs" ]; then
        echo "[*] offlining SMT siblings:$(printf ' cpu%s' $sibs)"
        for c in $sibs; do echo 0 > "$CPU_DIR/cpu$c/online"; done
    fi

    echo "[*] perf_event_paranoid -> 1"
    echo 1 > /proc/sys/kernel/perf_event_paranoid

    local moved=0 refused=0
    if [ -n "$housekeeping" ]; then
        local hk_list hk_mask
        hk_list="$(tr ' ' ',' <<< "$housekeeping")"
        # shellcheck disable=SC2086
        hk_mask="$(cpus_to_mask $housekeeping)"
        echo "[*] steering IRQs off measurement cores -> {$hk_list}"
        echo "$hk_mask" > /proc/irq/default_smp_affinity 2>/dev/null || warn "could not set default_smp_affinity"
        for d in /proc/irq/[0-9]*; do
            [ -e "$d/smp_affinity_list" ] || continue
            if echo "$hk_list" > "$d/smp_affinity_list" 2>/dev/null; then
                moved=$((moved + 1))
            else
                refused=$((refused + 1))
            fi
        done
        echo "    moved=$moved refused=$refused (per-cpu and kernel-managed IRQs cannot move)"
    else
        warn "no housekeeping core left; interrupts NOT steered off the measurement cores"
    fi

    local residual
    # shellcheck disable=SC2086
    residual="$(irqs_hitting $measure | tr '\n' ' ' | sed 's/ $//')"
    {
        echo "IRQ_MOVED=$moved"
        echo "IRQ_REFUSED=$refused"
        echo "IRQ_RESIDUAL=\"$residual\""
    } >> "$STATE_FILE"

    echo
    echo "[*] verifying..."
    local fail=0
    [ "$(rd "$PSTATE_DIR/no_turbo")" = "1" ]       || { warn "no_turbo not applied"; fail=1; }
    [ "$(rd "$PSTATE_DIR/min_perf_pct")" = "100" ] || { warn "min_perf_pct not applied"; fail=1; }
    [ "$(rd "$CPU_DIR/cpu0/cpufreq/scaling_governor")" = "performance" ] || { warn "governor not applied"; fail=1; }
    [ "$(rd /proc/sys/kernel/perf_event_paranoid)" = "1" ] || { warn "paranoid not applied"; fail=1; }
    [ -z "$(sibling_cpus)" ] || { warn "SMT siblings still online"; fail=1; }
    [ "$fail" -eq 0 ] || die "some settings failed; run 'sudo $0 restore'"
    echo "  all settings applied"
    if [ -n "$residual" ]; then
        warn "IRQs still delivered to measurement cores (could not be moved): $residual"
        warn "run_anchor.sh records how often they fire during the run"
    else
        echo "  no active IRQ is delivered to the measurement cores"
    fi

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

    # After CPUs are back, so masks naming them are accepted again.
    if [ -f "$IRQ_FILE" ]; then
        local ok=0 bad=0 irq aff
        echo "[*] restoring IRQ affinities"
        while read -r irq aff; do
            [ -e "/proc/irq/$irq/smp_affinity_list" ] || continue
            if echo "$aff" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null; then
                ok=$((ok + 1))
            else
                bad=$((bad + 1))
            fi
        done < "$IRQ_FILE"
        echo "    restored=$ok refused=$bad"
    fi
    if [ -n "${SAVED_DEFAULT_IRQ_MASK:-}" ] && [ "$SAVED_DEFAULT_IRQ_MASK" != "?" ]; then
        echo "$SAVED_DEFAULT_IRQ_MASK" > /proc/irq/default_smp_affinity 2>/dev/null \
            || warn "could not restore default_smp_affinity"
    fi

    echo "[*] restoring intel_pstate limits"
    # max before min, so an invalid intermediate range is never requested
    [ "${SAVED_MAX_PCT:-?}" != "?" ]  && echo "$SAVED_MAX_PCT"  > "$PSTATE_DIR/max_perf_pct"
    [ "${SAVED_MIN_PCT:-?}" != "?" ]  && echo "$SAVED_MIN_PCT"  > "$PSTATE_DIR/min_perf_pct"
    [ "${SAVED_NO_TURBO:-?}" != "?" ] && echo "$SAVED_NO_TURBO" > "$PSTATE_DIR/no_turbo"

    echo "[*] restoring governors"
    for pair in ${SAVED_GOVERNORS:-}; do
        local c="${pair%%:*}" g="${pair##*:}"
        [ -e "$CPU_DIR/cpu$c/cpufreq/scaling_governor" ] && echo "$g" > "$CPU_DIR/cpu$c/cpufreq/scaling_governor" 2>/dev/null || true
    done

    echo "[*] restoring perf_event_paranoid -> ${SAVED_PARANOID:-4}"
    echo "${SAVED_PARANOID:-4}" > /proc/sys/kernel/perf_event_paranoid

    rm -f "$STATE_FILE" "$IRQ_FILE"
    echo
    cmd_status
}

# ── dispatch ─────────────────────────────────────────────────────────────────

usage() { awk 'NR > 2 && /^# =====/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; }

case "${1:-}" in
    setup)    shift; cmd_setup "$@" ;;
    restore)  cmd_restore ;;
    status)   cmd_status ;;
    throttle) cmd_throttle ;;
    cores)    shift; cmd_cores "${1:-}" ;;
    *)        usage; exit 1 ;;
esac
