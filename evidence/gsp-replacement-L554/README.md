# GB10 GSP replacement in seed-native Omega — L554

> [!WARNING]
> **UNQUALIFIED FOR REAL HARDWARE. OFFLINE RESEARCH ONLY.**
>
> This is a partial behavioral reconstruction, not a complete or bootable GSP
> firmware replacement. Do not install or flash it, replace the working GPU
> firmware, or use it to reset or reconfigure a live GPU.
>
> Unqualified hardware experiments can cause GPU failure, system hangs, loss of
> display, data loss, or an unbootable system. Recovery is not guaranteed;
> failures involving persistent boot firmware or security state may require
> vendor repair or replacement.
>
> Boot admission, full GPU functionality, power/thermal safety, and hardware
> recovery/rollback have **not** been qualified. Passing host, emulator, symbolic,
> or code-review checks does not establish hardware safety. Live hardware testing
> on the development Spark is not authorized.

**Verdict: NOT IMPLEMENTED / CUTOVER BLOCKED.** This directory is prerequisite
inspection evidence, not replacement firmware and not permission to load one.
The running GPU and firmware were not reconfigured.

## Requested result and boundary

Drake requests replacement of GSP firmware with our own implementation written
in `om` (seed-native Omega), without losing GPU functionality. This is a new
firmware scope beyond the host driver in L332 and aienux#286: both explicitly
retained vendor GSP-RM. Existing driver, manual migration (L550), compiler and
hardware-window owners remain unchanged; there is no second driver worker.

The target is the Spark GB10, identified by the driver as GB10B / family gb10y.
A successful host driver port, generated C, a renamed firmware blob, or a
RISC-V hello-world cannot satisfy this request. An initial implementation may
preserve immutable ROM and required security firmware, but must state every
remaining vendor stage. Keeping vendor GSP-RM running cannot count as replacing
GSP-RM.

## Fresh evidence

Reproduce from the task-owned Spark worktree:

```sh
python3 evidence/gsp-replacement-L554/probe.py
```

`probe.py` is inspection orchestration only, not part of the proposed firmware
or its seed-compiler ancestry. It invokes the **existing C inspector as an
external oracle**, not as a claimed Omega replacement. It builds only that
host utility. No device-node writes, MMIO commands, firmware installation,
module loading/unloading, reset or reboot are performed.

See [probe.json](probe.json) for exact commands, timestamps, exit codes and
binary/source hashes, and [source-excerpts.txt](source-excerpts.txt) for pinned
source lines. Inputs:

- aienux inspector source: `18e276590424593647cd7c39c6d16c2f9f8d6027`.
- NVIDIA open kernel modules: `20e4e6e19cc26ba47b5cbe23130a396be100c427`
  (580.173.02), with an empty `git status --porcelain`.
- Firmware: `/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin`, 75,012,080 bytes,
  SHA-256 `32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963`.

| Check actually executed | Result and limitation |
|---|---|
| GPU and GSP version query before/after | NVIDIA GB10 at `0000000F:01:00.0`; driver and running GSP both `580.173.02` |
| Existing inspector self-test | PASS: valid fixture; missing-family refusal; all truncations 0..639; 22 field mutations |
| Installed firmware container and pinned digest | PASS; selected `.fwsignature_gb10y`, 4096 bytes; this is **not cryptographic signature verification** |
| Missing-family and wrong-version negative controls on the real file | Both refused with exit 2 for their respective reasons; the file was not changed |
| Firmware hash before/after | Identical |
| Existing model-list endpoint before/after | HTTP success, `qwen3.6-35b-a3b-fp8`; this is an availability check, **not an inference or complete GPU regression suite** |
| Custom firmware compilation, signature admission, boot, full-function equivalence | NOT RUN / no replacement produced |

## What the public source establishes

Line references below refer to the NVIDIA commit above and are reproduced in
the excerpt file.

