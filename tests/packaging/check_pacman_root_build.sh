#!/usr/bin/env bash
# Purpose: Check that build-pacman.sh as root does not give its output dir to builder.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.PacmanRootBuild or FAIL: <reason>
# Side effects: uses temp directories. Stubs id, useradd, chown, runuser, makepkg.
# Security: CWE-732, CWE-269. Root must keep ownership of the caller output dir.
set -euo pipefail

source_dir="${1:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$source_dir" ]]; then
  fail "missing source dir"
fi

script="${source_dir}/scripts/packaging/build-pacman.sh"
if [[ ! -f "$script" ]]; then
  fail "missing build-pacman.sh"
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
stub="${work}/bin"
mkdir -p -- "$stub"
log="${work}/log"
: > "$log"

# The id stub reports root until the runuser stub marks the builder process.
cat > "${stub}/id" <<'EOF'
#!/bin/bash
if [[ "${1:-}" == "-u" ]]; then
  if [[ "${HOTMON_STUB_AS_BUILDER:-}" == 1 ]]; then
    echo 1000
  else
    echo 0
  fi
  exit 0
fi
exit 1
EOF
cat > "${stub}/useradd" <<'EOF'
#!/bin/bash
echo "useradd $*" >> "$HOTMON_STUB_LOG"
EOF
cat > "${stub}/chown" <<'EOF'
#!/bin/bash
echo "chown $*" >> "$HOTMON_STUB_LOG"
EOF
cat > "${stub}/runuser" <<'EOF'
#!/bin/bash
echo "runuser $*" >> "$HOTMON_STUB_LOG"
if [[ "$1" != "-u" || "$2" != "builder" || "$3" != "--" ]]; then
  exit 1
fi
shift 3
export HOTMON_STUB_AS_BUILDER=1
exec "$@"
EOF
cat > "${stub}/makepkg" <<'EOF'
#!/bin/bash
if [[ "${HOTMON_STUB_AS_BUILDER:-}" != 1 ]]; then
  echo "makepkg-as-root" >> "$HOTMON_STUB_LOG"
  exit 1
fi
echo "makepkg PKGDEST=${PKGDEST}" >> "$HOTMON_STUB_LOG"
: > "${PKGDEST}/hotmon-1.2.3-1-x86_64.pkg.tar.zst"
EOF
chmod +x "${stub}/id" "${stub}/useradd" "${stub}/chown" "${stub}/runuser" "${stub}/makepkg"

tarball="${work}/hotmon-1.2.3.tar.gz"
: > "$tarball"
output_dir="${work}/out"

PATH="${stub}:${PATH}" HOTMON_STUB_LOG="$log" "$script" "1.2.3" "$tarball" "$output_dir" > "${work}/stdout"

if grep -q 'makepkg-as-root' "$log"; then
  fail "makepkg ran as root"
fi
if ! grep -q '^makepkg PKGDEST=' "$log"; then
  fail "makepkg did not run as builder"
fi
if grep -q "^makepkg PKGDEST=${output_dir}\$" "$log"; then
  fail "builder wrote into the caller output dir"
fi
while IFS= read -r line; do
  if [[ "$line" == chown* && "$line" == *"$output_dir"* ]]; then
    fail "root gave the output dir to builder: ${line}"
  fi
done < "$log"
if [[ ! -f "${output_dir}/hotmon-1.2.3-1-x86_64.pkg.tar.zst" ]]; then
  fail "package was not copied to the output dir"
fi
if [[ "$(cat -- "${work}/stdout")" != "${output_dir}/hotmon-1.2.3-1-x86_64.pkg.tar.zst" ]]; then
  fail "stdout is not the package path"
fi

echo "PASS: Packaging.PacmanRootBuild"
