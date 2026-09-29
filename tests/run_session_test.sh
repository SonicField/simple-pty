#!/bin/sh
set -eu

unset CDPATH
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
project_root=$(dirname "$script_dir")
program=${PROGRAM:-$project_root/build/test_session}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/simple-pty-session-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM
mkdir "$test_root/home"
HOME="$test_root/home" "$program"
