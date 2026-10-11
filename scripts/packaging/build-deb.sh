#!/usr/bin/env bash
# Purpose: Build a Debian binary package from a source tarball.
# Arguments: <version> <source-tarball> <output-dir>
# Outputs: the .deb path on stdout.
# Side effects: writes the .deb. Uses a temp directory.
set -euo pipefail

version="${1:-}"
tarball="${2:-}"
output_dir="${3:-}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source -- "${script_dir}/common.sh"

require_version build-deb "$version"
require_output_dir build-deb "$output_dir"
require_tarball build-deb "$tarball"

tarball="$(realpath -- "$tarball")"
output_dir="$(resolve_output_dir "$output_dir")"
repo_root="$(packaging_repo_root)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

if ! tar -xzf "$tarball" -C "$work"; then
  echo "build-deb: cannot unpack tarball" >&2
  exit 1
fi
src="${work}/hotmon-${version}"
if [[ ! -d "$src" ]]; then
  echo "build-deb: unpacked source missing" >&2
  exit 1
fi

cp -r -- "${repo_root}/packaging/debian" "${src}/debian"
chmod +x -- "${src}/debian/rules"
cp -- "$tarball" "${work}/hotmon_${version}.orig.tar.gz"

(
  cd -- "$src"
  dpkg-buildpackage -us -uc -b
)

shopt -s nullglob
debs=( "${work}/hotmon_${version}-1_"*.deb )
shopt -u nullglob
if [[ ${#debs[@]} -eq 0 ]]; then
  echo "build-deb: no package file" >&2
  exit 1
fi

cp -- "${debs[@]}" "${output_dir}/"
printf '%s\n' "${output_dir}/$(basename -- "${debs[0]}")"
