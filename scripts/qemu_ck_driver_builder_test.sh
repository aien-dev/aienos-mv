#!/usr/bin/env bash
# Allen's driver builder, end to end in QEMU (L268; device contracts ADR 0021).
# Gate: AIENOS_CK_DRIVER_BUILDER. QEMU is not hardware: a PASS qualifies
# nothing physical.
#
# The fixture device is a virtio entropy device (virtio-rng-pci-non-transitional,
# 1af4:1044, behind the smmuv3 vIOMMU with iommu_platform=on). The AienOS kernel
# has no driver for it.
#
#  boot A   default image: the devices stage prints one `inventory:` line per
#           PCI function; the rng line must say driver=none.
#  plan     allen-driver-builder plan: the rng is on the build path.
#  golden   (pipe proof, no language model) the hand-written golden contract
#           crates/allen-driver-builder/golden/virtio_rng.devc through the same
#           sandbox; image G built from it must bring the device up (boot G).
#  build    allen-driver-builder build with the LOCAL model endpoint
#           (AIENOS_LLM_LOCAL, default http://127.0.0.1:18091): the model writes
#           the device part, the sandbox (check, gen, compile, class selftest,
#           fuzzer, reference model from the golden contract) must pass, and the
#           contract is staged. Every attempt is in attempts.jsonl.
#  consent  the gate records a consent (by "qemu-gate-operator": this run IS the
#           attended enable in QEMU; on a user's machine Allen's window asks).
#  enable   image B carries the staged driver (CK_CONTRACT_RNG, attended).
#  boot B   SMMU on: "virtio_rng: up", inventory driver=vrng_contract with the
#           contract digest, then device reset, bus master revoked, stream to abort.
#  boot B'  no SMMU, same image: DMA denied, the driver never touches the device.
#  undo     one step; the next image is image A again (boot A2: driver=none).
#  cloud    the same build with a cloud endpoint when AIENOS_LLM_CLOUD_CONF names
#           an llm.conf whose key variable is set; otherwise NOT_RUN (stated).
#
# Needs: qemu-system-aarch64, AAVMF, a C compiler, cargo (offline), a model
# endpoint. Final line: AIENOS_CK_DRIVER_BUILDER: PASS|FAIL|NOT_RUN.
# Exit 0 PASS, 1 FAIL, 2 missing tools, 3 NOT_RUN.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
gate=AIENOS_CK_DRIVER_BUILDER
code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "${gate}: NOT_RUN"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "${gate}: NOT_RUN"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${repo_root}/target/native-kernel"
run_dir="${AIENOS_DRIVER_BUILDER_RUN:-${repo_root}/target/driver-builder/run-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "${run_dir}"
echo "run directory (kept as evidence): ${run_dir}"

cargo_target="${CARGO_TARGET_DIR:-${repo_root}/target}"
cargo build --offline -q -p aienos-devc -p allen-driver-builder
devc="${cargo_target}/debug/aienos-devc"
adb="${cargo_target}/debug/allen-driver-builder"
export AIENOS_DEVC="${devc}" AIENOS_REPO="${repo_root}"

make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" full >/dev/null
make -s -C native/kernel OUT="${out}" gpt-image >/dev/null
efi_a="${out}/full/BOOTAA64.EFI"

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
take_gate() {
    if ! gh_quiet_check || ! gh_lock_take "qemu_ck_driver_builder_test" "${AIENOS_GATE_MINUTES:-60}"; then
        echo "NOT_RUN  ${gh_why}"; echo "${gate}: NOT_RUN"; exit 3
    fi
}
top="$(mktemp -d)"
cleanup() { rm -rf "${top}"; gh_lock_release 2>/dev/null || true; }
trap cleanup EXIT

# shellcheck source=scripts/lib_ck_m1_checks.sh
source "${repo_root}/scripts/lib_ck_m1_checks.sh"
image="${top}/nvme.img"
"${out}/host/ck_gpt_image" create "${image}" 512 64 aienos-middle >/dev/null
all_fail=0

