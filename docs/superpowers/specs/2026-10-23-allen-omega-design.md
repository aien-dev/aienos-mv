# L694 Allen Omega contract freeze (WIP)

Approved authority: /home/drakestapleton/handoffs/briefs/L694-allen-omega.md.
Baseline da55660ea7337f2b6a42095fcd1fb6ddebe1cc7c. No master sequencing change.

All new implementation and test programs are handwritten seeded Omega. Shell,
Git, sed, cat, sha256sum, readelf and quiet-build are external development tools.
No foreign runtime, generated C, compiler edits, service changes or deployment.

## Contract and source map

* svc/allen_intent.c: JOB/SAY/WAIT, code and code-more vs ordinary model chat.
* x86/kmain_x86.c code_request/courier_step: one outstanding job, immediate code
  acknowledgment, stale sequence rejection, 20s slow and 30s silence timeout.
* svc/allen_code.c: 32768 bytes, 128-byte pieces, 256 slots, IEEE CRC32, 800
  lines, escaped newline/backslash. Complete code alone becomes displayable.
* x86/courier_udp{.h,_x86.c}: ALLEN-COURIER v=1, 16 lowercase hex session,
  1000-byte JOB pieces, 4096-byte job; ordered LINE delivery, duplicate GOT,
  wrong-session refusal. Protocol core only; no socket attachment.
* svc/allen_memory.c: AIENALM1, versions 2/3/4; payload 40 + 113*nfacts
  + (v>=3 ? 24 : 0) + (v>=4 ? 12 : 0). Header64, generation64 at16,
  SHA256(header[0:32] || payload), zero header24:32 and trailing unit bytes.
  Facts32 maximum, notes512. AIENNOT1 note length1..127; index, start, gone.
  Two-copy save writes older, flushes, reads back, then publishes generation.
* svc/allen_scene.h, docs/ALLEN_CANVAS.md and ui/allen_ui.c: bounded scene,
  calm sky/presence, no traditional desktop; code row wrapping and scrolling.

## Boundary and intentional changes

Fixed pools, explicit lengths, no pointer/heap/OS provider. Runtime caller supplies
monotonic milliseconds and session; identity remains Allen unless restored.
Overflow rejects whole operation rather than silently truncating user/model text.
Request IDs never reused across reset. Duplicate conflicting pieces rejected;
malformed numeric overflow rejected. No checksum claimed to authenticate a sender.
Disk tests use fixed buffers and injected failures, never a physical disk. SHA
integrity, keyed authorization, and freshness are independent; latter two are
unimplemented here and are never inferred from a successful SHA check.

## Required unclosed gates

Full legacy semantic/UI equivalence, transcript persistence, qualified native
provider integration, x86 EFI image, MMIO/interrupt/keyboard/framebuffer/disk/NIC,
iMac QEMU chat/keyboard/bootdisk and signed physical trial. Seed compiler currently
emits Linux AArch64 only. FR1/8/9/16 remain with L408; no C bridge may satisfy them.
Hermes JSON/WebSocket/admission belongs to L690 and is not reimplemented here.
