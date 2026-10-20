#!/bin/sh
# try.sh -- try AienOS with one command (Linux and macOS):
#
#     curl -fsSL https://www.aienos.com/try | sh
#
# What it does, in order (docs/AIENOS_RUN_LINUX.md):
#   1. downloads the aienos-run manifest (https by default),
#   2. picks the aienos-run program for this computer (linux-aarch64, linux-x86_64,
#      macos-aarch64, ...), downloads it and checks its size and SHA-256 against the
#      manifest; a mismatch stops here and nothing is installed,
#   3. installs it as ~/.local/bin/aienos-run (no root, nothing else on the system changes),
#   4. runs it: aienos-run downloads the AienOS image named by the same manifest, checks
#      its SHA-256 the same way and boots it to the AienOS console. Ctrl+] ends it.
#
# Arguments after `sh -s --` go to aienos-run, e.g.  curl ... | sh -s -- --backend qemu
# Settings (environment): AIENOS_TRY_BASE (default https://www.aienos.com/try; file:// works
# for local tests), AIENOS_TRY_BIN_DIR (default ~/.local/bin), AIENOS_TRY_NO_RUN=1 (install
# only). Exit codes: 0 ok (or aienos-run's own code), 10 unsupported computer, 11 download
# failed, 12 verification failed, 13 install failed.
set -eu

base="${AIENOS_TRY_BASE:-https://www.aienos.com/try}"
manifest_url="${base%/}/manifest.txt"
bin_dir="${AIENOS_TRY_BIN_DIR:-${HOME}/.local/bin}"

say() { printf 'aienos-try: %s\n' "$*" >&2; }
die() { code=$1; shift; say "$*"; exit "$code"; }

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) die 10 "this computer runs $(uname -s); try.sh supports Linux and macOS (Windows: aienos-run.exe, see aienos.com/try)" ;;
esac
case "$(uname -m)" in
    aarch64 | arm64) arch=aarch64 ;;
    x86_64 | amd64) arch=x86_64 ;;
    *) die 10 "unsupported processor $(uname -m)" ;;
esac
platform="${os}-${arch}"

command -v curl >/dev/null 2>&1 || die 11 "curl is needed"
if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    die 12 "no sha256sum or shasum to verify the download"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/aienos-try.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

say "manifest: ${manifest_url}"
curl -fsSL --proto '=https,file' -o "$work/manifest.txt" "$manifest_url" || die 11 "cannot download the manifest"
[ "$(head -n 1 "$work/manifest.txt")" = "aienos-run-manifest 1" ] || die 12 "the manifest is not an aienos-run manifest"

# runner <platform> <file-name> <size> <sha256> <url>
line="$(awk -v p="$platform" '$1 == "runner" && $2 == p { print; exit }' "$work/manifest.txt")"
[ -n "$line" ] || die 10 "no aienos-run for ${platform} in the manifest yet"
name="$(echo "$line" | awk '{print $3}')"
size="$(echo "$line" | awk '{print $4}')"
sum="$(echo "$line" | awk '{print $5}')"
url="$(echo "$line" | awk '{print $6}')"
case "$name" in */* | .* | "") die 12 "bad runner file name in the manifest" ;; esac
case "$url" in *://*) ;; /*) url="file://${url}" ;; *) url="${manifest_url%/*}/${url}" ;; esac

say "downloading ${name} for ${platform}"
curl -fsSL --proto '=https,file' -o "$work/$name" "$url" || die 11 "cannot download ${url}"
got_size="$(wc -c <"$work/$name" | tr -d ' ')"
[ "$got_size" = "$size" ] || die 12 "size mismatch for ${name}: manifest ${size}, got ${got_size}; not installed"
got_sum="$(sha256 "$work/$name")"
[ "$got_sum" = "$sum" ] || die 12 "sha256 mismatch for ${name}: manifest ${sum}, got ${got_sum}; not installed"
say "verified sha256 ${got_sum}"

mkdir -p "$bin_dir" || die 13 "cannot create ${bin_dir}"
chmod 0755 "$work/$name"
mv -f "$work/$name" "$bin_dir/aienos-run" || die 13 "cannot install into ${bin_dir}"
say "installed ${bin_dir}/aienos-run"
case ":${PATH}:" in *":${bin_dir}:"*) ;; *) say "(add ${bin_dir} to your PATH to run aienos-run later)" ;; esac

if [ "${AIENOS_TRY_NO_RUN:-0}" = 1 ]; then
    exit 0
fi
# The script itself arrives on stdin (curl | sh); give aienos-run the terminal instead.
if [ -t 1 ] && [ -r /dev/tty ]; then
    exec "$bin_dir/aienos-run" --manifest "$manifest_url" "$@" </dev/tty
fi
exec "$bin_dir/aienos-run" --manifest "$manifest_url" "$@"
