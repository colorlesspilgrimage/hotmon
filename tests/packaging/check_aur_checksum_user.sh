#!/usr/bin/env bash
# Purpose: Check that the AUR checksum step does not run makepkg as root.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.AurChecksumUser or FAIL: <reason>
# Side effects: uses temp directories. Does not use the network.
# Security: CWE-250. makepkg refuses root. The release job must use builder.
set -euo pipefail

source_dir="${1:-}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

if [[ ! -d "$source_dir" ]]; then
  fail "missing source dir"
fi

release="${source_dir}/.github/workflows/release.yml"
script="${source_dir}/scripts/packaging/update-aur-checksum.sh"
if [[ ! -f "$release" || ! -f "$script" ]]; then
  fail "missing release workflow or checksum script"
fi
if ! grep -q 'scripts/packaging/update-aur-checksum.sh' "$release"; then
  fail "release workflow does not call update-aur-checksum.sh"
fi
if grep -q 'updpkgsums' "$release"; then
  fail "release workflow calls updpkgsums directly"
fi
if ! grep -q 'runuser -u builder -- updpkgsums' "$script"; then
  fail "checksum script does not run updpkgsums as builder"
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
stub="${work}/bin"
mkdir -p -- "$stub"
log="${work}/log"
: > "$log"

cat > "${stub}/id" <<'EOF'
#!/bin/bash
if [[ "${1:-}" == "-u" ]]; then
  echo 0
  exit 0
fi
exit 1
EOF
cat > "${stub}/useradd" <<'EOF'
#!/bin/bash
echo "useradd $*" >> "$HOTMON_STUB_LOG"
exit 0
EOF
cat > "${stub}/chown" <<'EOF'
#!/bin/bash
echo "chown $*" >> "$HOTMON_STUB_LOG"
exit 0
EOF
cat > "${stub}/runuser" <<'EOF'
#!/bin/bash
echo "runuser $*" >> "$HOTMON_STUB_LOG"
if [[ "$1" != "-u" || "$2" != "builder" || "$3" != "--" ]]; then
  echo "bad-runuser" >> "$HOTMON_STUB_LOG"
  exit 1
fi
shift 3
export HOTMON_UPDPKG_AS_BUILDER=1
exec "$@"
EOF
cat > "${stub}/updpkgsums" <<'EOF'
#!/bin/bash
if [[ "${HOTMON_UPDPKG_AS_BUILDER:-}" != 1 ]]; then
  echo "ran-as-root" >> "$HOTMON_STUB_LOG"
  exit 1
fi
echo "ran-as-builder" >> "$HOTMON_STUB_LOG"
sed -i -- "s/sha256sums=('SKIP')/sha256sums=('abc')/" "$1"
EOF
chmod +x "${stub}/id" "${stub}/useradd" "${stub}/chown" "${stub}/runuser" "${stub}/updpkgsums"

pkg="${work}/PKGBUILD"
cp -- "${source_dir}/packaging/aur/PKGBUILD" "$pkg"
before="$(cat -- "$pkg")"

status=0
PATH="${stub}:${PATH}" HOTMON_STUB_LOG="$log" "$script" '1.0; touch X' "$pkg" || status=$?
if [[ "$status" -ne 2 ]]; then
  fail "bad version exited ${status}"
fi
if [[ -e "${work}/X" || "$(cat -- "$pkg")" != "$before" ]]; then
  fail "bad version changed a file"
fi
if [[ -s "$log" ]]; then
  fail "bad version called a stub"
fi

PATH="${stub}:${PATH}" HOTMON_STUB_LOG="$log" "$script" "9.8.7" "$pkg"
if ! grep -q '^pkgver=9.8.7$' "$pkg"; then
  fail "pkgver was not set"
fi
if ! grep -q "sha256sums=('abc')" "$pkg"; then
  fail "checksum was not written"
fi
if ! grep -q 'ran-as-builder' "$log"; then
  fail "updpkgsums did not run as builder"
fi
if grep -q 'ran-as-root' "$log"; then
  fail "updpkgsums ran as root"
fi
if ! grep -q 'useradd -m builder' "$log"; then
  fail "builder user was not created"
fi

plain="${work}/bin-plain"
mkdir -p -- "$plain"
cat > "${plain}/updpkgsums" <<'EOF'
#!/bin/bash
echo "ran-direct" >> "$HOTMON_STUB_LOG"
sed -i -- "s/sha256sums=('abc')/sha256sums=('def')/" "$1"
EOF
chmod +x "${plain}/updpkgsums"
: > "$log"
PATH="${plain}:${PATH}" HOTMON_STUB_LOG="$log" "$script" "1.2.3" "$pkg"
if ! grep -q '^pkgver=1.2.3$' "$pkg"; then
  fail "non-root pkgver was not set"
fi
if ! grep -q "sha256sums=('def')" "$pkg"; then
  fail "non-root checksum was not written"
fi
if ! grep -q 'ran-direct' "$log"; then
  fail "non-root path did not call updpkgsums"
fi
if grep -q 'runuser' "$log"; then
  fail "non-root path called runuser"
fi

echo "PASS: Packaging.AurChecksumUser"
