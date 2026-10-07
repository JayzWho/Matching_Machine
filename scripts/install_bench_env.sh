#!/usr/bin/env bash
# =============================================================================
# install_bench_env.sh — install bench_env.sh as a root-owned command
#
# Lets bench_env.sh's privileged actions run without a password prompt,
# without granting general sudo. The NOPASSWD rule must point at a root-owned
# copy: the copy in this repository is user-writable, and anyone able to edit a
# file named in a NOPASSWD rule has root.
#
# Usage:
#   sudo ./scripts/install_bench_env.sh              install or update the copy
#   sudo ./scripts/install_bench_env.sh --pin A,B    install/update, then pin the
#                                                    measurement cores (A runs the
#                                                    single-threaded benchmarks)
#   sudo ./scripts/install_bench_env.sh --unpin      remove the pin
#        ./scripts/install_bench_env.sh --check      report status (no root)
#   sudo ./scripts/install_bench_env.sh --uninstall  remove the copy
#
# Why pin: measurements are only comparable when they run on the same cores.
# Left unpinned, bench_env.sh ranks cores by counters that reset on reboot, so
# the pair can change from one boot to the next. The pin is a root-owned file,
# /etc/matching-machine-bench.conf, because the root-run command reads it.
#
# After the first install, add the sudoers rule yourself (this script never
# edits sudoers — that change should be made deliberately, by the admin):
#
#   sudo visudo -f /etc/sudoers.d/matching-machine-bench
#
# with exactly this line (arguments are matched exactly, so nothing else —
# not even `setup --cpus ...` — becomes password-free):
#
#   <user> ALL=(root) NOPASSWD: /usr/local/sbin/mm-bench-env setup, /usr/local/sbin/mm-bench-env restore
#
# Re-run the install after bench_env.sh changes. Until then the installed copy
# is stale; run_anchor.sh records whether the copy that performed setup matched
# the committed script.
# =============================================================================

set -euo pipefail
export LC_ALL=C

DEST="/usr/local/sbin/mm-bench-env"
SUDOERS_FILE="/etc/sudoers.d/matching-machine-bench"
PIN_FILE="/etc/matching-machine-bench.conf"
STATE_FILE="/run/matching-machine-bench/state"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/scripts/bench_env.sh"

die() { echo "[!] $*" >&2; exit 1; }
sha() { sha256sum -- "$1" 2>/dev/null | cut -d' ' -f1; }

rule_line() {
    printf '%s ALL=(root) NOPASSWD: %s setup, %s restore\n' "$1" "$DEST" "$DEST"
}

