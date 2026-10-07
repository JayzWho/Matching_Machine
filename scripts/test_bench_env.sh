#!/usr/bin/env bash
# =============================================================================
# test_bench_env.sh — tests for the privilege hardening in bench_env.sh
#
# bench_env.sh is meant to be runnable as root through a NOPASSWD sudo rule, so
# its restore path must never execute anything taken from the saved state.
# These tests run unprivileged: they source bench_env.sh (which then only
# defines functions), point it at temporary files owned by the current user,
# and pass that uid as the "trusted owner". Every attack case uses a payload
# that would create a marker file if it were ever executed.
#
# Usage: ./scripts/test_bench_env.sh
# =============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=bench_env.sh
source "$ROOT/scripts/bench_env.sh"
# The tests inspect statuses themselves, and a broken implementation must show
# up as FAILs rather than kill the harness (unset results expand to empty).
set +eu

TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
ME="$(id -u)"
PASS=0
FAIL=0

ok()  { echo "  PASS  $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
expect() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }   # $2 is test code, not input

# Fresh state directory for one case; state file content from stdin.
new_case() {
    STATE_DIR="$TMP/$1"
    STATE_FILE="$STATE_DIR/state"
    IRQ_FILE="$STATE_DIR/irq"
    mkdir -p "$STATE_DIR"; chmod 0755 "$STATE_DIR"
    cat > "$STATE_FILE"; chmod 0644 "$STATE_FILE"
}

# Run load_saved_state in a subshell (die exits) and report its status.
loads() { ( load_saved_state "$1" ) >/dev/null 2>&1; }

# Run load_saved_state in a subshell, so that an abort inside it (die, or an
# arithmetic/expansion error in a broken implementation) cannot take the
# harness down, then import the R_* results. `declare -p` output is shell-
# quoted by bash itself, so reading it back is safe here.
R_VARS="R_PARANOID R_NO_TURBO R_MIN_PCT R_MAX_PCT R_DEFAULT_IRQ_MASK R_GOVERNORS R_OFFLINED R_IRQS R_REJECTED"
load_isolated() {
    # shellcheck disable=SC2086
    unset $R_VARS
    # shellcheck disable=SC2086
    ( load_saved_state "$1" >/dev/null 2>&1 && declare -p $R_VARS ) > "$TMP/.loaded" 2>/dev/null
    source <(sed 's/^declare /declare -g /' "$TMP/.loaded")
}

VALID_STATE='# saved by bench_env.sh
SAVED_PARANOID=4
SAVED_NO_TURBO=0
SAVED_MIN_PCT=9
SAVED_MAX_PCT=100
SAVED_GOVERNORS="0:powersave 1:powersave 2:powersave 3:powersave 4:powersave 5:powersave 6:powersave 7:powersave "
SAVED_OFFLINED="4 5 6 7"
SAVED_DEFAULT_IRQ_MASK=ff
MEASURE_CPUS="3 1"'

echo "== valid state is accepted and parsed"
new_case valid <<< "$VALID_STATE"
printf '9 0-7\n120 0-7\n148 0,2\n' > "$IRQ_FILE"; chmod 0644 "$IRQ_FILE"
load_isolated "$ME"
expect "paranoid parsed"             '[ "$R_PARANOID" = 4 ]'
expect "no_turbo parsed"             '[ "$R_NO_TURBO" = 0 ]'
expect "min/max pct parsed"          '[ "$R_MIN_PCT" = 9 ] && [ "$R_MAX_PCT" = 100 ]'
expect "8 governors parsed"          '[ "${#R_GOVERNORS[@]}" -eq 8 ]'
expect "4 offlined CPUs parsed"      '[ "${R_OFFLINED[*]}" = "4 5 6 7" ]'
expect "default IRQ mask parsed"     '[ "$R_DEFAULT_IRQ_MASK" = ff ]'
expect "3 IRQ entries parsed"        '[ "${#R_IRQS[@]}" -eq 3 ]'
expect "nothing rejected"            '[ "${#R_REJECTED[@]}" -eq 0 ]'

echo "== control: the payload is real (the old 'source' approach executes it)"
new_case control <<< "SAVED_PARANOID=\$(touch $TMP/pwned_by_source)"
( source "$STATE_FILE" ) 2>/dev/null
expect "sourcing the planted file runs the payload" '[ -e "$TMP/pwned_by_source" ]'

