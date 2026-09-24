#!/usr/bin/env bash
# =============================================================================
# bench_env.sh — put the machine into (and out of) a repeatable benchmark state
#
# Frequency scaling, turbo, SMT and interrupt placement make benchmark results
# on this machine unrepeatable. This script applies a fixed, documented
# measurement state and restores the previous one afterwards, so day-to-day use
# is unaffected. Nothing is written to /etc; a reboot reverts everything.
#
# Usage (after the one-time install, the privileged commands can also be run
# as `sudo -n /usr/local/sbin/mm-bench-env setup|restore` — see below):
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
# Security model (this script is meant to be runnable via a NOPASSWD rule):
#
#   - The sudoers rule must name the ROOT-OWNED copy installed by
#     scripts/install_bench_env.sh (/usr/local/sbin/mm-bench-env), never this
#     file. This file lives in a user-writable repository; anyone who can edit
#     it would get root through such a rule.
#   - State lives in /run/matching-machine-bench/. /run is root-owned and 0755,
#     so only root can create entries there, and it is a tmpfs, so state does
#     not outlive a reboot — which reverts every setting anyway. The previous
#     location, /var/tmp, is world-writable: any local process could plant a
#     "state file" there for restore to consume.
#   - restore never sources or evaluates the state. It checks that the state
#     directory and files are root-owned, not symlinks and not group/world
#     writable, then parses each value and accepts it only if it matches a
#     strict pattern (numbers, CPU lists, hex masks, governor names). Values
#     are pattern-checked BEFORE any arithmetic, because bash arithmetic on an
#     untrusted string can execute commands (e.g. a[$(cmd)]).
#   - As root the script pins PATH and umask instead of inheriting them.
#
# See docs/AUDIT.md for why this matters to the measurements.
# =============================================================================

set -euo pipefail
export LC_ALL=C
if [ "$(id -u)" -eq 0 ]; then
    # Never inherit PATH or umask in a privileged run.
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    export PATH
    umask 022
fi

STATE_DIR="/run/matching-machine-bench"
STATE_FILE="$STATE_DIR/state"
IRQ_FILE="$STATE_DIR/irq"
# Written by versions before the privilege hardening. Never trusted.
LEGACY_STATE_FILE="/var/tmp/matching_machine_bench_env.state"
PSTATE_DIR="/sys/devices/system/cpu/intel_pstate"
CPU_DIR="/sys/devices/system/cpu"

# ── helpers ──────────────────────────────────────────────────────────────────

die()  { echo "[!] $*" >&2; exit 1; }
warn() { echo "  [!] $*" >&2; }
need_root() { [ "$(id -u)" -eq 0 ] || die "this action needs root: sudo $0 $1"; }
rd()   { cat "$1" 2>/dev/null || echo "?"; }

# Value of KEY in a KEY=value / KEY="value" file. Pure text extraction: the
# file is never sourced or evaluated. $1 = file, $2 = key (a literal from this
# script, never user input).
kv_get() {
    local line
    line="$(grep -m1 -E "^$2=" "$1" 2>/dev/null || true)"
    line="${line#*=}"
    line="${line#\"}"
    line="${line%\"}"
    printf '%s' "$line"
}

state_get() {  # for read-only, unprivileged display
    [ -f "$STATE_FILE" ] || return 0
    kv_get "$STATE_FILE" "$1"
}

