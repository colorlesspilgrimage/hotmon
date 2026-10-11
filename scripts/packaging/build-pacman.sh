#!/usr/bin/env bash
# Purpose: Build a pacman package from a source tarball.
# Arguments: <version> <source-tarball> <output-dir>
# Outputs: the package path on stdout.
# Side effects: writes the package. As root, creates user builder.
# As root, builder builds in a temp dir and root copies the package out (CWE-732).
set -euo pipefail

version="${1:-}"
tarball="${2:-}"
output_dir="${3:-}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source -- "${script_dir}/common.sh"

require_version build-pacman "$version"
require_output_dir build-pacman "$output_dir"
require_tarball build-pacman "$tarball"

tarball="$(realpath -- "$tarball")"
output_dir="$(resolve_output_dir "$output_dir")"

# Run as root. makepkg refuses root, so user builder runs this script again.
# Root never gives <output-dir> to builder. Builder writes only into a temp dir.
# The EXIT trap runs after this function returns. Its variables must stay global.
build_as_root() {
  if ! id builder >/dev/null 2>&1; then
    useradd -m builder
  fi
  local script_path
  local built=()
  local pkg=""
  builder_dest="$(mktemp -d)"
  trap 'rm -rf -- "$builder_dest"' EXIT
  chown -- builder "$builder_dest"
  script_path="$(realpath -- "$0")"
  runuser -u builder -- "$script_path" "$version" "$tarball" "$builder_dest" > /dev/null
  shopt -s nullglob
  built=( "${builder_dest}/hotmon-${version}-1-"*.pkg.tar.zst )
  shopt -u nullglob
  if [[ ${#built[@]} -eq 0 ]]; then
    echo "build-pacman: no package file" >&2
    exit 1
  fi
  for pkg in "${built[@]}"; do
    # builder owns builder_dest. Copy regular files only, never a symlink target.
    if [[ -L "$pkg" || ! -f "$pkg" ]]; then
      echo "build-pacman: package is not a regular file" >&2
      exit 1
    fi
    cp -- "$pkg" "${output_dir}/"
  done
  printf '%s\n' "${output_dir}/$(basename -- "${built[0]}")"
}

# Run as the build user. Build the package with makepkg into PKGDEST.
build_as_user() {
  local repo_root
  local tarball_url
  local pkgs=()
  repo_root="$(packaging_repo_root)"
  work="$(mktemp -d)"
  trap 'rm -rf -- "$work"' EXIT
  # The script prints resolved paths. Resolve the temp dir too, in case TMPDIR is a symlink.
  work="$(realpath -- "$work")"

  tarball_url="file://$(encode_url_path "$tarball")"
  cp -- "${repo_root}/packaging/aur/PKGBUILD" "${work}/PKGBUILD"
  (
    cd -- "$work"
    env HOTMON_SOURCE_URL="$tarball_url" PKGDEST="$output_dir" makepkg --force --skipinteg
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
}

if [[ "$(id -u)" -eq 0 ]]; then
  build_as_root
else
  build_as_user
fi
