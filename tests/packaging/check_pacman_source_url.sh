#!/usr/bin/env bash
# Purpose: Check that build-pacman.sh gives makepkg a valid file URL for any tarball path.
# Arguments: <source-dir>
# Outputs: PASS: Packaging.PacmanSourceUrl or FAIL: <reason>
# Side effects: uses temp directories with stub id and makepkg. Does not use the network.
# Security: CWE-116. The tarball path must be URL-encoded, not passed as raw text.
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
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
# The script prints resolved paths. Resolve the temp dir too, in case TMPDIR is a symlink.
work="$(realpath -- "$work")"

stub="${work}/bin"
mkdir -p -- "$stub"
# The stub id reports a normal user, so the script never creates user builder.
cat > "${stub}/id" <<'EOF'
#!/usr/bin/env bash
echo 1000
EOF
# The stub makepkg records the source URL and writes an empty package file.
cat > "${stub}/makepkg" <<'EOF'
#!/usr/bin/env bash
printf '%s' "$HOTMON_SOURCE_URL" > "$HOTMON_STUB_URL_FILE"
: > "${PKGDEST}/hotmon-1.2.3-1-x86_64.pkg.tar.zst"
EOF
chmod +x -- "${stub}/id" "${stub}/makepkg"

tarball_dir="${work}/a b#c%d?e é"
mkdir -p -- "$tarball_dir"
tarball="${tarball_dir}/hotmon-1.2.3.tar.gz"
: > "$tarball"
url_file="${work}/url"

output="$(PATH="${stub}:${PATH}" HOTMON_STUB_URL_FILE="$url_file" "$script" 1.2.3 "$tarball" "${work}/out")"
if [[ "$output" != "${work}/out/hotmon-1.2.3-1-x86_64.pkg.tar.zst" ]]; then
  fail "unexpected output: ${output}"
fi

url="$(cat -- "$url_file")"
if [[ "$url" != "file://"* ]]; then
  fail "URL does not start with file://: ${url}"
fi
if [[ "$url" == *" "* || "$url" == *"#"* || "$url" == *"?"* ]]; then
  fail "URL holds a raw space, '#' or '?': ${url}"
fi
if [[ "$url" != *"/a%20b%23c%25d%3Fe%20%C3%A9/hotmon-1.2.3.tar.gz" ]]; then
  fail "URL is not percent-encoded: ${url}"
fi

encoded_path="${url#file://}"
printf -v decoded_path '%b' "${encoded_path//%/\\x}"
if [[ "$decoded_path" != "$tarball" ]]; then
  fail "decoded URL path ${decoded_path} is not ${tarball}"
fi

echo "PASS: Packaging.PacmanSourceUrl"
