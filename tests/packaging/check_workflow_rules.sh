#!/usr/bin/env bash
# Purpose: Check GitHub workflow files for injection and privilege rules.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.WorkflowRules or FAIL: <reason>
# Side effects: none. Runs gitleaks only when that command exists.
# Security: CWE-94, OWASP A03, CWE-250, ASVS V1.2, CWE-532.
set -euo pipefail

source_dir="${1:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$source_dir" ]]; then
  fail "missing source dir"
fi

workflow_dir="${source_dir}/.github/workflows"
shopt -s nullglob
files=( "${workflow_dir}/"*.yml )
shopt -u nullglob
if [[ ${#files[@]} -eq 0 ]]; then
  fail "no workflow files"
fi

reject_context() {
  local text="$1"
  local file="$2"
  if [[ "$text" == *"$ref_mark"* || "$text" == *"$event_mark"* ]]; then
    fail "${file} puts GitHub context inside a run block"
  fi
}

scan_file() {
  local file="$1"
  local in_run=0
  local run_indent=0
  local line=""
  local indent=0
  local body=""
  if ! grep -q '^permissions:' "$file"; then
    fail "${file} has no top-level permissions key"
  fi
  if grep -q 'pull_request_target' "$file"; then
    fail "${file} uses pull_request_target"
  fi
  if grep -q 'set -x' "$file"; then
    fail "${file} uses set -x"
  fi
  open='$'"{{"
  ref_mark="${open} github.ref_name }}"
  event_mark="${open} github.event"
  secret_name='AUR_SSH_PRIVATE_KEY'
  # The loop prints the path. It does not write the workflow file.
  # shellcheck disable=SC2094
  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$in_run" -eq 1 ]]; then
      if [[ "$line" =~ ^[[:space:]]*$ ]]; then
        continue
      fi
      indent="${line%%[^[:space:]]*}"
      indent="${#indent}"
      if [[ "$indent" -le "$run_indent" ]]; then
        in_run=0
      else
        reject_context "$line" "$file"
        if [[ "$line" == *'$'"${secret_name}"* && "$line" == *echo* ]]; then
          fail "${file} prints a secret"
        fi
        continue
      fi
    fi
    if [[ "$line" =~ ^([[:space:]]*)run:[[:space:]]*(\||>) ]]; then
      in_run=1
      run_indent="${#BASH_REMATCH[1]}"
      continue
    fi
    if [[ "$line" =~ ^([[:space:]]*)run:[[:space:]]*(.*)$ ]]; then
      body="${BASH_REMATCH[2]}"
      reject_context "$body" "$file"
    fi
  done < "$file"
}

for file in "${files[@]}"; do
  scan_file "$file"
done

if command -v gitleaks >/dev/null 2>&1; then
  gitleaks detect --no-git --source "${source_dir}/scripts/packaging"
  gitleaks detect --no-git --source "${source_dir}/.github"
  gitleaks detect --no-git --source "${source_dir}/packaging"
fi

echo "PASS: Packaging.WorkflowRules"
