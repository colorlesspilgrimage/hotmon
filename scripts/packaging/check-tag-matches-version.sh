#!/usr/bin/env bash
# Purpose: Fail when a git tag does not match the CMake version.
# Arguments: <tag>
# Outputs: none on success. An error line on failure.
# Side effects: none.
set -euo pipefail

tag="${1:-}"

# In UTF-8 locales, bash [0-9] can match non-ASCII digits. Use the ASCII digit list.
if [[ ! "$tag" =~ ^v[0123456789]+\.[0123456789]+\.[0123456789]+$ ]]; then
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
