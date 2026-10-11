#!/usr/bin/env bash
# Purpose: Check that package files use the CMake version.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.MetadataVersionsMatch or FAIL: <reason>
# Side effects: none.
set -euo pipefail

source_dir="${1:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$source_dir" ]]; then
  fail "missing source dir"
fi

version_ok() {
  [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

cmake_file="${source_dir}/CMakeLists.txt"
pkgbuild="${source_dir}/packaging/aur/PKGBUILD"
spec="${source_dir}/packaging/rpm/hotmon.spec"
changelog="${source_dir}/packaging/debian/changelog"
control="${source_dir}/packaging/debian/control"
rules="${source_dir}/packaging/debian/rules"

cmake_version="$(sed -n 's/^project(hotmon VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' "$cmake_file" | head -n 1)"
pkg_version="$(sed -n 's/^pkgver=//p' "$pkgbuild" | head -n 1)"
rpm_version="$(sed -n 's/^Version:[[:space:]]*//p' "$spec" | head -n 1)"
deb_line="$(head -n 1 -- "$changelog")"
deb_version="$(sed -n 's/^hotmon (\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)-1).*/\1/p' <<< "$deb_line")"

for value in "$cmake_version" "$pkg_version" "$rpm_version" "$deb_version"; do
  if ! version_ok "$value"; then
    fail "bad version value: ${value}"
  fi
done

for value in "$pkg_version" "$rpm_version" "$deb_version"; do
  if [[ "$value" != "$cmake_version" ]]; then
    fail "versions differ: cmake=${cmake_version} pkg=${pkg_version} rpm=${rpm_version} deb=${deb_version}"
  fi
done

if [[ ! -x "$rules" ]]; then
  fail "debian/rules is not executable"
fi

for file in "$pkgbuild" "$spec" "$control"; do
  text="$(tr '[:upper:]' '[:lower:]' < "$file")"
  for token in ncurses yyjson iw iproute; do
    if [[ "$text" != *"$token"* ]]; then
      fail "${file} does not list ${token}"
    fi
  done
done

echo "PASS: Packaging.MetadataVersionsMatch"
