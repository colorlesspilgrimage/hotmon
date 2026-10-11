#!/usr/bin/env bash
# Purpose: Set pkgver and fill sha256sums in the AUR PKGBUILD.
# Arguments: <version> [pkgbuild-path]
# Outputs: none.
# Side effects: writes the PKGBUILD. As root, creates user builder.
# makepkg exits when EUID is 0, before it writes checksums. Run it as builder.
set -euo pipefail

version="${1:-}"
pkgbuild="${2:-}"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "update-aur-checksum: bad version" >&2
  exit 2
fi

if [[ -z "$pkgbuild" ]]; then
  script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  repo_root="$(cd -- "${script_dir}/../.." && pwd)"
  pkgbuild="${repo_root}/packaging/aur/PKGBUILD"
fi

if [[ ! -f "$pkgbuild" ]]; then
  echo "update-aur-checksum: PKGBUILD not found" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cp -- "$pkgbuild" "${work}/PKGBUILD"
sed -i -- "s/^pkgver=.*/pkgver=${version}/" "${work}/PKGBUILD"

if [[ "$(id -u)" -eq 0 ]]; then
  if ! id builder >/dev/null 2>&1; then
    useradd -m builder
  fi
  chown -R -- builder:builder "$work"
  runuser -u builder -- updpkgsums "${work}/PKGBUILD"
else
  updpkgsums "${work}/PKGBUILD"
fi

cp -- "${work}/PKGBUILD" "$pkgbuild"