# boot <name> <efi> [smmu|nosmmu]
boot() {
    work="${top}/$1"
    mkdir -p "${work}/esp/EFI/BOOT" "${work}/esp/EFI/AIENOS"
    touch "${work}/esp/EFI/AIENOS/BOOTREPORT.TXT"
    cp "$2" "${work}/esp/EFI/BOOT/BOOTAA64.EFI"
    cp "${vars_fd}" "${work}/vars.fd"
    local machine="virt,virtualization=on,gic-version=3"
    [[ "${3:-}" != smmu ]] || machine+=",iommu=smmuv3"
    set +e
    timeout "${AIENOS_QEMU_TIMEOUT:-180}" qemu-system-aarch64 \
        -M "${machine}" -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${work}/esp" \
        -device virtio-blk-pci,drive=esp \
        -drive if=none,id=nvme0,format=raw,file="${image}" \
        -device nvme,drive=nvme0,serial=aienos-drv-test \
        -device virtio-rng-pci-non-transitional,iommu_platform=on \
        -device ramfb -display none \
        -serial file:"${work}/serial.log" -no-reboot
    qemu_status=$?
    set -e
    tr -d '\r' <"${work}/serial.log" >"${work}/serial.txt"
    cp "${work}/serial.txt" "${run_dir}/serial-$1.txt"
    echo "== boot $1: qemu exit ${qemu_status}"
    failed=0
    ck_m1_checks
}
verdict() { [[ "${failed}" == 0 ]] || all_fail=1; failed=0; }
rng_line() { grep -E '^inventory: pci [0-9a-f:.]+ 1af4:1044 ' "${work}/serial.txt" | head -1 || true; }

# ---- boot A: no driver for the rng ----
take_gate
boot A "${efi_a}" smmu
check "inventory printed for every function" "^inventory: end functions=[0-9]*$"
check "the rng is listed with no driver" "^inventory: pci [0-9a-f:.]* 1af4:1044 class=00ff00 .* driver=none contract=none proven=n/a$"
verdict
gh_lock_release
slot="$(rng_line | awk '{print $3}')"
[[ -n "${slot}" ]] || { echo "FAIL  no inventory line for 1af4:1044"; echo "${gate}: FAIL"; exit 1; }
grep '^inventory: ' "${top}/A/serial.txt" >"${run_dir}/inventory-A.txt"

# ---- plan ----
"${adb}" plan --inventory "${run_dir}/inventory-A.txt" --contracts "${repo_root}/native/net/contract" \
    --machine qemu822 | tee "${run_dir}/plan.txt"
if grep -q "^allen-drivers-state: ${slot} build ready ok$" "${run_dir}/plan.txt"; then
    echo "PASS  plan puts the rng on the build path"
else echo "FAIL  plan did not put the rng on the build path"; all_fail=1; fi

# ---- golden pipe proof (no language model) ----
gst="${run_dir}/golden-state"; mkdir -p "${gst}"
golden_dir="${run_dir}/golden-sandbox"
set +e
"${devc}" check crates/allen-driver-builder/golden/virtio_rng.devc >"${run_dir}/golden-check.txt" 2>&1; gck=$?
"${devc}" gen crates/allen-driver-builder/golden/virtio_rng.devc "${golden_dir}/gen" >"${run_dir}/golden-gen.txt" 2>&1; ggen=$?
set -e
echo "golden: devc check exit ${gck}, gen exit ${ggen}"
if [[ "${gck}${ggen}" == 00 ]]; then
    mkdir -p "${gst}/staged/golden"; cp -r "${golden_dir}/gen/driver" "${gst}/staged/golden/"
    out_g="${repo_root}/target/native-kernel-rng-golden"
    make -s -C native/kernel CROSS="${cross}" OUT="${out_g}" AIENOS_COMMIT="${commit}" \
        CK_CONTRACT_RNG="${gst}/staged/golden" CK_CONTRACT_RNG_ATTENDED=1 full >/dev/null
    take_gate
    boot G "${out_g}/full-rng-contract/BOOTAA64.EFI" smmu
    check "golden contract driver brings the rng up" "^virtio_rng: up [0-9a-f:.]* .* bytes=64 distinct=[0-9]* "
    verdict
    gh_lock_release
else echo "FAIL  golden contract does not compile"; all_fail=1; fi

# ---- build with the local model ----
local_label_default="Allen's own model on this computer"
local_url="${AIENOS_LLM_LOCAL:-http://127.0.0.1:18091}"
printf 'kind openai\nbase_url %s\nmodel auto\nlabel %s\n' "${local_url}" \
    "${AIENOS_LLM_LOCAL_LABEL:-${local_label_default}}" >"${run_dir}/llm-local.conf"
st="${run_dir}/state"
set +e
"${adb}" build --inventory "${run_dir}/inventory-A.txt" --slot "${slot}" --llm "${run_dir}/llm-local.conf" \
    --state "${st}" --work "${run_dir}/build-local" --rounds "${AIENOS_BUILD_ROUNDS:-4}" \
    --reference crates/allen-driver-builder/golden/virtio_rng.devc --serial "${top}/A/serial.txt" \
    --machine qemu822 2>&1 | tee "${run_dir}/build-local.txt"
bst=${PIPESTATUS[0]}
set -e
echo "build (local model): exit ${bst}"
if [[ "${bst}" != 0 ]]; then
    echo "FAIL  the local model's contract did not pass the sandbox (attempts: ${run_dir}/build-local/attempts.jsonl)"
    echo "${gate}: FAIL"; exit 1
