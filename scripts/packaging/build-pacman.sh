#!/usr/bin/env bash
# Purpose: Build a pacman package from a source tarball.
# Arguments: <version> <source-tarball> <output-dir>
# Outputs: the package path on stdout.
# Side effects: writes the package. As root, creates user builder.
set -euo pipefail

version="${1:-}"
tarball="${2:-}"
output_dir="${3:-}"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "build-pacman: bad version" >&2
  exit 2
fi

if [[ ! -f "$tarball" ]]; then
  echo "build-pacman: tarball not found" >&2
  exit 3
fi

tarball="$(realpath -- "$tarball")"
mkdir -p -- "$output_dir"
output_dir="$(realpath -m -- "$output_dir")"

if [[ "$(id -u)" -eq 0 ]]; then
  if ! id builder >/dev/null 2>&1; then
    useradd -m builder
  fi
  chown -- builder "$output_dir"
  script_path="$(realpath -- "$0")"
  exec runuser -u builder -- "$script_path" "$version" "$tarball" "$output_dir"
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

cp -- "${repo_root}/packaging/aur/PKGBUILD" "${work}/PKGBUILD"
(
  cd -- "$work"
  env HOTMON_SOURCE_URL="file://${tarball}" PKGDEST="$output_dir" makepkg --force --skipinteg
)

shopt -s nullglob
pkgs=( "${output_dir}/hotmon-${version}-1-"*.pkg.tar.zst )
if [[ ${#pkgs[@]} -eq 0 ]]; then
  pkgs=( "${work}/hotmon-${version}-1-"*.pkg.tar.zst )
  if [[ ${#pkgs[@]} -gt 0 ]]; then
    cp -- "${pkgs[@]}" "${output_dir}/"
    pkgs=( "${output_dir}/hotmon-${version}-1-"*.pkg.tar.zst )
  fi
fi
shopt -u nullglob

if [[ ${#pkgs[@]} -eq 0 ]]; then
  echo "build-pacman: no package file" >&2
  exit 1
fi

printf '%s\n' "${pkgs[0]}"
