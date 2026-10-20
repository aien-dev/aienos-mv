#!/usr/bin/env bash
# aienos_run_qemu_bundle.sh -- build the QEMU runtime that aienos-run.exe (crates/aienos-run,
# candidate A) downloads on first run: qemu-system-x86_64.exe, qemu-img.exe, the DLLs they
# load and the x86 UEFI firmware, as one zip that Windows' own tar.exe unpacks.
#
# The parts are the MSYS2 ucrt64 builds (https://repo.msys2.org/mingw/ucrt64/). The lock file
# crates/aienos-run/qemu-bundle.lock pins every package by file name and sha256 and lists the
# exact members taken from it, so a normal run needs no package database and takes nothing
# that is not pinned:
#     scripts/aienos_run_qemu_bundle.sh OUTDIR            -> OUTDIR/aienos-qemu-win64.zip
#     scripts/aienos_run_qemu_bundle.sh OUTDIR --refresh  -> re-resolve from the live MSYS2
#                                                            database and rewrite the lock
# Refresh starts from qemu-system-x86_64.exe and qemu-img.exe and follows the PE import
# tables (objdump -p): every imported DLL that some ucrt64 package ships in bin/ is added
# with its package; a DLL no package ships is a Windows system DLL and is recorded as
# "sysdll" in the lock. A normal run repeats the import walk over the unpacked bundle and
# fails if any import is neither in the bundle nor a recorded system DLL.
# Licences: QEMU is GPL-2.0-or-later; every package's licence is copied into licenses/ and
# the source for each pinned package is at https://github.com/msys2/MINGW-packages.
# Needs curl, tar with zstd, zip, sha256sum or shasum, and a PE objdump
# (x86_64-w64-mingw32-objdump, or llvm-objdump). Last line:
#     AIENOS_RUN_QEMU_BUNDLE: PASS <sha256> <bytes>   exit 0, or FAIL (...) exit 1
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag="AIENOS_RUN_QEMU_BUNDLE"
lock="${repo_root}/crates/aienos-run/qemu-bundle.lock"
mirror="${AIENOS_MSYS2_MIRROR:-https://repo.msys2.org/mingw/ucrt64}"
out="${1:?usage: $0 OUTDIR [--refresh]}"
mode="${2:-}"
fail() { echo "${tag}: FAIL ($*)"; exit 1; }

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
objdump_bin=""
for t in x86_64-w64-mingw32-objdump llvm-objdump /opt/homebrew/opt/llvm/bin/llvm-objdump; do
    if command -v "${t}" >/dev/null; then objdump_bin="${t}"; break; fi
done
for t in curl tar zip; do command -v "${t}" >/dev/null || fail "${t} missing"; done
[[ -n "${objdump_bin}" ]] || fail "no PE objdump"
imports() { "${objdump_bin}" -p "$1" | sed -nE 's/^[[:space:]]*DLL Name: (.*)$/\1/p' | tr -d '\r'; }
lower() { tr '[:upper:]' '[:lower:]'; }

mkdir -p "${out}"
out="$(cd "${out}" && pwd)"
work="${out}/work"
rm -rf "${work}"
mkdir -p "${work}/pkgs" "${work}/root"

fetch() { # package file, expected sha256
    local f="${work}/pkgs/$1"
    [[ -s "${f}" ]] || curl -sfL --retry 3 -o "${f}" "${mirror}/$1" || fail "download $1"
    [[ "$(sha "${f}")" == "$2" ]] || fail "sha256 mismatch for $1"
}

# Files always taken from the qemu packages (firmware and option ROMs the q35 machine with
# virtio-net-pci reads; keymaps for the optional SDL window).
want_share='^ucrt64/share/qemu/(edk2-x86_64-code\.fd|edk2-i386-vars\.fd|edk2-licenses\.txt|efi-virtio\.rom|efi-e1000e\.rom|kvmvapic\.bin|linuxboot_dma\.bin|multiboot_dma\.bin|pvh\.bin|bios-256k\.bin|vgabios-(stdvga|virtio|ramfb|bochs-display)\.bin|keymaps/[^/]+)$'