fi
echo "PASS  local model wrote a contract that passed every sandbox step"

# ---- consent, enable, image B ----
"${adb}" consent --state "${st}" --pci-id 1af4:1044 --by qemu-gate-operator >/dev/null
staged="$("${adb}" enable --state "${st}" --pci-id 1af4:1044 | tail -1)"
digest="$(sha256sum "${staged}/contract.devc" | cut -c1-12)"
out_b="${repo_root}/target/native-kernel-rng-built"
make -s -C native/kernel CROSS="${cross}" OUT="${out_b}" AIENOS_COMMIT="${commit}" \
    CK_CONTRACT_RNG="${staged}" CK_CONTRACT_RNG_ATTENDED=1 full >/dev/null
efi_b="${out_b}/full-rng-contract/BOOTAA64.EFI"
take_gate
boot B "${efi_b}" smmu
check "SMMU window for the built driver's one DMA region" "^smmu_dma_window: virtio_rng only iova=0x[0-9a-f]* len=0x[0-9a-f]* "
check "DMA granted only after confinement" "^dma_gate: virtio_rng granted (Confined), bus master on$"
check "the device comes up with the model-written driver" \
    "^virtio_rng: up [0-9a-f:.]* rid=0x[0-9a-f]* qsize=[0-9]* features=0x[0-9a-f]* access_platform=yes bytes=64 distinct=[0-9]* first8=[0-9a-f]* contract=${digest} (Ok)$"
check "inventory names the built driver and its contract" \
    "^inventory: pci [0-9a-f:.]* 1af4:1044 .* driver=vrng_contract contract=${digest} proven=no$"
check "released: device reset, bus master revoked, stream back to abort" \
    "^smmu: virtio_rng stream 0x[0-9a-f]* returned to abort (rc=0)$"
verdict
boot B-nosmmu "${efi_b}" nosmmu
check "no SMMU: the built driver's DMA is denied" "^dma_gate: virtio_rng denied (NoSmmu"
check_absent "no SMMU: the device is never brought up" "^virtio_rng: up"
verdict

# ---- undo: one step back to image A ----
"${adb}" undo --state "${st}" --pci-id 1af4:1044 | tee "${run_dir}/undo.txt"
if [[ "$("${adb}" active --state "${st}" --pci-id 1af4:1044)" == none ]]; then
    echo "PASS  undo leaves no built driver active"; efi_next="${efi_a}"
else echo "FAIL  undo left the driver active"; all_fail=1; efi_next="${efi_b}"; fi
boot A2 "${efi_next}" smmu
check "after undo the rng is back to no driver" "^inventory: pci [0-9a-f:.]* 1af4:1044 .* driver=none contract=none proven=n/a$"
check_absent "after undo the built driver is not in the image" "^virtio_rng:"
verdict
gh_lock_release

# ---- cloud model ----
cloud_conf="${AIENOS_LLM_CLOUD_CONF:-}"
key_env="$( [[ -n "${cloud_conf}" && -r "${cloud_conf}" ]] && sed -nE 's/^key_env ([A-Za-z_][A-Za-z0-9_]*).*/\1/p' "${cloud_conf}" || true)"
if [[ -n "${key_env}" && -n "${!key_env:-}" ]]; then
    set +e
    "${adb}" build --inventory "${run_dir}/inventory-A.txt" --slot "${slot}" --llm "${cloud_conf}" \
        --state "${run_dir}/state-cloud" --work "${run_dir}/build-cloud" --rounds "${AIENOS_BUILD_ROUNDS:-4}" \
        --reference crates/allen-driver-builder/golden/virtio_rng.devc --machine qemu822 2>&1 | tee "${run_dir}/build-cloud.txt"
    cst=${PIPESTATUS[0]}
    set -e
    if [[ "${cst}" == 0 ]]; then echo "PASS  cloud model wrote a contract that passed every sandbox step"
    else echo "FAIL  cloud model build (exit ${cst})"; all_fail=1; fi
else
    echo "NOT_RUN  cloud model path: no llm.conf with a set key variable (AIENOS_LLM_CLOUD_CONF)"
fi

if [[ "${all_fail}" != 0 || -n "${AIENOS_QEMU_VERBOSE:-}" ]]; then
    for s in "${top}"/*/serial.txt; do
        echo "---- serial $(basename "$(dirname "${s}")") (driver lines) ----"
        grep -E '^(inventory|virtio_rng|dma_gate: virtio_rng|smmu: virtio_rng|smmu_dma_window: virtio_rng|devices:)' "${s}" | head -40 || true
    done
fi
if [[ "${all_fail}" == 0 ]]; then echo "${gate}: PASS"; exit 0; fi
echo "${gate}: FAIL"
exit 1