# Refuse any path that a non-root user could have planted or modified.
# $1 = path, $2 = expected owner uid (0 in production; the tests pass their own)
require_trusted() {
    local p="$1" uid="$2" owner mode
    [ -L "$p" ] && die "$p is a symlink; refusing to trust it"
    [ -e "$p" ] || die "$p does not exist"
    owner="$(stat -c %u -- "$p")"
    mode="$(stat -c %a -- "$p")"
    [ "$owner" = "$uid" ] || die "$p is owned by uid $owner, expected uid $uid; refusing to trust it"
    (( (8#$mode & 8#022) == 0 )) || die "$p is group- or world-writable (mode $mode); refusing to trust it"
}

# Create the state directory, or verify an existing one.
ensure_state_dir() {
    if [ -e "$STATE_DIR" ] || [ -L "$STATE_DIR" ]; then
        require_trusted "$STATE_DIR" 0
        [ -d "$STATE_DIR" ] || die "$STATE_DIR exists but is not a directory"
    else
        install -d -o root -g root -m 0755 "$STATE_DIR"
    fi
}

# Parse and validate the saved state WITHOUT executing any of it. Populates the
# R_* globals; every value that fails its pattern is dropped and described in
# R_REJECTED. Patterns are checked before any arithmetic on a value.
#   $1 = expected owner uid of the state directory and files
load_saved_state() {
    local uid="$1" v key pair c g irq aff extra
    local -a items=()
    require_trusted "$STATE_DIR" "$uid"
    [ -d "$STATE_DIR" ] || die "$STATE_DIR is not a directory"
    require_trusted "$STATE_FILE" "$uid"
    [ -f "$STATE_FILE" ] || die "$STATE_FILE is not a regular file"

    R_REJECTED=()

    v="$(kv_get "$STATE_FILE" SAVED_PARANOID)"
    if [[ "$v" =~ ^-?[0-9]$ ]] && (( v >= -1 && v <= 4 )); then
        R_PARANOID="$v"
    else
        R_PARANOID=4
        R_REJECTED+=("SAVED_PARANOID='$v' (restoring the most restrictive value, 4)")
    fi

    v="$(kv_get "$STATE_FILE" SAVED_NO_TURBO)"
    if [[ "$v" =~ ^[01]$ ]]; then R_NO_TURBO="$v"; else R_NO_TURBO=""; R_REJECTED+=("SAVED_NO_TURBO='$v'"); fi

    for key in SAVED_MIN_PCT SAVED_MAX_PCT; do
        v="$(kv_get "$STATE_FILE" "$key")"
        if [[ "$v" =~ ^[0-9]{1,3}$ ]] && (( 10#$v <= 100 )); then
            v="$((10#$v))"
        else
            R_REJECTED+=("$key='$v'"); v=""
        fi
        if [ "$key" = SAVED_MIN_PCT ]; then R_MIN_PCT="$v"; else R_MAX_PCT="$v"; fi
    done

    R_GOVERNORS=()
    read -r -a items <<< "$(kv_get "$STATE_FILE" SAVED_GOVERNORS)"
    for pair in "${items[@]}"; do
        c="${pair%%:*}"; g="${pair#*:}"
        if [[ "$c" =~ ^[0-9]{1,4}$ && "$g" =~ ^[a-z_]{1,32}$ ]]; then
            R_GOVERNORS+=("$c:$g")
        else
            R_REJECTED+=("governor entry '$pair'")
        fi
    done

    R_OFFLINED=()
    read -r -a items <<< "$(kv_get "$STATE_FILE" SAVED_OFFLINED)"
    for c in "${items[@]}"; do
        if [[ "$c" =~ ^[0-9]{1,4}$ ]]; then R_OFFLINED+=("$c"); else R_REJECTED+=("offlined CPU '$c'"); fi
    done

    v="$(kv_get "$STATE_FILE" SAVED_DEFAULT_IRQ_MASK)"
    if [[ "$v" =~ ^[0-9a-fA-F]{1,16}(,[0-9a-fA-F]{1,16})*$ ]]; then
        R_DEFAULT_IRQ_MASK="$v"
    else
        R_DEFAULT_IRQ_MASK=""; R_REJECTED+=("SAVED_DEFAULT_IRQ_MASK='$v'")
    fi

    R_IRQS=()
    if [ -e "$IRQ_FILE" ] || [ -L "$IRQ_FILE" ]; then
        require_trusted "$IRQ_FILE" "$uid"
        [ -f "$IRQ_FILE" ] || die "$IRQ_FILE is not a regular file"
        while read -r irq aff extra; do
            if [[ "$irq" =~ ^[0-9]{1,5}$ && "$aff" =~ ^[0-9]{1,4}(-[0-9]{1,4})?(,[0-9]{1,4}(-[0-9]{1,4})?)*$ && -z "$extra" ]]; then
                R_IRQS+=("$irq $aff")
            else
                R_REJECTED+=("IRQ entry '$irq $aff${extra:+ $extra}'")
            fi
        done < "$IRQ_FILE"
    fi
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
    if [ -e "$LEGACY_STATE_FILE" ]; then
        echo "  note: $LEGACY_STATE_FILE was left by an older version and is ignored"
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
    [ -e "$LEGACY_STATE_FILE" ] && warn "ignoring $LEGACY_STATE_FILE (left by an older version; it is not trusted)"
    [ -d "$PSTATE_DIR" ] || die "intel_pstate not present; this script targets intel_pstate systems"

    # Choose measurement cores BEFORE changing anything: the ranking reads
    # interrupt columns that disappear from /proc/interrupts once siblings go
    # offline, and interrupt counts that steering is about to change.
    local measure selection rationale sibs housekeeping="" c
    measure="$(select_cores "$override" | tr '\n' ' ' | sed 's/ $//')"
    [ "$(wc -w <<< "$measure")" -eq 2 ] || die "could not select two measurement cores"
    if [ -n "$override" ]; then selection="override (--cpus $override)"; else selection="ranked"; fi
    rationale="$(rank_rationale)"
    sibs="$(sibling_cpus | tr '\n' ' ' | sed 's/ $//')"
    for c in $(online_cpus); do
        case " $sibs "    in *" $c "*) continue ;; esac
        case " $measure " in *" $c "*) continue ;; esac
        housekeeping="$housekeeping $c"
    done
    housekeeping="${housekeeping# }"

    echo "[*] measurement cores : $measure  ($selection)"
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

    ensure_state_dir
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
        echo "MEASURE_SOURCE=\"$selection\""
        echo "MEASURE_RATIONALE=\"$rationale\""
        echo "HOUSEKEEPING=\"$housekeeping\""
        # Which script performed setup, so an archive can tell whether the
        # installed copy matched the committed one.
        echo "SETUP_SCRIPT=\"$(readlink -f -- "$0")\""
        echo "SETUP_SCRIPT_SHA256=$(sha256sum -- "$0" | cut -d' ' -f1)"
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
    if [ ! -e "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ]; then
        if [ -e "$LEGACY_STATE_FILE" ]; then
            die "no state in $STATE_DIR, but $LEGACY_STATE_FILE exists. It was written by an
    older version into a world-writable directory and will not be trusted.
    Reboot to revert every setting, then delete that file."
        fi
        die "no saved state at $STATE_FILE — nothing to restore"
    fi

    load_saved_state 0
    local r
    for r in "${R_REJECTED[@]}"; do warn "saved state rejected, not restored: $r"; done

    echo "[*] bringing offlined CPUs back online:${R_OFFLINED[*]:+ ${R_OFFLINED[*]}}"
    local c
    for c in "${R_OFFLINED[@]}"; do
        if [ -e "$CPU_DIR/cpu$c/online" ]; then echo 1 > "$CPU_DIR/cpu$c/online"; fi
    done

    # After CPUs are back, so masks naming them are accepted again.
    local ok=0 bad=0 entry irq aff
    echo "[*] restoring IRQ affinities"
    for entry in "${R_IRQS[@]}"; do
        irq="${entry%% *}"; aff="${entry#* }"
        [ -e "/proc/irq/$irq/smp_affinity_list" ] || continue
        if echo "$aff" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null; then
            ok=$((ok + 1))
        else
            bad=$((bad + 1))
        fi
    done
    echo "    restored=$ok refused=$bad"
    if [ -n "$R_DEFAULT_IRQ_MASK" ]; then
        echo "$R_DEFAULT_IRQ_MASK" > /proc/irq/default_smp_affinity 2>/dev/null \
            || warn "could not restore default_smp_affinity"
    fi

    echo "[*] restoring intel_pstate limits"
    # max before min, so an invalid intermediate range is never requested
    if [ -n "$R_MAX_PCT" ];  then echo "$R_MAX_PCT"  > "$PSTATE_DIR/max_perf_pct"; fi
    if [ -n "$R_MIN_PCT" ];  then echo "$R_MIN_PCT"  > "$PSTATE_DIR/min_perf_pct"; fi
    if [ -n "$R_NO_TURBO" ]; then echo "$R_NO_TURBO" > "$PSTATE_DIR/no_turbo"; fi

    echo "[*] restoring governors"
    local pair g
    for pair in "${R_GOVERNORS[@]}"; do
        c="${pair%%:*}"; g="${pair#*:}"
        if [ -e "$CPU_DIR/cpu$c/cpufreq/scaling_governor" ]; then
            echo "$g" > "$CPU_DIR/cpu$c/cpufreq/scaling_governor" 2>/dev/null || warn "could not restore governor of cpu$c"
        fi
    done

    echo "[*] restoring perf_event_paranoid -> $R_PARANOID"
    echo "$R_PARANOID" > /proc/sys/kernel/perf_event_paranoid

    rm -f -- "$STATE_FILE" "$IRQ_FILE"
    echo
    cmd_status
}

# ── dispatch ─────────────────────────────────────────────────────────────────

usage() { awk 'NR > 2 && /^# =====/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; }

main() {
    case "${1:-}" in
        setup)    shift; cmd_setup "$@" ;;
        restore)  cmd_restore ;;
        status)   cmd_status ;;
        throttle) cmd_throttle ;;
        cores)    shift; cmd_cores "${1:-}" ;;
        *)        usage; exit 1 ;;
    esac
}

# Only dispatch when executed. When sourced (scripts/test_bench_env.sh), the
# file just defines its functions so they can be tested without root.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