echo "== command substitution in values is never executed"
new_case subst <<EOF
SAVED_PARANOID=\$(touch $TMP/pwned1)
SAVED_NO_TURBO=\`touch $TMP/pwned2\`
SAVED_GOVERNORS="0:powersave 1:\$(touch $TMP/pwned3)"
SAVED_OFFLINED="4 \$(touch $TMP/pwned4)"
SAVED_DEFAULT_IRQ_MASK=ff;touch $TMP/pwned5
EOF
printf '9 0-7;touch %s\n9 $(touch %s)\n' "$TMP/pwned6" "$TMP/pwned7" > "$IRQ_FILE"; chmod 0644 "$IRQ_FILE"
load_isolated "$ME"
expect "no payload executed"                 '! ls "$TMP"/pwned[1-7] >/dev/null 2>&1'
expect "paranoid falls back to 4"            '[ "$R_PARANOID" = 4 ]'
expect "no_turbo rejected"                   '[ -z "$R_NO_TURBO" ]'
expect "malicious governor dropped"          '[ "${R_GOVERNORS[*]}" = "0:powersave" ]'
expect "malicious offlined entry dropped"    '[ "${R_OFFLINED[*]}" = "4" ]'
expect "malicious IRQ mask rejected"         '[ -z "$R_DEFAULT_IRQ_MASK" ]'
expect "malicious IRQ lines dropped"         '[ "${#R_IRQS[@]}" -eq 0 ]'

echo "== arithmetic injection is never evaluated"
new_case arith <<EOF
SAVED_PARANOID=a[\$(touch $TMP/pwned_arith1)]
SAVED_MIN_PCT=a[\$(touch $TMP/pwned_arith2)]
SAVED_MAX_PCT=x[\$(touch $TMP/pwned_arith3)]+1
EOF
load_isolated "$ME"
expect "no arithmetic payload executed"      '! ls "$TMP"/pwned_arith* >/dev/null 2>&1'
expect "pct values rejected"                 '[ -z "$R_MIN_PCT" ] && [ -z "$R_MAX_PCT" ]'

echo "== out-of-range and malformed values are rejected"
new_case range <<'EOF'
SAVED_PARANOID=9
SAVED_NO_TURBO=2
SAVED_MIN_PCT=150
SAVED_MAX_PCT=0100
EOF
load_isolated "$ME"
expect "paranoid 9 -> 4"                     '[ "$R_PARANOID" = 4 ]'
expect "no_turbo 2 rejected"                 '[ -z "$R_NO_TURBO" ]'
expect "min_pct 150 rejected"                '[ -z "$R_MIN_PCT" ]'
expect "max_pct with 4 digits rejected"      '[ -z "$R_MAX_PCT" ]'

echo "== leading zeros are decimal, not bash octal"
new_case octal <<'EOF'
SAVED_MIN_PCT=089
SAVED_MAX_PCT=010
EOF
load_isolated "$ME"
expect "089 -> 89 (octal would be an error)" '[ "$R_MIN_PCT" = 89 ]'
expect "010 -> 10 (octal would give 8)"      '[ "$R_MAX_PCT" = 10 ]'

echo "== untrusted files and directories are refused outright"
new_case owner <<< "$VALID_STATE"
expect "state not owned by the expected uid"  '! loads 0'
new_case wwfile <<< "$VALID_STATE"; chmod 0666 "$STATE_FILE"
expect "world-writable state file"            '! loads "$ME"'
new_case gwfile <<< "$VALID_STATE"; chmod 0664 "$STATE_FILE"
expect "group-writable state file"            '! loads "$ME"'
new_case wwdir <<< "$VALID_STATE"; chmod 0777 "$STATE_DIR"
expect "world-writable state directory"       '! loads "$ME"'
new_case link <<< "$VALID_STATE"
mv "$STATE_FILE" "$STATE_DIR/real"; ln -s "$STATE_DIR/real" "$STATE_FILE"
expect "symlinked state file"                 '! loads "$ME"'
new_case irqlink <<< "$VALID_STATE"
printf '9 0-7\n' > "$TMP/elsewhere"; ln -s "$TMP/elsewhere" "$IRQ_FILE"
expect "symlinked IRQ file"                   '! loads "$ME"'
new_case irqww <<< "$VALID_STATE"
printf '9 0-7\n' > "$IRQ_FILE"; chmod 0666 "$IRQ_FILE"
expect "world-writable IRQ file"              '! loads "$ME"'

echo "== script contains no source/eval of its own"
expect "no 'source' or 'eval' outside comments" \
    '[ "$(grep -nwE "source|eval" "$ROOT/scripts/bench_env.sh" | grep -vcE "^[0-9]+:\s*#")" -eq 0 ]'

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
