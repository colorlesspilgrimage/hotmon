#!/usr/bin/env bash
# Purpose: Write SHA256SUMS for release package files.
# Arguments: <directory>
# Outputs: SHA256SUMS in that directory.
# Side effects: creates or replaces SHA256SUMS.
set -euo pipefail

directory="${1:-}"
if [[ -z "$directory" || ! -d "$directory" ]]; then
  echo "make-checksums: directory missing" >&2
  exit 1
fi

directory="$(realpath -m -- "$directory")"
cd -- "$directory"

shopt -s nullglob
debs=( *.deb )
rpms=( *.rpm )
pkgs=( *.pkg.tar.zst )
tars=( *.tar.gz )
shopt -u nullglob

if [[ ${#debs[@]} -eq 0 || ${#rpms[@]} -eq 0 || ${#pkgs[@]} -eq 0 || ${#tars[@]} -eq 0 ]]; then
  echo "make-checksums: a package pattern matched nothing" >&2
  exit 1
fi

mapfile -t names < <(printf '%s\n' "${debs[@]}" "${rpms[@]}" "${pkgs[@]}" "${tars[@]}" | LC_ALL=C sort)
if [[ ${#names[@]} -lt 4 ]]; then
  echo "make-checksums: fewer than 4 files" >&2
  exit 1
fi

sha256sum -- "${names[@]}" > SHA256SUMS
