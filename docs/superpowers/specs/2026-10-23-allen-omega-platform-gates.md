# L694 required provider handoff, WIP

No new compiler request or owner. Existing FR1/8/9/16 stay with L408.
L690's gateway implementation is untouched. No deployment is authorized here.

Verified immutable compiler source at omega-bridge-native-development
0fc7d8662cb799e7c206b17c18ea4c3ec2b8fea9, seed/omc/omc.om:
vs_fd/vs_arr/vs_off/vs_end implement Linux read/write, main startup exits via93;
elfhdr fixes ELF AArch64 and program headers. No x86/EFI codegen, MMIO,
interrupt, disk, socket or clock API is present at this checkpoint. This is an
inspection of the actual pinned compiler, not an inference from an old roadmap.
The fresh source-native bootstrap reproduces hash191b776e. There are no compiler
edits here and no foreign wrapper may substitute for the missing target.

| Existing dependency | Actual L694 entry/state to bind | Smallest qualification receipt needed |
| --- | --- | --- |
| FR9 x86 EFI/native target | same handwritten runtime/probe and tests | Source-only compiler pin produces nonempty PE32+ AMD64 EFI image with inspected headers, native marker in isolated iMac QEMU; then same behavioral gates on that target. Current ELF is not EFI. |
| FR8 privileged/device interfaces | Frame192x128, render/glow; start/heard/tick milliseconds | Native typed MMIO/port/IRQ/monotonic interface with bounds/refusals; keyboard event changes Prompt, frame buffer observes native pixels, monotonic timeout test. No C/asm wrapper to count as Omega product implementation. |
| FR1 durable storage provider | Enc4096, Unit4096; Tx target/generation, txwrite/txflush/txread; nwrite/nflush/nread | Native block read/write/flush on isolated disposable simulated/QEMU disk,512B and4KB geometry, fault at each phase, restart chooses valid newest; old copy bytes unchanged. Hash integrity, keyed authorization and anti-rollback each report separately. |
| FR16 bounded transport | In/Inlen, packet Outb/Outlen, qreceive/qtake | Immutable native socket/readiness/close API, isolated loopback recorded datagrams, partial writes/reads, EOF/error/timeout, malformed and session mismatch. No production endpoint. |
| iMac execution | cohesive runtime plus qualified providers | Existing iMac QEMU boot/chat/keyboard/bootdisk gates with complete Omega execution closure; then parent-signed physical trial. All remain NOT_RUN. |

Exact provider API names are not invented here: no qualified interface for these
features is supplied by the pinned compiler. The owner must hand off its actual
immutable signatures and executable receipts. L694's fixed-pool entry points
above define the consumer side, not an alternate socket or driver implementation.

Independent portable work remains in native/omega/allen/README.md. In particular,
a component render is not full visual parity and state restoration is not actual
durable transcript continuity. These are implementation gaps beyond platform
blocking and must remain visible to the parent. L694 is not closed or READY.
