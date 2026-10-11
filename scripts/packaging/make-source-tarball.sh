#!/usr/bin/env bash
# Purpose: Write a git archive source tarball for one version.
# Arguments: <version> <output-dir>
# Outputs: the tarball path on stdout.
# Side effects: creates the output directory and the tarball.
set -euo pipefail

version="${1:-}"
output_dir="${2:-}"

# In UTF-8 locales, bash [0-9] can match non-ASCII digits. Use the ASCII digit list.
if [[ ! "$version" =~ ^[0123456789]+\.[0123456789]+\.[0123456789]+$ ]]; then
  echo "make-source-tarball: bad version" >&2
  exit 2
fi

if [[ -z "$output_dir" ]]; then
  echo "make-source-tarball: output dir missing" >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"

mkdir -p -- "$output_dir"
output_dir="$(realpath -m -- "$output_dir")"
tarball="${output_dir}/hotmon-${version}.tar.gz"

git -C "$repo_root" archive --format=tar.gz --prefix="hotmon-${version}/" -o "$tarball" HEAD
printf '%s\n' "$tarball"
