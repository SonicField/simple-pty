#!/bin/sh
set -eu

unset CDPATH
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
program=${PROGRAM:-$project_root/simple-pty}
case $program in
    /*) ;;
    *) program="$project_root/${program#./}" ;;
esac

command -v gdb >/dev/null || {
    printf 'FAIL: gdb is required for test-tools\n' >&2
    exit 1
}
command -v vim >/dev/null || {
    printf 'FAIL: vim is required for test-tools\n' >&2
    exit 1
}

test_root=$(mktemp -d "${TMPDIR:-/tmp}/simple-pty-tools.XXXXXX")
export SIMPLE_PTY_DIR="$test_root/sessions"
handle=

cleanup() {
    if [ -n "$handle" ]; then
        "$program" stop "$handle" >/dev/null 2>&1 || true
        "$program" remove "$handle" >/dev/null 2>&1 || true
    fi
    rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

handle=$(TERM=xterm-256color "$program" start -- \
    gdb -q -iex 'set debuginfod enabled off')
"$program" wait-output "$handle" '(gdb)' --timeout=10000
position=$("$program" position "$handle")
"$program" send "$handle" 'printf "GDB_OK\n"'
"$program" wait-output "$handle" GDB_OK --from="$position" --timeout=10000
"$program" send "$handle" quit
[ "$("$program" wait "$handle" --timeout=10000)" = 0 ]
"$program" remove "$handle"
handle=

touch "$test_root/vim-file"
handle=$(TERM=xterm-256color "$program" start --rows=24 --cols=80 -- \
    vim -Nu NONE -n "$test_root/vim-file")
alternate_screen=$(printf '\033[?1049h')
"$program" wait-output "$handle" "$alternate_screen" --timeout=10000
"$program" key "$handle" Escape
"$program" send "$handle" ':call append(0, "VIM_OK")'
"$program" send "$handle" ':wq'
[ "$("$program" wait "$handle" --timeout=10000)" = 0 ]
"$program" remove "$handle"
handle=
grep -Fx VIM_OK "$test_root/vim-file" >/dev/null

printf 'GDB and Vim integration tests: PASS\n'
