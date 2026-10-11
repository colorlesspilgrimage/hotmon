#!/usr/bin/env bash
# Purpose: Check that an installed hotmon binary starts and links.
# Arguments: none.
# Outputs: PASS: installed package works
# Exit codes: 0 on pass. 1 on any failed check, a missing ldd, or a failed ldd.
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

# A missing ldd must fail the check. Otherwise a missing library goes unseen.
if ! command -v ldd >/dev/null 2>&1; then
  fail "ldd is not on PATH"
fi
ldd_output=""
if ! ldd_output="$(ldd -- "$binary" 2>&1)"; then
  fail "ldd failed for ${binary}"
fi
if [[ "$ldd_output" == *"not found"* ]]; then
  fail "ldd reports a missing library"
fi

stderr_file="$(mktemp)"
trap 'rm -f -- "$stderr_file"' EXIT
status=0
hotmon extra-argument > /dev/null 2> "$stderr_file" || status=$?

if [[ "$status" -ne 2 ]]; then
  fail "hotmon extra-argument exited ${status}, want 2"
fi

# Command substitution removes the trailing newline of the usage line.
actual="$(cat -- "$stderr_file")"
if [[ "$actual" != "Usage: hotmon" ]]; then
  fail "stderr is not Usage: hotmon"
fi

echo "PASS: installed package works"
