#!/usr/bin/env bash
# Purpose: Shared input checks and paths for the packaging scripts.
# Use: source this file. Loading it runs no command. It defines functions only.
# Exit codes: the require_* functions exit 2 for bad input. require_tarball exits 3.
# Side effects: none. resolve_output_dir creates the output directory.
# shellcheck shell=bash

# Return 0 when the version is x.y.z with ASCII digits only. Return 1 otherwise.
# Bash [0-9] can match non-ASCII digits in UTF-8 locales. The digit list avoids that.
version_is_valid() {
  local version="$1"
  [[ "$version" =~ ^[0123456789]+\.[0123456789]+\.[0123456789]+$ ]]
}

# Arguments: <tool-name> <version>
require_version() {
  local tool="$1"
  local version="$2"
  if ! version_is_valid "$version"; then
    echo "${tool}: bad version" >&2
    exit 2
  fi
}

# Arguments: <tool-name> <output-dir>
require_output_dir() {
  local tool="$1"
  local output_dir="$2"
  if [[ -z "$output_dir" ]]; then
    echo "${tool}: output dir missing" >&2
    exit 2
  fi
}

# Arguments: <tool-name> <tarball>
require_tarball() {
  local tool="$1"
  local tarball="$2"
  if [[ ! -f "$tarball" ]]; then
    echo "${tool}: tarball not found" >&2
    exit 3
  fi
}

# Arguments: <output-dir>. Creates the directory. Prints its absolute path.
# realpath -m resolves a path whose last part does not exist yet.
resolve_output_dir() {
  local output_dir="$1"
  mkdir -p -- "$output_dir"
  realpath -m -- "$output_dir"
}

# Print the repo root. This file lives in scripts/packaging/, two levels below it.
packaging_repo_root() {
  local lib_dir
  lib_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  (cd -- "${lib_dir}/../.." && pwd)
}

# Print a path percent-encoded for a file:// URL. curl rejects a raw space, '#', '?' or '%'.
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