if [[ "${mode}" == "--refresh" ]]; then
    curl -sfL -o "${work}/ucrt64.db" "${mirror}/ucrt64.db" || fail "download ucrt64.db"
    curl -sfL -o "${work}/ucrt64.files" "${mirror}/ucrt64.files" || fail "download ucrt64.files"
    mkdir -p "${work}/db"
    tar -xf "${work}/ucrt64.db" -C "${work}/db"
    tar -xf "${work}/ucrt64.files" -C "${work}/db"
    # pkgdir -> file name and sha256, from desc
    descval() { awk -v k="%$2%" '$0==k{getline; print; exit}' "${work}/db/$1/desc"; }
    # dll (lower case) -> pkgdir, from every package's files list
    : >"${work}/dllmap"
    for d in "${work}/db"/*/; do
        p="$(basename "${d}")"
        [[ -f "${d}files" ]] || continue
        sed -nE 's#^ucrt64/bin/([^/]+\.dll)$#\1#p' "${d}files" | lower | sed "s#\$# ${p}#" >>"${work}/dllmap"
    done
    qpkg="$(cd "${work}/db" && ls -d mingw-w64-ucrt-x86_64-qemu-[0-9]* | head -1)"
    ipkg="$(cd "${work}/db" && ls -d mingw-w64-ucrt-x86_64-qemu-image-util-[0-9]* | head -1)"
    [[ -n "${qpkg}" && -n "${ipkg}" ]] || fail "qemu packages not in the database"
    : >"${work}/lock.new"
    declare -A taken=()
    add_pkg() { # pkgdir
        [[ -n "${taken[$1]:-}" ]] && return 0
        taken[$1]=1
        local f s
        f="$(descval "$1" FILENAME)"
        s="$(descval "$1" SHA256SUM)"
        echo "pkg ${f} ${s}" >>"${work}/lock.new"
        fetch "${f}" "${s}"
        mkdir -p "${work}/x/$1"
        tar -xf "${work}/pkgs/${f}" -C "${work}/x/$1" 2>/dev/null || fail "unpack ${f}"
        # licences of every package
        if [[ -d "${work}/x/$1/ucrt64/share/licenses" ]]; then
            (cd "${work}/x/$1" && find ucrt64/share/licenses -type f) | sed "s#^#file ${f} #" >>"${work}/lock.new"
        fi
    }
    add_member() { # pkgdir member
        local f
        f="$(descval "$1" FILENAME)"
        echo "file ${f} $2" >>"${work}/lock.new"
        mkdir -p "${work}/root/$(dirname "$2")"
        cp "${work}/x/$1/$2" "${work}/root/$2"
    }
    add_pkg "${qpkg}"
    add_pkg "${ipkg}"
    add_member "${qpkg}" ucrt64/bin/qemu-system-x86_64.exe
    add_member "${ipkg}" ucrt64/bin/qemu-img.exe
    (cd "${work}/x/${qpkg}" && find ucrt64/share/qemu -type f) | grep -E "${want_share}" | sort | while read -r m; do
        add_member "${qpkg}" "${m}"
    done
    queue=(ucrt64/bin/qemu-system-x86_64.exe ucrt64/bin/qemu-img.exe)
    declare -A seen=()
    while [[ ${#queue[@]} -gt 0 ]]; do
        bin="${queue[0]}"
        queue=("${queue[@]:1}")
        while read -r dll; do
            l="$(lower <<<"${dll}")"
            [[ -n "${seen[$l]:-}" ]] && continue
            seen[$l]=1
            p="$(awk -v d="${l}" '$1==d{print $2; exit}' "${work}/dllmap")"
            if [[ -z "${p}" ]]; then
                echo "sysdll ${l}" >>"${work}/lock.new"
                continue
            fi
            add_pkg "${p}"
            m="$(cd "${work}/x/${p}" && find ucrt64/bin -maxdepth 1 -type f -iname "${l}" | head -1)"
            [[ -n "${m}" ]] || fail "${l} listed for ${p} but not unpacked"
            add_member "${p}" "${m}"
            queue+=("${m}")
        done < <(imports "${work}/root/${bin}")
    done
    {
        echo "# aienos-run QEMU bundle lock, written by scripts/aienos_run_qemu_bundle.sh --refresh"
        echo "# mirror ${mirror}"
        echo "# pkg <file> <sha256> | file <pkg file> <member> | sysdll <name>"
        grep '^pkg ' "${work}/lock.new" | sort -u
        grep '^file ' "${work}/lock.new" | sort -u
        grep '^sysdll ' "${work}/lock.new" | sort -u
    } >"${lock}"
    rm -rf "${work}/root"
    mkdir -p "${work}/root"
fi

[[ -f "${lock}" ]] || fail "no lock file ${lock}; run with --refresh"
# Normal build from the lock alone.
while read -r kind a b; do
    [[ "${kind}" == "pkg" ]] || continue
    fetch "${a}" "${b}"
    mkdir -p "${work}/u/${a}"
    members="$(awk -v f="${a}" '$1=="file" && $2==f {print $3}' "${lock}")"
    [[ -n "${members}" ]] || continue
    # shellcheck disable=SC2086
    tar -xf "${work}/pkgs/${a}" -C "${work}/u/${a}" ${members} 2>/dev/null || fail "unpack members of ${a}"
    while read -r m; do
        case "${m}" in
            ucrt64/bin/*) dst="${work}/root/aienos-qemu/$(basename "${m}")" ;;
            ucrt64/share/qemu/*) dst="${work}/root/aienos-qemu/share/${m#ucrt64/share/qemu/}" ;;
            ucrt64/share/licenses/*) dst="${work}/root/aienos-qemu/licenses/${m#ucrt64/share/licenses/}" ;;
            *) fail "unexpected member ${m}" ;;
        esac
        mkdir -p "$(dirname "${dst}")"
        cp "${work}/u/${a}/${m}" "${dst}"
    done <<<"${members}"
done < <(grep -v '^#' "${lock}")

b="${work}/root/aienos-qemu"
[[ -f "${b}/qemu-system-x86_64.exe" && -f "${b}/qemu-img.exe" && -f "${b}/share/edk2-x86_64-code.fd" && -f "${b}/share/edk2-i386-vars.fd" ]] || fail "bundle incomplete"
# Import closure: every DLL any bundled exe or dll imports is in the bundle or a recorded system DLL.
missing=0
for f in "${b}"/*.exe "${b}"/*.dll; do
    while read -r dll; do
        l="$(lower <<<"${dll}")"
        if ls "${b}" | lower | grep -qxF "${l}"; then continue; fi
        if grep -qxF "sysdll ${l}" "${lock}"; then continue; fi
        echo "missing import ${dll} of $(basename "${f}")"
        missing=$((missing + 1))
    done < <(imports "${f}")
done
[[ ${missing} == 0 ]] || fail "${missing} unresolved imports"
{
    echo "aienos-qemu: QEMU for Windows (MSYS2 ucrt64 builds), bundled for aienos-run."
    echo "Built by scripts/aienos_run_qemu_bundle.sh from crates/aienos-run/qemu-bundle.lock (sha256 $(sha "${lock}"))."
    echo "Licences are in licenses/. QEMU is GPL-2.0-or-later; sources: https://github.com/msys2/MINGW-packages"
    grep '^pkg ' "${lock}"
} >"${b}/BUNDLE.txt"
# Deterministic zip: fixed mtimes, sorted entries, no extra attributes.
find "${b}" -exec touch -h -t 202601010000 {} +
zipf="${out}/aienos-qemu-win64.zip"
rm -f "${zipf}"
(cd "${work}/root" && find aienos-qemu -type f | LC_ALL=C sort | zip -q -X -D -9 "${zipf}" -@)
n="$(find "${b}" -type f | wc -l | tr -d ' ')"
echo "files=${n} dlls=$(ls "${b}"/*.dll | wc -l | tr -d ' ')"
echo "${tag}: PASS $(sha "${zipf}") $(wc -c <"${zipf}" | tr -d ' ')"
