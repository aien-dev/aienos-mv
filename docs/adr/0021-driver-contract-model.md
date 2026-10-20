# ADR 0021: Drivers are device contracts the kernel compiles

Status: Proposed (L215, 2026-10-20). Prototype on virtio-net; nothing here is retired yet.
Date: 2026-10-20
Governing: ADR 0012 rule 5 (no native admission without enforcement), ADR 0024 in aien-architecture
(Rust scaffolding, Omega destination), `native/kernel/core/smmu.c` (DMA windows),
`native/kernel/artifact/osc_receipt.h` (Omega receipts in the kernel).
Prototype: `native/net/contract/virtio_net.devc`, `crates/aienos-devc`, `native/net/contract/gen/`.

Tags used below: OBSERVED (ran it, command and exit code given), INFERRED (follows from what was
observed or read), UNKNOWN (not tested; each has an experiment).

## Context

Every Aienux driver today is hand-written C: `native/net/aienos_virtio_net.c` (345 lines),
`native/disk/disk_nvme.c` (537 lines), the xHCI, HDA and RTL8127 work in flight. Each one repeats the
same four jobs by hand: transcribe a register map from a manual, write the bring-up order, write a
chip model so host tests can run, and write the QEMU gate expectations. The four copies drift. The
review that a driver touches only its own registers and DMA memory is done by reading code. For chips
with no manual (the Spark's Realtek NIC, L198; MediaTek Wi-Fi, L206; MacBook GPU, L214) there is no
register map to transcribe at all, only a vendor driver to read.

Drake, 2026-10-20: "going forward can you reimagine how drivers are supposed to work in the future?
Right now it's kind of a pain in the ass."

## Decision

### 1. A driver is a device contract

A device contract is a data description of one chip, plus the small amount of logic that cannot be
data. It holds:

| Part | Contract words | Declarative? |
|---|---|---|
| identity | `match pci VID DID [subsys N]` | yes |
| evidence | `manual SOURCE SECTION "quote"`, `source` (vendor code, devc 2), `proven PLATFORM MACHINE DATE GATES COMMIT` | yes |
| register windows | `window NAME INDEX LOCATOR min LEN` | yes |
| registers | `reg WINDOW NAME OFFSET WIDTH ro/rw/wo [reset V] [bank SEL] [count N] [role R]` | yes |
| named bits, features, init flags, errors | `bits`, `feature`, `flag`, `error` | yes |
| resources the kernel grants | `dma region align N`, `irq none`, `queue-size min max pow2` | yes |
| DMA rings | `ring NAME INDEX KIND DIRECTION buf LEN stride S hdr H` | yes |
| bring-up state machine | `init ... end`, `qsetup ... end`: `write`, `poll`, `expect`, `negotiate`, `queues`, `stable-read`, `post`, `kick`, `armfail` | yes: steps are a closed vocabulary, no expressions |
| protocol rules | `rule REG write requires-status BITS [lacks BITS]` | yes: enforced by the generated chip model |
| ring engine (submit, complete, error, teardown) | `ring-kind virtq-split` selects a checked template in the generator | template, not per-chip code |
| logic that is not data | `logic` block (devc 2) | no: reviewed by hand, counted |

For virtio-net the `logic` part is empty: OBSERVED, `aienos-devc stats` prints
`hand_logic_lines=0` (the run is in "Prototype results"). The ring engine is a generator template
shared by every contract with the same `ring-kind`; a new ring shape (the RTL8127's OWN-bit
descriptors) is a new template, written once, not a new driver.

### 2. The kernel compiles the contract

Today (devc 1): a host tool, `crates/aienos-devc` (Rust, per ADR 0024 host-tool rule), parses and checks
the contract and emits C. The generated C is committed under `contract/gen/`, and
`make -C native/net contract-check` regenerates it in memory and fails on any difference, so the
kernel build never runs cargo and never trusts an uncommitted generator run.

Future (when Omegese stage 0 exists, L184 rungs K1, K2, K5): the contract is an Omegese unit,
`checker2` judges it, a receipt binds the compiled driver to the contract hash, and the kernel's
existing receipt verifier (`osc_receipt_verify`) admits it at boot or install time. The generator
already emits the Omegese form (`gen/virtio_net.omegese`); it is UNCHECKED because Omegese today has no
byte arrays (K1), no peek/poke of machine memory (K2) and no units (K5).

### 3. Capabilities come from the contract, so reach is checked by construction

The compiled driver reaches the device only through one accessor per declared register, and each
accessor bounds-checks against the window the kernel mapped. There is no generic "write offset X"
in the generated code. The checker refuses, before any C exists:

- a step that names an undeclared register, writes a read-only one or reads a write-only one;
- a register that lies outside its window's checked minimum length;
- a value wider than the register;
- a banked register touched outside the selector's scope;
- a doorbell written anywhere but `kick`;
- an interrupt line on a polled class;
- a contract with no `manual` line.

OBSERVED: the unit test `checker_refuses_reach_outside_the_contract` edits the virtio-net contract
eleven ways and requires each edit to be refused with the right reason.

Device-side reach (what the chip itself may DMA to) is the kernel's job and does not change: the
binder sizes one DMA region from the contract's layout (`vnet_mem_size`) and confines the device's
stream to it with the SMMU (`ck_dma_confine`); the existing negative test redirects RX descriptors
outside the window and requires an SMMU translation fault. The generated driver keeps every ring and
buffer inside that one region (one `dma region` statement, offsets computed by the template).

Security argument, in one line: the driver can only do what the contract says, the contract can only
say what the checker allows, and the device can only reach what the SMMU window allows. INFERRED from
the checker tests, the differential trace (no access outside the contract, see results) and the
existing SMMU negative gate. Not claimed: that the template itself is bug-free (it is ordinary C,
covered by the same hosted test, sanitizers and fuzzer as the hand driver).

### 4. One contract, every artifact

From one `.devc` the generator emits: the driver (header + C), the chip model (register file that
enforces access, reset values, banks and the `rule` lines, plus a split-ring device), a state-machine
fuzzer (random walks over init/submit/complete/reset against the model, both drivers), the
documentation page (`gen/virtio_net.md`), and the Omegese form. The QEMU gate expectations are the
same lines the hand driver prints; the kernel binder adds one line naming the contract and digest.

### 5. The contract carries its evidence

`manual` lines carry the exact quotes the manual-guard hook already asks for. `proven` lines carry
the platform, machine, date, gate names and commit of every hardware run. The generated header exports
`<PREFIX>_CONTRACT_PROVEN` (count of `proven` lines with a real commit). The kernel binder prints the
contract digest and proof count on every bind, and on a hardware staging image (`CK_HARDWARE_STAGING`)
refuses a contract with zero proofs: an UNPROVEN driver binds on attended boots (QEMU, tests) only.

### 6. Chips with no manual: traces and inference

Trace format `devc-trace 1`, one register access per line:

```text
R common+0x14/1 0x0      read  WINDOW+OFFSET/WIDTH VALUE
W common+0x14/1 0x1      write
```

Anything else on a line is ignored, so API markers and results can sit between accesses. The
generated chip model prints exactly this format; a recording of a vendor driver is converted to it
(QEMU `memory_region_ops_read/write` trace events or a hypervisor MMIO trap give address, size and
value; the converter maps an address to WINDOW+OFFSET using the BAR layout).

`aienos-devc infer TRACE DEVICE` turns a trace into a draft contract: every register touched with its
width and access, the value read before any write (reset candidate), every value written, repeated
equal reads (poll candidates), and the write order as a draft `init`. The draft has no `manual` or
`proven` line, so the checker refuses it: inference proposes, a person or an AI writes the evidence,
the checker and the gates decide.

`aienos-devc covers CONTRACT TRACE` is the reverse check: every access in a recording must be a declared
register with an allowed access. It is how a contract written from vendor code (RTL8127) is held
against a recording of the vendor driver before it may be marked PROVEN (`trace-required`, devc 2).

### 7. Migration: the hand-written drivers stay as the reference

A hand-written driver is retired only when its generated twin (a) passes the same hosted tests, (b)
produces a byte-identical register and DMA trace against the generated chip model, (c) passes the same
QEMU gates, (d) is within noise of the same timing on the same image and host, and (e) has a `proven`
line from real hardware where the hand driver had one. Until then both build: `CK_VNET_IMPL=contract`
selects the twin in the kernel image (`full-vnet-contract`), and the QEMU gate takes
`AIENOS_CK_NET_IMPL=contract`.

Order after virtio-net: virtio-rng (L268 is adding class `entropy-poll` on this format), NVMe (needs
a `ring-kind nvme-sq-cq` template; feeds four gates, so second), RTL8127 (L198, needs devc 2 below).

## Prototype results (virtio-net)

Why virtio-net and not NVMe: virtio-net is 345 lines behind one ops interface, has a hosted simulated
test (`tests/vnet_test.c`) and one self-contained QEMU gate with an SMMU fence
(`scripts/qemu_ck_net_test.sh`). `disk_nvme.c` is 537 lines and feeds four or more gates (M4_NVME,
M4_STORE, SMMU, NVME_SHUTDOWN), a larger blast radius for a first prototype.

All on the Spark (aarch64 host, QEMU), branch `feat/driver-contract-model`. Commands and exit codes:

| Check | Command | Exit | Result |
|---|---|---|---|
| contract compiles, checker refuses 11 bad edits, trace inference | `cargo test -q -p aienos-devc` | 0 | OBSERVED: 5 tests pass |
| lint | `cargo clippy -q -p aienos-devc --all-targets -- -D warnings` | 0 | OBSERVED: clean |
| no drift; recording stays inside the contract | `make -C native/net contract-check` | 0 | OBSERVED: `AIENOS_DEVC_CHECK: PASS`, `AIENOS_DEVC_COVERS: PASS (3979 accesses, 25 registers, 0 outside)` |
| same hosted test as the hand driver | `make -C native/net contract-test` | 0 | OBSERVED: `VIRTIO_PCI_QEMU822_CAPS`, `VNET_ACCESS_PLATFORM_SIM`, `VNET_M6A_UDP_ROUNDTRIP_SIM`, `VNET_SIM_DATAPATH` all PASS on the generated driver |
| fuzzer, both drivers | (same) | 0 | OBSERVED: 20000 walks each, `VNET_FUZZ: PASS`, identical visit counts (tx 245090, rx 245077, hostile 30870, reset 15190) |
| differential register + DMA trace | (same) | 0 | OBSERVED: `VNET_TRACE_DIFF: PASS (100952 lines identical, hand vs generated)` |
| sanitizers | `make -C native/net contract-sanitize` | 0 | OBSERVED: all of the above under ASan + UBSan |
| QEMU NET gate, hand driver | `scripts/qemu_ck_net_test.sh` | 0 | OBSERVED: `AIENOS_CK_NET: PASS`, 86 PASS, 0 FAIL |
| QEMU NET gate, generated driver | `AIENOS_CK_NET_IMPL=contract scripts/qemu_ck_net_test.sh` | 0 | OBSERVED: `AIENOS_CK_NET: PASS`, the same 86 PASS lines plus `driver generated from the virtio-net contract, digest printed, UNPROVEN`, 0 FAIL. Includes the SMMU fence: RX descriptors redirected outside the window fault, the device recovers inside it. |

Size: the contract is 114 statements (171 lines with comments); the generated driver is 377 lines
against the hand driver's 345; `hand_logic_lines=0` (OBSERVED, `aienos-devc stats`).

Two defects the process found and fixed (not waived): the first generated C failed the strict build
(unused register accessors; a `length >= 0` test on an unsigned field in the model), and the first
fuzzer judged a planted bad completion wrongly when the walk touched the other queue first (it failed
the hand driver identically, which is how it was found to be the fuzzer's fault).

Timing, three receipts in `native/net/contract/receipts/`, all under `quiet-measure` (fast cores, no
builds running):

| Receipt | What | Hand | Generated | Verdict |
|---|---|---|---|---|
| `vnet-timing-20261020T184324Z` | hosted twin, -O2, same chip model, 20 rounds x 5 reps interleaved; noise bound from hand odd vs even rounds | init 2210 ns, round trip 102 ns | init 2188 ns, round trip 102 ns | OBSERVED: `VNET_TIMING: WITHIN_NOISE` (bounds 44 ns and 2 ns), exit 0 |
| `vnet-qemu-timing-20261020T184324Z` | QEMU NET gate, 5 rounds, hand always first | init median 1302 us, round trip 5472 us, gate wall 19.4 s | init 1479 us, round trip 4425 us, wall 20.1 s | OBSERVED: 10/10 gates PASS; init higher for the generated driver in 5/5 rounds |
| `vnet-qemu-timing-abba-20261020T185822Z` | the control: 6 rounds, order hand, generated, generated, hand | init median 2412 us (spread 1980 to 3477), round trip 8529 us, wall 21.8 s | init 2101 us (spread 1412 to 3708), round trip 4367 us, wall 22.0 s | OBSERVED: 24/24 gates PASS; the init sign flipped |

Reading (INFERRED): the 5-round gap in init was the run order and the box, not the driver. With the
order balanced the generated driver came out faster, and the two init routines are 2712 and 2768
bytes of machine code (`nm -S` on both kernel images). QEMU here is TCG (software emulation, one
thread), so in-guest timings vary widely between identical runs of the same image (hand driver alone: init
1.76x, calc:ca3c49b9, round trip 1.86x, calc:f38d479a, plus one 105783 us outlier); the in-guest numbers can show a large regression but not a small one. The hosted twin is
the precise comparison and it is within noise. Rule (d) is met for QEMU; real hardware is U1.

## Options considered

1. Keep hand-writing C drivers, add more review. Upside: nothing new. Downside: the drift and the
   reach review stay manual; chips with no manual stay a reading exercise.
2. Write drivers directly in Omegese now. Downside: Omegese cannot express MMIO or byte buffers until
   L184 K1/K2/K5; blocks on that work.
3. Device contracts compiled by a host tool now, by Omega later (chosen). Upside: works today, the
   format is designed to become an Omegese unit, every artifact comes from one source. Downside: a new
   generator to maintain, and ring templates are still C.

## Open questions (UNKNOWN, each with its experiment)

| # | Question | Experiment |
|---|---|---|
| U1 | Does the generated driver stay within noise on real hardware, not only QEMU? | Boot `full-vnet-contract` on a Spark with a virtio NIC under a hypervisor (attended), same timing run as the QEMU receipt. |
| U2 | Can the OWN-bit ring of the RTL8127 be one template shared with Intel/Broadcom shapes? | Write the `own-bit` template, generate RTL8127 against L198's chip model, run L198's tests on it. |
| U3 | Does `infer` recover a usable bring-up from a real vendor trace (noise: timers, interrupts, PHY polling)? | Record r8127 under QEMU with a passthrough-free model or a hypervisor trap; `infer`; compare with the sketch. |
| U4 | How much of a GPU (L214) fits the declarative part? | Draft the MacBook GPU contract; count `logic` lines against total. |
| U5 | Can Omegese stage 0 compile the contract with checker2 receipts? | After L184 K1/K2/K5 land: compile `gen/virtio_net.omegese`, verify the receipt with `osc_receipt_verify`. |
| U6 | Interrupt-driven classes: what does `irq msix N` grant and how is it checked? | Add class `net-irq` with an MSI-X vector capability; negative test: a driver that unmasks an undeclared vector is refused. |
| U7 | Why is the generated driver's in-guest round trip lower in both QEMU receipts (median 4425 vs 5472 us, 4367 vs 8529 us) when the hosted twin and the register trace are identical? | Same gate with `-accel kvm` on an aarch64 host, ABBA order, 12 rounds; if the gap holds, swap the link order of the two driver objects to test code placement. |

## What devc 2 must add (from the RTL8127 sketch)

`transport pci-bar` (registers in a BAR, no capability list), `ring-kind own-bit`, `source` evidence
(vendor code with line ranges), `trace-required`, a DMA mask on `dma region`, and a `logic` block for
the PHY and firmware-patch steps that are not data. The sketch is
`native/net/contract/rtl8127.devc.sketch`.
