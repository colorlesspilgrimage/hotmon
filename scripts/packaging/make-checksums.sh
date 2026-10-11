#!/usr/bin/env bash
# Purpose: Write SHA256SUMS for release package files.
# Arguments: <directory>
# Outputs: SHA256SUMS in that directory.
# Exit codes: 0 on success. 1 on a missing directory, a missing package type, or a hash failure.
# Side effects: creates or replaces SHA256SUMS. On failure, an old SHA256SUMS stays unchanged.
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

# NUL separators keep a file name with a newline as one name.
mapfile -d '' -t names < <(printf '%s\0' "${debs[@]}" "${rpms[@]}" "${pkgs[@]}" "${tars[@]}" | LC_ALL=C sort -z)
if [[ ${#names[@]} -lt 4 ]]; then
  echo "make-checksums: fewer than 4 files" >&2
  exit 1
fi

# Write to a temp file first. A failed hash must not leave a partial SHA256SUMS.
partial="$(mktemp -p "$directory" .SHA256SUMS.XXXXXX)"
trap 'rm -f -- "$partial"' EXIT
sha256sum -- "${names[@]}" > "$partial"
chmod 644 -- "$partial"
mv -f -- "$partial" SHA256SUMS
