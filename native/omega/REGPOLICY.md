# Seed-native register-access policy, RGP1

Current source composition: `regcore.om` followed by one LF and `regpolicy.om`. The policy functions now live only in the shared core; GSQ3 uses that same core directly. The test harness assembles these native source bytes before compiling. Original `policy-*` receipts describe the pre-extraction commit; `shared-policy-*` receipts qualify the current composition. See `GSQ3.md` for the integration contract.

`regpolicy.om` hand-ports the register permission and one-shot SEC2 doorbell policy from the pinned `native/linux/aien-gpu/aien_gpu.c` into seeded Omega. It decides permissions only: **no ioctl, MMIO, register values, firmware, module load, or live-driver replacement**. The C shim remains active code and is not retired by this offline milestone. GSQ2 still uses its eight-slot simulator; this policy is not yet attached to a hardware backend.

## Wire contract

Input: literal `RGP1`, then little-endian u32 `allow_boot`, `initial_doorbells`, `count`, then count pairs `(operation, BAR0_offset)`. Operation 0 means read, 1 means write; other operation numbers receive ENOTTY. `allow_boot` is 0 or 1; initial doorbell count may be any u32; count is at most256. Exactly the declared frame must be present, at most2064 bytes.

Output: little-endian u32 final simulated doorbell count, count, then one **positive errno** per request: 0 allowed, 1 EPERM, 114 EALREADY, 25 ENOTTY. Process exit0 means a well-formed frame was evaluated, NOT that every request was allowed. Exit2 rejects malformed/oversized frames or I/O. No output on input refusal; an output failure may leave a prefix. Short reads/writes are accumulated; EINTR refuses. One process/frame; no persistent permission or latch state outside that frame.

The complete frame is read before evaluation. Requests are evaluated in order; a denied request does not stop later requests or change the latch. Reads use the exact13-entry read list independent of allow_boot. Writes require allow_boot and the exact6-entry write list. Permission denial precedes repeat-doorbell denial. Only an allowed write to SEC2 QHEAD0 changes latch0 to1; any nonzero initial latch refuses that write without wrapping. GSP command-queue QHEAD0 is NOT this one-shot SEC2 latch. Values are absent because the pinned policy does not restrict write values.

The output latch models the permission state's transition on an accepted write. It does not attest that hardware performed a write. A future backend must keep checks and state changes serialized, bind state to the actual device instance, handle errors and qualify recovery before using this policy with real devices. Linux copy_from_user/copy_to_user faults, device lifetime, mutex/atomic-open behavior and memory mapping are NOT ported here.

## Verification

```sh
python3 native/omega/tests/regpolicy.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/regpolicy-mutants.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/regpolicy.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/regpolicy-io.py "$PWD"
```

Compiler pin remains01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6; output `build/omega-regpolicy/regpolicy` is checked as static AArch64 ELF. No C compiler/runtime constructs or executes the Omega artifact. Python is external test/reference infrastructure. The Python oracle parses exact allowlists and doorbell constant from the C shim and separately implements its inspected decision ordering. This is NOT execution of the kernel module or a complete C equivalence proof.

The C shim SHA256 is pinned10990a78ff93af70f8a7c2f36fb92fa05d8a53f787f70df77dfd73f28db6d5d6. Its ancestry is aienux2b47925d45171c2399e2617b62b85e6033e7747a. Source drift refuses qualification.

32 frames contain3332 evaluated requests plus malformed-input controls. Coverage includes every allowed address, adjacent/misaligned addresses, deterministic random u32 offsets, unknown operations, both boot-switch states, latch0/1/uint32max, repeated/mixed doorbells, empty and capacity frames. Observed allowed sets match the pinned lists in both directions over that corpus. Stub RED27failures; final GREEN0. Four compiled mutants are caught: read-all6, ignore-boot3, repeated-doorbell4, wrong-read-register6. Source restored/retested afterward. Three real I/O refusals are checked after a positive two-doorbell control.

Wolfram independently verifies all address conversions, RGP1 packing and frame bounds. Receipts are `evidence/omega-sequencer/policy-*`. Fresh reconstruction and current GSQ2/legacy boot regressions are separate. Static source review and hardware qualification of this new policy remain separate gates.
