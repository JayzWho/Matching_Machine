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

# Commands a user may run AS ROOT WITHOUT A PASSWORD, one per line, extracted
# from a `sudo -l` listing on stdin.
#
# The listing is parsed on purpose. `sudo -n -l CMD` does NOT answer "can CMD
# run without a password": it succeeds for any command that some rule permits,
# password or not, as soon as listing itself needs no password — which is the
# case once a single NOPASSWD rule exists. Next to the usual `(ALL : ALL) ALL`
# entry it therefore reports every command as allowed. An earlier version of
# this check used that probe and could not tell the two apart.
#
# Listing format, one entry per line (long entries may be wrapped):
#     (runas-users[ : groups]) [TAG: ...] command[, [TAG: ...] command ...]
# A tag such as NOPASSWD: applies to the commands after it until PASSWD: resets
# it. Entries whose runas users include neither root nor ALL are ignored.
nopasswd_commands() {
    awk '
        function flush(    runas, rest, close_at, n, i, m, j, u, ok, nopass, item, tag) {
            if (cur == "") return
            sub(/^[ \t]+/, "", cur)
            close_at = index(cur, ")")
            if (substr(cur, 1, 1) != "(" || close_at == 0) { cur = ""; return }
            runas = substr(cur, 2, close_at - 2)
            rest  = substr(cur, close_at + 1)
            sub(/[ \t]*:.*$/, "", runas)                      # users only, drop groups
            ok = 0
            m = split(runas, users, /[ \t]*,[ \t]*/)
            for (j = 1; j <= m; j++) {
                u = users[j]; gsub(/^[ \t]+|[ \t]+$/, "", u)
                if (u == "root" || u == "ALL") ok = 1
            }
            if (ok) {
                nopass = 0
                n = split(rest, items, /,[ \t]+/)
                for (i = 1; i <= n; i++) {
                    item = items[i]; gsub(/^[ \t]+|[ \t]+$/, "", item)
                    while (match(item, /^[A-Z_]+:[ \t]*/)) {
                        tag = substr(item, 1, RLENGTH); sub(/:[ \t]*$/, "", tag)
                        if (tag == "NOPASSWD") nopass = 1
                        if (tag == "PASSWD")   nopass = 0
                        item = substr(item, RLENGTH + 1)
                    }
                    if (nopass && item != "") print item
                }
            }
            cur = ""
        }
        /^[ \t]+\(/ { flush(); cur = $0; next }               # a new entry
        /^[ \t]+/   { if (cur != "") cur = cur " " $0; next } # wrapped continuation
                    { flush() }                               # header or blank line
        END         { flush() }
    '
}

# How "$DEST <action>" may run without a password, given nopasswd_commands
# output: "exact" (the intended rule), "broad" (only through something wider —
# NOPASSWD: ALL, the bare command with any arguments, or a wildcard), or "none".
#   $1 = nopasswd_commands output, $2 = action
rule_status() {
    local granted="$1" want="$DEST $2" line
    if grep -qxF -- "$want" <<< "$granted"; then echo exact; return; fi
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        # $line is deliberately unquoted on the right: sudoers wildcards are
        # shell-style patterns, so this is a glob match. It is never executed.
        # shellcheck disable=SC2053
        if [ "$line" = ALL ] || [ "$line" = "$DEST" ] || [[ "$want" == $line ]]; then
            echo broad; return
        fi
    done <<< "$granted"
    echo none
}

# Password-free grants that are wider than the two intended commands.
#   $1 = nopasswd_commands output
broader_rules() {
    local line
    while IFS= read -r line; do
        case "$line" in
            "$DEST setup"|"$DEST restore") ;;
            ALL|"$DEST"|"$DEST "*) printf '%s\n' "$line" ;;
        esac
    done <<< "$1"
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
    # Which commands are password-free is read from the rule listing itself;
    # see nopasswd_commands for why `sudo -n -l CMD` cannot be used for this.
    local a listing="" granted="" listed=1 rule_missing=0 wide
    if [ "$(id -u)" -eq 0 ]; then
        listing="$(sudo -l -U "$user" 2>/dev/null)" || listed=0
    else
        # Fails when listing itself needs a password, i.e. (with sudo's default
        # listpw=any) when the user has no NOPASSWD rule at all.
        listing="$(sudo -n -l 2>/dev/null)" || listed=0
    fi
    [ "$listed" = 1 ] && granted="$(nopasswd_commands <<< "$listing")"
    for a in setup restore; do
        case "$(rule_status "$granted" "$a")" in
            exact) echo "sudo rule       : '$a' allowed without password" ;;
            broad) echo "sudo rule       : '$a' allowed without password, but only via a BROADER rule (below)" ;;
            *)     rule_missing=1; ok=0
                   if [ "$listed" = 1 ]; then echo "sudo rule       : '$a' requires a password (rule missing)"
                   else echo "sudo rule       : '$a' requires a password (no password-free rule can be listed)"; fi ;;
        esac
    done
    while IFS= read -r wide; do
        [ -n "$wide" ] || continue
        if [ "$wide" = ALL ]; then
            # The machine owner's policy, not this tool's rule: report, do not fail.
            echo "  [!] note: a 'NOPASSWD: ALL' rule makes EVERY command password-free for $user"
        else
            echo "  [!] BROADER THAN INTENDED: '$wide' is password-free; only 'setup' and 'restore' should be"
            ok=0
        fi
    done <<< "$(broader_rules "$granted")"
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
    if [ "$rule_missing" = 1 ] && [ "$(id -u)" -ne 0 ]; then
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
        # Output is captured in memory: a root process must not write to a
        # predictable path in a world-writable directory like /tmp.
        local out t
        for t in test_bench_env.sh test_install_bench_env.sh; do
            echo "[*] running scripts/$t as $user..."
            if ! out="$(runuser -u "$user" -- "$ROOT/scripts/$t" 2>&1)"; then
                printf '%s\n' "$out"
                die "scripts/$t failed; not installing"
            fi
            printf '%s\n' "$out" | tail -1
        done
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

main() {
    case "${1:-}" in
        "")          install_copy; echo; check || true ;;
        --pin)       pin_cores "${2:-}"; echo; check || true ;;
        --unpin)     unpin_cores ;;
        --check)     check ;;
        --uninstall) uninstall_copy ;;
        *)           awk 'NR > 2 && /^# =====/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 1 ;;
    esac
}

# Only dispatch when executed; when sourced (scripts/test_install_bench_env.sh)
# the file just defines its functions.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
