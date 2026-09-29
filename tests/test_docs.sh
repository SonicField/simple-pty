#!/bin/sh
set -eu

for document in README.md SKILL.md docs/CONTRACT.md; do
    [ -s "$document" ] || {
        printf 'FAIL: missing or empty %s\n' "$document" >&2
        exit 1
    }
done

if grep -Eiq 'Codex|Claude|OpenAI|Anthropic|Gemini' SKILL.md; then
    printf 'FAIL: SKILL.md is tied to a particular agent provider\n' >&2
    exit 1
fi

for command in start write send key output position wait-output resize status wait stop remove list; do
    grep -Eq "simple-pty[[:space:]]+$command([[:space:]]|$)" README.md || {
        printf 'FAIL: README.md does not document %s\n' "$command" >&2
        exit 1
    }
done

grep -F 'simple-termshot' README.md >/dev/null
grep -F 'simple-termshot' SKILL.md >/dev/null
printf 'documentation checks: PASS\n'