1. **Use the GB10B path, not generic Blackwell instructions.**
   `g_kern_fsp_nvoc.c:220-240` excludes GB10B from the FSP-present list;
   `g_kernel_sec2_nvoc.c:438-454` enables SEC2 GSP-FMC boot for GB10B.
   `kernel_gsp_gh100.c:775-808,845-900` selects and sends the SEC2 boot command.
2. **First-stage authentication material is explicit.**
   `kernel_sec2_gb10b.c:216-237` selects a debug-*signed* or production-*signed*
   FMC archive. Lines 438-535 retrieve its image, hash, signature and public
   key, and put them in the chain-of-trust payload. A debug archive's presence
   does not establish that this production device accepts arbitrary code.
3. **GSP-RM has a separate signature selection and transport.**
   `nv-firmware-chip-family-select.h:47-67` maps GB10B to GB10Y;
   `nv-firmware.h:50-68,85-109` maps that family to gsp_ga10x;
   `kernel_gsp_gb100.c:90-110` chooses the signature prefix;
   `kernel_gsp.c:4740-4905` checks the version, finds the chip-family signature,
   copies it and prepares the image page tables. The on-disk section is
   confirmed independently by the existing parser.

These observations establish the signed-image boot interface. They do **not**
reveal all internal ROM/FMC verification policy, prove the device's fuse state,
or constitute a real unsigned-image rejection experiment. The live GSP was
not disturbed to obtain such a result. No signing service, trusted key
provisioning or supported custom-firmware development route was established
by this inspection. Possession of the distributed public key is not possession
of a private signing key. Host UEFI Secure Boot settings are a separate trust
boundary and cannot be taken as proof that GPU firmware authentication is off.

**Immediate blocker:** we have no demonstrated way to boot our own GSP-RM
image with the privileges needed to preserve the GPU's services. Until one is
established, replacing the installed file would be an unqualified outage risk,
not a firmware replacement. This is a current qualification blocker, not a
claim that all possible custom-firmware routes are physically impossible.

## Gates for the actual replacement

This is the acceptance checklist for L554, not a competing whole-system plan.
All rows are OPEN. Do not mark them passed from this inspection receipt.

1. **Boot admission:** establish a supported signing/provisioning/development
   route on this exact GB10 revision and the privileges it confers. Audit both
   first-stage FMC and runtime GSP-RM boundaries. If evaluating a non-secure
   execution path, separately prove access to every service the replacement
   needs; code execution alone is insufficient. Hardware experiments require
   an isolated recoverable window, no live device holders, and the existing
   hardware owner's coordination. Do not burn fuses or alter persistent
   security state as an experiment.
2. **Compiler and image:** a seed-descended Omega compiler emits the required
   GSP ISA/ABI, startup/trap code and image layout; independent decoding and
   execution checks qualify the exact artifact. An AArch64 host executable or
   an sm_121 compute kernel is not GSP firmware. Exact GSP ISA extensions,
   privileged state and boot ABI need their own evidence.
3. **Complete service contract:** inventory all services exposed by the stock
   GB10 stack, not only the current model's RPC subset: initialization and
   teardown; memory/page tables, DMA and cache visibility; channel/context
   creation and scheduling/preemption; interrupts/fault reporting/reset;
   power, clocks and thermal protection; graphics/display and media services
   where this SKU supports them. Mark unsupported baseline features as such
   with evidence rather than silently dropping them. Reuse L192/L193/L332
   host protocol and simulator work as references, not firmware substitutes.
4. **Implementation and qualification:** implement the frozen service contract
   in seed-native Omega; run independent host/emulator oracles, refusal and
   compiled-mutant checks, then real cold-start/warm-start, compute, graphics,
   media, concurrency and failure-recovery comparisons against the frozen
   vendor baseline. Test sustained thermal/power behavior. Do not claim full
   functionality from one vector add or one successful inference request.
5. **Cutover and rollback:** first qualify recovery on isolated hardware or a
   coordinated recoverable window; pin every source/compiler/image and
   baseline; identify which vendor stages remain. Only switch the live GPU
   after all required parity and recovery gates pass. Keep the present known
   working path available until then.

No replacement firmware is running. The next hardware step is intentionally
not automated by this directory.
