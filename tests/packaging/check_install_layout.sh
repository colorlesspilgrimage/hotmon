#!/usr/bin/env bash
# Purpose: Check the CMake install layout for hotmon.
# Arguments: <build-dir> <source-dir>
# Outputs: PASS: Packaging.InstallLayout or FAIL: <reason>
# Side effects: installs into a temp prefix.
# Security: CWE-22. The prefix is a temp path, not a version string.
set -euo pipefail

build_dir="${1:-}"
source_dir="${2:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$build_dir" || ! -d "$source_dir" ]]; then
  fail "missing build dir or source dir"
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
prefix="${work}/prefix"

if ! cmake --install "$build_dir" --prefix "$prefix"; then
  fail "first install failed"
fi

if [[ ! -x "${prefix}/bin/hotmon" ]]; then
  fail "bin/hotmon is missing or not executable"
fi
if [[ ! -f "${prefix}/share/doc/hotmon/README.md" ]]; then
  fail "README doc is missing"
fi
if ! cmp -s -- "${source_dir}/README.md" "${prefix}/share/doc/hotmon/README.md"; then
  fail "installed README does not match the source README"
fi

extra="$(find "$prefix" \( -name 'hotmon_tests' -o -name '*.a' \))"
if [[ -n "$extra" ]]; then
  fail "tests or static lib were installed"
fi

if ! cmake --install "$build_dir" --prefix "$prefix"; then
  fail "second install failed"
fi

echo "PASS: Packaging.InstallLayout"