check() {
    local user="${SUDO_USER:-$(id -un)}" s_repo s_inst ok=1
    s_repo="$(sha "$SRC")"
    echo "repository copy : $SRC"
    echo "                  sha256 $s_repo"
    if [ -e "$DEST" ]; then
        s_inst="$(sha "$DEST")"
        echo "installed copy  : $DEST ($(stat -c '%U:%G %a' -- "$DEST"))"
        echo "                  sha256 $s_inst"
        if [ "$(stat -c %u -- "$DEST")" != 0 ] || (( (8#$(stat -c %a -- "$DEST") & 8#022) != 0 )); then
            echo "  [!] NOT SAFE: must be root-owned and not group/world-writable"; ok=0
        fi
        if [ "$s_inst" = "$s_repo" ]; then echo "  up to date with the repository"
        else echo "  [!] STALE: differs from the repository — re-run: sudo $0"; ok=0; fi
    else
        echo "installed copy  : (not installed) — run: sudo $0"; ok=0
    fi
    # `sudo -n -l CMD` succeeds only if CMD may run without a password prompt.
    local a
    for a in setup restore; do
        if [ "$(id -u)" -eq 0 ]; then
            if sudo -l -U "$user" -n "$DEST" "$a" >/dev/null 2>&1; then echo "sudo rule       : '$a' allowed for $user"
            else echo "sudo rule       : '$a' NOT allowed for $user"; ok=0; fi
        elif sudo -n -l "$DEST" "$a" >/dev/null 2>&1; then
            echo "sudo rule       : '$a' allowed without password"
        else
            echo "sudo rule       : '$a' requires a password (rule missing)"; ok=0
        fi
    done
    # Pinned cores, judged by the repository script: the same validation setup
    # applies (root-owned, not writable by others, a well-formed pair).
    local cores
    if [ -e "$STATE_FILE" ]; then
        echo "pinned cores    : (measurement mode active; using $("$SRC" cores --list 2>/dev/null))"
    elif [ ! -e "$PIN_FILE" ] && [ ! -L "$PIN_FILE" ]; then
        echo "pinned cores    : [!] NOT pinned — the pair can change after a reboot"
        echo "                  pin with: sudo $0 --pin A,B"
        ok=0
    elif cores="$("$SRC" cores --list 2>&1)"; then
        echo "pinned cores    : $cores  ($PIN_FILE, $(stat -c '%U:%G %a' -- "$PIN_FILE"))"
    else
        echo "pinned cores    : [!] $PIN_FILE is REFUSED: ${cores##*\[!\] }"
        ok=0
    fi
    if [ "$ok" = 0 ] && ! sudo -n -l "$DEST" setup >/dev/null 2>&1 && [ "$(id -u)" -ne 0 ]; then
        echo
        echo "to add the rule:  sudo visudo -f $SUDOERS_FILE"
        echo "with the line:    $(rule_line "$user")"
    fi
    [ "$ok" = 1 ]
}

install_copy() {
    [ "$(id -u)" -eq 0 ] || die "install needs root: sudo $0"
    [ -f "$SRC" ] || die "missing $SRC"
    bash -n "$SRC" || die "$SRC has a syntax error; not installing"

    # Refuse to install a copy whose privilege-hardening tests fail. They are
    # run unprivileged, as the user who invoked sudo.
    local user="${SUDO_USER:-}"
    if [ -n "$user" ] && [ "$user" != root ]; then
        echo "[*] running scripts/test_bench_env.sh as $user..."
        # Output is captured in memory: a root process must not write to a
        # predictable path in a world-writable directory like /tmp.
        local out
        if ! out="$(runuser -u "$user" -- "$ROOT/scripts/test_bench_env.sh" 2>&1)"; then
            printf '%s\n' "$out"
            die "hardening tests failed; not installing"
        fi
        printf '%s\n' "$out" | tail -1
    else
        echo "[!] could not determine the invoking user; hardening tests skipped"
    fi

    install -o root -g root -m 0755 -- "$SRC" "$DEST"
    [ "$(sha "$DEST")" = "$(sha "$SRC")" ] || die "installed copy does not match $SRC"
    echo "[+] installed $DEST"
}

# Pin the measurement cores. $1 = "A,B", A being the solo core.
pin_cores() {
    local pair="${1:-}" a b old="" had_old=0 tmp got
    # Format first, so a typo is reported before anyone is asked for a password.
    [[ "$pair" =~ ^[0-9]{1,4},[0-9]{1,4}$ ]] \
        || die "--pin expects two CPU ids such as 3,1 (solo core first), got '$pair'"
    [ "$(id -u)" -eq 0 ] || die "pinning needs root: sudo $0 --pin $pair"
    # With siblings offline a pair cannot be validated against the real topology.
    [ -e "$STATE_FILE" ] && die "measurement mode is active; run 'sudo $DEST restore' first"
    a="$((10#${pair%,*}))"; b="$((10#${pair#*,}))"
    pair="$a,$b"

    # The installed command must understand the config before it is written.
    install_copy

    if [ -f "$PIN_FILE" ] && [ ! -L "$PIN_FILE" ]; then old="$(cat -- "$PIN_FILE")"; had_old=1; fi
    # /etc is writable by root only, so this temporary file cannot be raced.
    tmp="$(mktemp /etc/.matching-machine-bench.XXXXXX)"
    {
        echo "# Measurement cores for the Matching_Machine benchmarks, solo core first."
        echo "# Written by scripts/install_bench_env.sh --pin; read by mm-bench-env setup."
        echo "# Must stay root-owned and not group/world-writable, or it is refused."
        echo "MEASURE_CPUS=$pair"
    } > "$tmp"
    chown root:root "$tmp"
    chmod 0644 "$tmp"
    mv -f -- "$tmp" "$PIN_FILE"

    # One validator: let the installed command judge the file exactly as setup
    # will. If it refuses the pair, put back whatever was there before.
    if got="$("$DEST" cores --list 2>&1)" && [ "$got" = "$pair" ]; then
        echo "[+] pinned measurement cores: $pair  ($PIN_FILE)"
    else
        if [ "$had_old" = 1 ]; then printf '%s\n' "$old" > "$PIN_FILE"; else rm -f -- "$PIN_FILE"; fi
        die "the pair '$pair' was refused, pin left unchanged: ${got##*\[!\] }"
    fi
}

unpin_cores() {
    [ "$(id -u)" -eq 0 ] || die "unpinning needs root: sudo $0 --unpin"
    rm -f -- "$PIN_FILE"
    echo "[+] removed $PIN_FILE — cores are no longer pinned"
}

uninstall_copy() {
    [ "$(id -u)" -eq 0 ] || die "uninstall needs root: sudo $0 --uninstall"
    if [ -e /run/matching-machine-bench/state ]; then
        die "measurement mode is active; run 'sudo $DEST restore' first"
    fi
    rm -f -- "$DEST"
    echo "[+] removed $DEST"
    [ -e "$SUDOERS_FILE" ] && echo "[i] also remove the rule:  sudo rm $SUDOERS_FILE"
    [ -e "$PIN_FILE" ] && echo "[i] the core pin is kept:   $PIN_FILE  (remove with --unpin)"
    return 0
}

case "${1:-}" in
    "")          install_copy; echo; check || true ;;
    --pin)       pin_cores "${2:-}"; echo; check || true ;;
    --unpin)     unpin_cores ;;
    --check)     check ;;
    --uninstall) uninstall_copy ;;
    *)           awk 'NR > 2 && /^# =====/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 1 ;;
esac
