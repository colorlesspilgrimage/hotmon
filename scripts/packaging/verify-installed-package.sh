#!/usr/bin/env bash
# Purpose: Check that an installed hotmon binary starts and links.
# Arguments: none.
# Outputs: PASS: installed package works
# Side effects: runs the installed hotmon binary.
set -euo pipefail

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if ! command -v hotmon >/dev/null 2>&1; then
  fail "hotmon is not on PATH"
fi

binary="$(command -v hotmon)"
if ldd "$binary" | grep -F 'not found' >/dev/null; then
  fail "ldd reports a missing library"
fi

stderr_file="$(mktemp)"
trap 'rm -f -- "$stderr_file"' EXIT
status=0
hotmon extra-argument > /dev/null 2> "$stderr_file" || status=$?

if [[ "$status" -ne 2 ]]; then
  fail "hotmon extra-argument exited ${status}, want 2"
fi

actual="$(cat -- "$stderr_file")"
if [[ "$actual" != "Usage: hotmon" && "$actual" != $'Usage: hotmon\n' ]]; then
  fail "stderr is not Usage: hotmon"
fi

echo "PASS: installed package works"
