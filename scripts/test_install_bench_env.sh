#!/usr/bin/env bash
# =============================================================================
# test_install_bench_env.sh — tests for install_bench_env.sh's sudo-rule check
#
# `--check` must say whether `setup` and `restore` are password-free, and must
# notice a rule that is wider than those two commands. It decides that by
# parsing a `sudo -l` listing, because `sudo -n -l CMD` cannot tell
# "password-free" from "permitted with a password". These tests feed the parser
# canned listings, so they need neither root nor any particular sudoers setup.
#
# Usage: ./scripts/test_install_bench_env.sh
# =============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=install_bench_env.sh
source "$ROOT/scripts/install_bench_env.sh"
set +eu                    # statuses are inspected here; failures must not abort

PASS=0
FAIL=0
D="$DEST"                  # /usr/local/sbin/mm-bench-env

# is "<expected>" what nopasswd_commands prints for the listing on stdin?
grants() {
    local desc="$1" expected="$2" got
    got="$(nopasswd_commands)"
    if [ "$got" = "$expected" ]; then echo "  PASS  $desc"; PASS=$((PASS + 1))
    else echo "  FAIL  $desc"; echo "        expected: [${expected//$'\n'/ | }]"; echo "        got     : [${got//$'\n'/ | }]"; FAIL=$((FAIL + 1)); fi
}
# is rule_status "<granted>" "<action>" equal to "<expected>"?
status_is() {
    local desc="$1" granted="$2" action="$3" expected="$4" got
    got="$(rule_status "$granted" "$action")"
    if [ "$got" = "$expected" ]; then echo "  PASS  $desc"; PASS=$((PASS + 1))
    else echo "  FAIL  $desc (expected $expected, got $got)"; FAIL=$((FAIL + 1)); fi
}

HDR="User jz may run the following commands on host:"

echo "== the intended rule"
grants "both exact commands are found" "$D setup"$'\n'"$D restore" <<EOF
Matching Defaults entries for jz on host:
    env_reset, mail_badpass, secure_path=/usr/local/sbin\:/usr/local/bin, use_pty

$HDR
    (ALL : ALL) ALL
    (root) NOPASSWD: $D setup, $D restore
EOF
BOTH="$D setup"$'\n'"$D restore"
status_is "setup is exact"                    "$BOTH" setup   exact
status_is "restore is exact"                  "$BOTH" restore exact

echo "== regression: a password-requiring ALL rule is NOT password-free"
# What the old probe got wrong: next to any NOPASSWD entry, 'sudo -n -l CMD'
# reported every command as allowed because of this general rule.
grants "general ALL rule grants nothing password-free" "" <<EOF
$HDR
    (ALL : ALL) ALL
EOF
grants "unrelated NOPASSWD entry does not cover our command" "/usr/bin/apt update" <<EOF
$HDR
    (ALL : ALL) ALL
    (root) NOPASSWD: /usr/bin/apt update
EOF
status_is "setup needs a password next to an unrelated rule"  "/usr/bin/apt update" setup none
status_is "nothing granted -> none"                           ""                    setup none

echo "== partial rule"
grants "only setup is password-free" "$D setup" <<EOF
$HDR
    (ALL : ALL) ALL
    (root) NOPASSWD: $D setup
EOF
status_is "setup exact when only setup is granted"    "$D setup" setup   exact
status_is "restore none when only setup is granted"   "$D setup" restore none

echo "== tags apply until reset"
grants "PASSWD: resets NOPASSWD: within an entry" "/a" <<EOF
$HDR
    (root) NOPASSWD: /a, PASSWD: /b, /c
EOF
grants "NOPASSWD: carries to later commands" "/a"$'\n'"/b" <<EOF
$HDR
    (root) NOPASSWD: /a, /b
EOF
grants "other tags are skipped, in either order" "/a"$'\n'"/b" <<EOF
$HDR
    (root) SETENV: NOPASSWD: /a
    (root) NOPASSWD: NOEXEC: /b
EOF
grants "a tag does not leak into the next entry" "/a" <<EOF
$HDR
    (root) NOPASSWD: /a
    (root) /b
EOF

echo "== only entries that can run as root count"
grants "runas another user is ignored" "" <<EOF
$HDR
    (www-data) NOPASSWD: $D setup
EOF
grants "runas lists and groups are understood" "/a"$'\n'"/b"$'\n'"/c" <<EOF
$HDR
    (www-data, root) NOPASSWD: /a
    (root : root) NOPASSWD: /b
    (ALL : ALL) NOPASSWD: /c
EOF

echo "== wrapped entries are rejoined"
grants "an entry wrapped across lines" "$D setup"$'\n'"$D restore" <<EOF
$HDR
    (root) NOPASSWD: $D setup,
        $D restore
EOF

echo "== rules wider than intended are recognised"
status_is "NOPASSWD: ALL -> broad"                 "ALL"            setup broad
status_is "bare command (any arguments) -> broad"  "$D"             setup broad
status_is "wildcard arguments -> broad"            "$D *"           setup broad
status_is "wildcard that matches the action"       "$D se*"         setup broad
status_is "wildcard that does not match"           "$D re*"         setup none
status_is "longer command is not the action"       "$D setup --cpus 0,2" setup none
expect_broader() {
    local desc="$1" granted="$2" expected="$3" got
    got="$(broader_rules "$granted")"
    if [ "$got" = "$expected" ]; then echo "  PASS  $desc"; PASS=$((PASS + 1))
    else echo "  FAIL  $desc (expected [$expected], got [$got])"; FAIL=$((FAIL + 1)); fi
}
expect_broader "the two exact commands are not flagged"   "$BOTH"                       ""
expect_broader "extra arguments are flagged"              "$BOTH"$'\n'"$D setup --cpus *" "$D setup --cpus *"
expect_broader "bare command is flagged"                  "$D"                          "$D"
expect_broader "NOPASSWD: ALL is reported"                "ALL"                         "ALL"
expect_broader "unrelated commands are not our concern"   "/usr/bin/apt update"         ""

echo "== wildcard patterns are matched, never executed"
MARK="$(mktemp -u)"
status_is "command substitution in a rule is inert" "$D \$(touch $MARK)" setup none
if [ -e "$MARK" ]; then echo "  FAIL  a payload from the listing was executed"; FAIL=$((FAIL + 1)); rm -f -- "$MARK"
else echo "  PASS  nothing from the listing was executed"; PASS=$((PASS + 1)); fi

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ]
