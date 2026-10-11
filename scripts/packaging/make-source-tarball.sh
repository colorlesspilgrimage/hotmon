#!/usr/bin/env bash
# Purpose: Write a git archive source tarball for one version.
# Arguments: <version> <output-dir>
# Outputs: the tarball path on stdout.
# Side effects: creates the output directory and the tarball.
set -euo pipefail

version="${1:-}"
output_dir="${2:-}"

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source -- "${script_dir}/common.sh"

require_version make-source-tarball "$version"
require_output_dir make-source-tarball "$output_dir"

repo_root="$(packaging_repo_root)"
output_dir="$(resolve_output_dir "$output_dir")"
tarball="${output_dir}/hotmon-${version}.tar.gz"

git -C "$repo_root" archive --format=tar.gz --prefix="hotmon-${version}/" -o "$tarball" HEAD
printf '%s\n' "$tarball"
