#!/bin/sh
set -eu

unset CDPATH
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
program=${PROGRAM:-$project_root/simple-pty}
fixture=${FIXTURE:-$project_root/build/pty_fixture}
case $program in
    /*) ;;
    *) program="$project_root/${program#./}" ;;
esac
case $fixture in
    /*) ;;
    *) fixture="$project_root/${fixture#./}" ;;
esac
test_root=$(mktemp -d "${TMPDIR:-/tmp}/simple-pty-test.XXXXXX")
export SIMPLE_PTY_DIR="$test_root/sessions"
handles=

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

cleanup() {
    for handle in $handles; do
        "$program" stop "$handle" >/dev/null 2>&1 || true
        "$program" remove "$handle" >/dev/null 2>&1 || true
    done
    rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

start() {
    new_handle=$($program start "$@")
    handles="$new_handle $handles"
}

finish() {
    "$program" stop "$new_handle" >/dev/null 2>&1 || true
    "$program" remove "$new_handle"
    remaining=
    for item in $handles; do
        [ "$item" = "$new_handle" ] || remaining="$remaining $item"
    done
    handles=$remaining
}

expect_status() {
    expected=$1
    shift
    set +e
    "$@" >"$test_root/stdout" 2>"$test_root/stderr"
    actual=$?
    set -e
    [ "$actual" -eq "$expected" ] ||
        fail "expected status $expected, got $actual: $*"
}

file_mode() {
    stat -c %a "$1" 2>/dev/null || stat -f %Lp "$1"
}

# Direct argv, real controlling PTY, cwd, and target exit status.
mkdir "$test_root/work"
literal="\$(touch SHOULD_NOT_EXIST); *"
start --rows=31 --cols=97 --cwd="$test_root/work" --name=probe -- \
    "$fixture" probe "two words" "$literal"
"$program" wait "$new_handle" --timeout=2000 >"$test_root/exit"
[ "$(cat "$test_root/exit")" = 0 ] || fail "wait did not report exit 0"
"$program" output "$new_handle" >"$test_root/probe"
grep -F "TTY 111 FG 1 CWD $test_root/work ARGC 2" "$test_root/probe" >/dev/null ||
    fail "target did not receive a real controlling PTY or requested cwd"
grep -F 'ARG 0 <two words>' "$test_root/probe" >/dev/null ||
    fail "space-containing argument changed"
grep -F "ARG 1 <$literal>" "$test_root/probe" >/dev/null ||
    fail "shell metacharacters changed"
[ ! -e "$test_root/work/SHOULD_NOT_EXIST" ] || fail "start invoked a shell"
[ "$("$program" status "$new_handle")" = "exited 0" ] ||
    fail "status did not report target exit"
finish

# Failed exec is synchronous, prints no handle, and leaves no session.
expect_status 1 "$program" start -- /simple-pty-program-that-does-not-exist
[ ! -s "$test_root/stdout" ] || fail "failed start printed a handle"
[ -z "$("$program" list)" ] || fail "failed start left session state"

# write preserves bytes; send adds CR; key uses documented stable sequences.
old_umask=$(umask)
umask 0777
start -- "$fixture" bytes 3
umask "$old_umask"
[ "$(file_mode "$SIMPLE_PTY_DIR")" = 700 ] ||
    fail "session root is not mode 700"
[ "$(file_mode "$SIMPLE_PTY_DIR/$new_handle")" = 700 ] ||
    fail "session directory is not mode 700"
for state_file in output input control; do
    [ "$(file_mode "$SIMPLE_PTY_DIR/$new_handle/$state_file")" = 600 ] ||
        fail "$state_file is not mode 600"
done
"$program" wait-output "$new_handle" READY --timeout=2000
printf 'A\000B' | "$program" write "$new_handle"
"$program" wait-output "$new_handle" '41 00 42' --timeout=2000
finish

start -- "$fixture" bytes 3
"$program" wait-output "$new_handle" READY --timeout=2000
"$program" send "$new_handle" hi
"$program" wait-output "$new_handle" '68 69 0d' --timeout=2000
finish

start -- "$fixture" bytes 36
"$program" wait-output "$new_handle" READY --timeout=2000
for key in Enter Escape Tab Backspace Up Down Left Right Home End \
    PageUp PageDown Delete Ctrl-A Ctrl-Z; do
    "$program" key "$new_handle" "$key"
done
"$program" wait-output "$new_handle" \
    '0d 1b 09 7f 1b 5b 41 1b 5b 42 1b 5b 44 1b 5b 43 1b 5b 48 1b 5b 46 1b 5b 35 7e 1b 5b 36 7e 1b 5b 33 7e 01 1a' \
    --timeout=2000
finish

# Resize changes the kernel PTY size and produces SIGWINCH in the target.
start --rows=20 --cols=70 -- "$fixture" resize
"$program" wait-output "$new_handle" 'SIZE 20 70' --timeout=2000
offset=$("$program" position "$new_handle")
"$program" resize "$new_handle" 42 101
"$program" wait-output "$new_handle" 'SIZE 42 101' \
    --from="$offset" --timeout=2000
finish

# A literal split between separate writes remains detectable.
start -- "$fixture" split
"$program" wait-output "$new_handle" SPLIT_PATTERN --timeout=2000
[ "$("$program" output "$new_handle")" = SPLIT_PATTERN ] ||
    fail "output did not preserve raw bytes"
"$program" wait "$new_handle" --timeout=2000 >/dev/null
finish

# Offsets exclude old output, and waits use the documented timeout status.
start -- "$fixture" bytes 1
"$program" wait-output "$new_handle" READY --timeout=2000
offset=$("$program" position "$new_handle")
expect_status 3 "$program" wait-output "$new_handle" READY \
    --from="$offset" --timeout=20
expect_status 3 "$program" wait "$new_handle" --timeout=20
printf Z | "$program" write "$new_handle"
"$program" wait "$new_handle" --timeout=2000 >/dev/null
finish

# Removing a running session is rejected; stop retains evidence; list is sorted.
start -- "$fixture" resize
first=$new_handle
start -- "$fixture" resize
second=$new_handle
expect_status 2 "$program" remove "$first"
listed=$("$program" list)
[ "$listed" = "$(printf '%s\n%s\n' "$first" "$second" | sort)" ] ||
    fail "list is not a sorted handle list"
"$program" stop "$first"
[ "$("$program" status "$first" | cut -d' ' -f1)" = exited ] ||
    fail "stop did not retain an exit status"

start -- "$fixture" ignore-term
"$program" stop "$new_handle"
[ "$("$program" status "$new_handle")" = "exited 137" ] ||
    fail "stop did not escalate from SIGTERM to SIGKILL"
finish

# Argument and missing-session errors.
expect_status 4 "$program" start "$fixture"
expect_status 4 "$program" key "$second" NotAKey
expect_status 2 "$program" status deadbeef

printf 'CLI contract tests: PASS\n'
