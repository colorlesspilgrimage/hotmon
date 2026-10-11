#!/usr/bin/env bash
# Purpose: Fail when a git tag does not match the CMake version.
# Arguments: <tag>
# Outputs: none on success. An error line on failure.
# Side effects: none.
set -euo pipefail

tag="${1:-}"

if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "check-tag: bad tag" >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
version="$("${script_dir}/read-version.sh")"
tag_version="${tag#v}"

if [[ "$tag_version" != "$version" ]]; then
  echo "check-tag: tag ${tag} does not match version ${version}" >&2
  exit 1
fi
