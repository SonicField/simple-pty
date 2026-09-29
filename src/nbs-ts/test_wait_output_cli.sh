#!/bin/sh
set -eu

program=${PROGRAM:-./nbs-ts}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/simple-pty-wait-cli.XXXXXX")
test_home="$test_root/home"
handle=

cleanup() {
    if [ -n "$handle" ]; then
        HOME="$test_home" "$program" kill "$handle" >/dev/null 2>&1 || true
    fi
    rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

mkdir "$test_home"

handle=$(HOME="$test_home" "$program" create \
    "printf SPLIT_; sleep 1; printf PATTERN; sleep 1")
HOME="$test_home" "$program" wait-pattern "$handle" SPLIT_PATTERN \
    --timeout=3
actual=$(HOME="$test_home" "$program" read "$handle")
if [ "$actual" != SPLIT_PATTERN ]; then
    printf 'FAIL: raw output differs: [%s]\n' "$actual" >&2
    exit 1
fi
HOME="$test_home" "$program" kill "$handle" >/dev/null
handle=

handle=$(HOME="$test_home" "$program" create \
    "printf OLD; sleep 1; printf NEW_TARGET; sleep 1")
session_dir="$test_home/.nbs-ts/sessions/$handle"
offset=$(wc -c <"$session_dir/output.log" | tr -d ' ')
HOME="$test_home" "$program" wait-pattern "$handle" NEW_TARGET \
    --from="$offset" --timeout=3

set +e
HOME="$test_home" "$program" wait-pattern "$handle" OLD \
    --from="$offset" --timeout=1
status=$?
set -e
if [ "$status" -ne 3 ]; then
    printf 'FAIL: --from matched old output; status=%d\n' "$status" >&2
    exit 1
fi

printf 'wait-output CLI tests: PASS\n'
