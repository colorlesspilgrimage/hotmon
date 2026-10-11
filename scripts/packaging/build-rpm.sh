#!/usr/bin/env bash
# Purpose: Build an RPM package from a source tarball.
# Arguments: <version> <source-tarball> <output-dir>
# Outputs: the .rpm path on stdout.
# Side effects: writes the .rpm. Uses a temp rpmbuild tree.
set -euo pipefail

version="${1:-}"
tarball="${2:-}"
output_dir="${3:-}"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "build-rpm: bad version" >&2
  exit 2
fi

if [[ ! -f "$tarball" ]]; then
  echo "build-rpm: tarball not found" >&2
  exit 3
fi

tarball="$(realpath -- "$tarball")"
mkdir -p -- "$output_dir"
output_dir="$(realpath -m -- "$output_dir")"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

mkdir -p -- "${work}/SOURCES" "${work}/SPECS" "${work}/BUILD" "${work}/RPMS" "${work}/SRPMS"
cp -- "$tarball" "${work}/SOURCES/hotmon-${version}.tar.gz"
cp -- "${repo_root}/packaging/rpm/hotmon.spec" "${work}/SPECS/hotmon.spec"

rpmbuild -bb --define "_topdir ${work}" "${work}/SPECS/hotmon.spec"

shopt -s nullglob
rpms=( "${work}/RPMS/"*/"hotmon-${version}-1"*.rpm )
shopt -u nullglob
copied=0
for rpm in "${rpms[@]}"; do
  base="$(basename -- "$rpm")"
  case "$base" in
    hotmon-debuginfo-*|hotmon-debugsource-*)
      continue
      ;;
  esac
  cp -- "$rpm" "${output_dir}/"
  printf '%s\n' "${output_dir}/${base}"
  copied=1
done

if [[ "$copied" -ne 1 ]]; then
  echo "build-rpm: no package file" >&2
  exit 1
fi
