#!/usr/bin/env bash
# Purpose: Check that packaging scripts reject bad version and tag text.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.ScriptsRejectBadInput or FAIL: <reason>
# Side effects: uses temp directories. Does not use the network.
# Security: CWE-78, ASVS V5.3.8, CWE-22. Bad text must not run as a command.
set -euo pipefail

source_dir="${1:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$source_dir" ]]; then
  fail "missing source dir"
fi

scripts="${source_dir}/scripts/packaging"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cd -- "$work"
missing="${work}/missing.tar.gz"

inject='$'" (touch X)"
inject="${inject/ /}"
bad_versions=(
  ''
  '1.0'
  '1.0.0.0'
  'v1.0.0'
  '1.0.0; touch X'
  "$inject"
  '../1.0.0'
  '1.0.0 '
  '-1.0.0'
  '1.0.-1'
)

expect_status() {
  local want="$1"
  shift
  local status=0
  "$@" || status=$?
  if [[ "$status" -ne "$want" ]]; then
    fail "command exited ${status}, want ${want}: $*"
  fi
}

index=0
for version in "${bad_versions[@]}"; do
  index=$((index + 1))
  probe="${work}/out-${index}"
  rm -f -- "${work}/X"
  expect_status 2 "${scripts}/make-source-tarball.sh" "$version" "$probe"
  expect_status 2 "${scripts}/build-deb.sh" "$version" "$missing" "$probe"
  expect_status 2 "${scripts}/build-rpm.sh" "$version" "$missing" "$probe"
  expect_status 2 "${scripts}/build-pacman.sh" "$version" "$missing" "$probe"
  expect_status 2 "${scripts}/update-aur-checksum.sh" "$version"
  if [[ -e "$probe" || -e "${work}/X" ]]; then
    fail "bad version created a file: ${version}"
  fi
done

for version in 0.0.0 10.20.30; do
  probe="${work}/good-out"
  rm -rf -- "$probe"
  expect_status 3 "${scripts}/build-deb.sh" "$version" "$missing" "$probe"
  expect_status 3 "${scripts}/build-rpm.sh" "$version" "$missing" "$probe"
  expect_status 3 "${scripts}/build-pacman.sh" "$version" "$missing" "$probe"
  if [[ -e "$probe" ]]; then
    fail "missing tarball created an output dir"
  fi
done

bad_tags=(
  '1.0.0'
  'v1.0'
  'vx.y.z'
  'v1.0.0/../x'
  'refs/tags/v1.0.0'
)
for tag in "${bad_tags[@]}"; do
  expect_status 2 "${scripts}/check-tag-matches-version.sh" "$tag"
done

read_out="$("${scripts}/read-version.sh")"
if [[ "$read_out" != "0.1.0" ]]; then
  fail "read-version printed ${read_out}"
fi

expect_status 0 "${scripts}/check-tag-matches-version.sh" "v${read_out}"
shape_status=0
"${scripts}/check-tag-matches-version.sh" "v1.0.0" || shape_status=$?
if [[ "$shape_status" -eq 2 ]]; then
  fail "v1.0.0 is a valid tag shape"
fi
expect_status 1 "${scripts}/check-tag-matches-version.sh" "v999.0.0"

sum_dir="${work}/sums"
mkdir -p -- "$sum_dir"
: > "${sum_dir}/a.deb"
: > "${sum_dir}/b.rpm"
: > "${sum_dir}/c.pkg.tar.zst"
: > "${sum_dir}/d.tar.gz"
"${scripts}/make-checksums.sh" "$sum_dir"
(
  cd -- "$sum_dir"
  sha256sum -c SHA256SUMS
)

short_dir="${work}/short"
mkdir -p -- "$short_dir"
: > "${short_dir}/a.deb"
: > "${short_dir}/b.rpm"
: > "${short_dir}/c.pkg.tar.zst"
expect_status 1 "${scripts}/make-checksums.sh" "$short_dir"

dash_dir="${work}/dash"
mkdir -p -- "$dash_dir"
: > "${dash_dir}/-n.deb"
: > "${dash_dir}/b.rpm"
: > "${dash_dir}/c.pkg.tar.zst"
: > "${dash_dir}/d.tar.gz"
"${scripts}/make-checksums.sh" "$dash_dir"
if ! grep -F -- '-n.deb' "${dash_dir}/SHA256SUMS" >/dev/null; then
  fail "checksum file skipped -n.deb"
fi
(
  cd -- "$dash_dir"
  sha256sum -c SHA256SUMS
)

echo "PASS: Packaging.ScriptsRejectBadInput"
