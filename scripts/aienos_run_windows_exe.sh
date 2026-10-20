#!/usr/bin/env bash
# aienos_run_windows_exe.sh -- cross-build aienos-run.exe (crates/aienos-run) for 64-bit
# Windows into dist/aienos-run-windows-x86_64.exe and print its size and sha256 (the values
# the manifest's `runner windows-x86_64` line carries).
#
# Builds with the GNU target (x86_64-pc-windows-gnu; needs the rustup target and a MinGW
# linker, e.g. `brew install mingw-w64`). The builder's home folder is remapped to /build so
# the exe carries no local paths (panic locations otherwise name the build machine's user);
# the build fails if any remain. The linker writes no time stamp, so the same source and
# toolchain give the same bytes. Only Windows system DLLs may be imported: the exe must run
# from a download folder with nothing installed. The hypervisor DLLs are loaded at run time.
# Last line:
#     AIENOS_RUN_WINDOWS_EXE: PASS <sha256> <bytes>   exit 0, or FAIL (...) exit 1
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag="AIENOS_RUN_WINDOWS_EXE"
target="x86_64-pc-windows-gnu"
out="${repo_root}/dist/aienos-run-windows-x86_64.exe"
fail() { echo "${tag}: FAIL ($*)"; exit 1; }

objdump=""
for o in x86_64-w64-mingw32-objdump llvm-objdump; do
    command -v "${o}" >/dev/null && { objdump="${o}"; break; }
done
[[ -n "${objdump}" ]] || fail "no PE objdump (x86_64-w64-mingw32-objdump or llvm-objdump)"

export RUSTFLAGS="--remap-path-prefix=${HOME}=/build -C link-arg=-Wl,--no-insert-timestamp"
cargo build --release -p aienos-run --target "${target}" --manifest-path "${repo_root}/Cargo.toml" \
    || fail "cargo build"
mkdir -p "${repo_root}/dist"
cp "${repo_root}/target/${target}/release/aienos-run.exe" "${out}"

if LC_ALL=C grep -q -F -- "${HOME}" "${out}"; then
    fail "the exe still contains ${HOME}"
fi
system_dll='^(kernel32|ntdll|userenv|ws2_32|bcryptprimitives|advapi32|api-ms-win-[a-z0-9-]+)\.dll$'
while read -r dll; do
    echo "${dll}" | tr 'A-Z' 'a-z' | grep -Eq "${system_dll}" || fail "imports ${dll}"
done < <("${objdump}" -p "${out}" | awk '/DLL Name:/ {print $3}')

bytes=$(wc -c < "${out}" | tr -d ' ')
if command -v sha256sum >/dev/null; then
    sum=$(sha256sum "${out}" | cut -d' ' -f1)
else
    sum=$(shasum -a 256 "${out}" | cut -d' ' -f1)
fi
echo "runner windows-x86_64 aienos-run-windows-x86_64.exe ${bytes} ${sum} aienos-run-windows-x86_64.exe"
echo "${tag}: PASS ${sum} ${bytes}"
