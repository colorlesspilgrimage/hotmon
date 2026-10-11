#!/usr/bin/env bash
# Purpose: Build a Debian binary package from a source tarball.
# Arguments: <version> <source-tarball> <output-dir>
# Outputs: the .deb path on stdout.
# Side effects: writes the .deb. Uses a temp directory.
set -euo pipefail

version="${1:-}"
tarball="${2:-}"
output_dir="${3:-}"

# In UTF-8 locales, bash [0-9] can match non-ASCII digits. Use the ASCII digit list.
if [[ ! "$version" =~ ^[0123456789]+\.[0123456789]+\.[0123456789]+$ ]]; then
  echo "build-deb: bad version" >&2
  exit 2
fi

if [[ -z "$output_dir" ]]; then
  echo "build-deb: output dir missing" >&2
  exit 2
fi

if [[ ! -f "$tarball" ]]; then
  echo "build-deb: tarball not found" >&2
  exit 3
fi

tarball="$(realpath -- "$tarball")"
mkdir -p -- "$output_dir"
output_dir="$(realpath -m -- "$output_dir")"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
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
