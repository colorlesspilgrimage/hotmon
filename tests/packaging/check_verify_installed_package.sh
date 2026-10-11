#!/usr/bin/env bash
# Purpose: Check that verify-installed-package.sh fails on each broken install.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.VerifyInstalledPackage or FAIL: <reason>
# Side effects: uses temp directories with stub commands. Does not use the network.
# Security: CWE-754. A missing or failed ldd must not give a pass.
set -euo pipefail

source_dir="${1:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$source_dir" ]]; then
  fail "missing source dir"
fi

verify="${source_dir}/scripts/packaging/verify-installed-package.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

# Each case gets a PATH with only these tools, a hotmon stub, and an optional ldd stub.
base_tools=( bash cat grep mktemp rm )

# make_case <name> <hotmon-status|none> <hotmon-stderr> <ldd-mode: ok|missing-lib|fails|absent>
make_case() {
  local name="$1"
  local hotmon_status="$2"
  local hotmon_stderr="$3"
  local ldd_mode="$4"
  local dir="${work}/${name}"
  local tool=""
  mkdir -p -- "$dir"
  for tool in "${base_tools[@]}"; do
    ln -s -- "$(command -v "$tool")" "${dir}/${tool}"
  done
  if [[ "$hotmon_status" != none ]]; then
    printf '#!/usr/bin/env bash\nprintf %%s %q >&2\nexit %d\n' "$hotmon_stderr" "$hotmon_status" > "${dir}/hotmon"
    chmod +x -- "${dir}/hotmon"
  fi
  case "$ldd_mode" in
    ok) printf '#!/usr/bin/env bash\necho "libc.so.6 => /usr/lib/libc.so.6"\n' > "${dir}/ldd" ;;
    missing-lib) printf '#!/usr/bin/env bash\necho "libyyjson.so.0 => not found"\n' > "${dir}/ldd" ;;
    fails) printf '#!/usr/bin/env bash\necho "not a dynamic executable" >&2\nexit 1\n' > "${dir}/ldd" ;;
    absent) ;;
    *) fail "unknown ldd mode ${ldd_mode}" ;;
  esac
  if [[ -f "${dir}/ldd" ]]; then
    chmod +x -- "${dir}/ldd"
  fi
}

# expect_verify <name> <want-status>
expect_verify() {
  local name="$1"
  local want="$2"
  local status=0
  PATH="${work}/${name}" "$verify" > /dev/null 2>&1 || status=$?
  if [[ "$status" -ne "$want" ]]; then
    fail "case ${name} exited ${status}, want ${want}"
  fi
}

usage=$'Usage: hotmon\n'
make_case good 2 "$usage" ok
make_case ldd-missing-lib 2 "$usage" missing-lib
make_case ldd-fails 2 "$usage" fails
make_case ldd-absent 2 "$usage" absent
make_case hotmon-absent none "" ok
make_case wrong-status 0 "$usage" ok
make_case wrong-stderr 2 $'Usage: hotmon extra\n' ok
make_case empty-stderr 2 "" ok

expect_verify good 0
expect_verify ldd-missing-lib 1
expect_verify ldd-fails 1
expect_verify ldd-absent 1
expect_verify hotmon-absent 1
expect_verify wrong-status 1
expect_verify wrong-stderr 1
expect_verify empty-stderr 1

echo "PASS: Packaging.VerifyInstalledPackage"
