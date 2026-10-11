#!/usr/bin/env bash
# Purpose: Build a pacman package from a source tarball.
# Arguments: <version> <source-tarball> <output-dir>
# Outputs: the package path on stdout.
# Side effects: writes the package. As root, creates user builder.
# As root, builder writes only into a temp dir. Root copies the package to <output-dir>.
# Root never gives <output-dir> to builder (CWE-732).
set -euo pipefail

version="${1:-}"
tarball="${2:-}"
output_dir="${3:-}"

if [[ ! "$version" =~ ^[0123456789]+\.[0123456789]+\.[0123456789]+$ ]]; then
  echo "build-pacman: bad version" >&2
  exit 2
fi

if [[ -z "$output_dir" ]]; then
  echo "build-pacman: output dir missing" >&2
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
  exit 0
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

# Print a path as a file URL path. curl rejects a raw space, '#', '?' or '%' in a URL.
# Keeps ASCII letters, digits, and "/._~-". Encodes every other byte as %XX.
encode_url_path() {
  local LC_ALL=C
  local text="$1"
  local encoded=""
  local byte=""
  local hex=""
  local index=0
  for (( index = 0; index < ${#text}; index++ )); do
    byte="${text:index:1}"
    case "$byte" in
      [abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._~-])
        encoded+="$byte"
        ;;
      *)
        printf -v hex '%%%02X' "'${byte}"
        encoded+="$hex"
        ;;
    esac
  done
  printf '%s' "$encoded"
}

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
