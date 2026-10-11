#!/usr/bin/env bash
# Purpose: Print the hotmon version from CMakeLists.txt.
# Arguments: none.
# Outputs: one version line on stdout.
# Side effects: none.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
cmake_file="${repo_root}/CMakeLists.txt"
version=""

if [[ -f "$cmake_file" ]]; then
  while IFS= read -r line; do
    if [[ "$line" =~ ^project\(hotmon\ VERSION\ ([0-9]+\.[0-9]+\.[0-9]+)([^0-9]|$) ]]; then
      version="${BASH_REMATCH[1]}"
      break
    fi
  done < "$cmake_file"
fi

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "read-version: version not found" >&2
  exit 1
fi

printf '%s\n' "$version"
