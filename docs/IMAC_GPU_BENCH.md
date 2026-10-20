# IMAC GPU Bench: interfaces for feat/imac-gpu-bench

## Purpose

Two tools for lab bringup of the iMac's AMD Radeon Pro 555/560 (Polaris 11, PCI
01:00.0, id 1002:67ef) without firmware boots:

1. **Live kernel swap** (`ck_imac_swap`): while Allen is up, push a new signed
   kernel to the running iMac over UDP; the running kernel verifies it, quiesces
   devices, and jumps in — no Apple logo, no UEFI entry.

2. **GPU bench console** (`ck_gpu_bench`): lab-only authenticated RPC over UDP;
   read/write PCI config and GPU registers in BAR5 (256 KiB MMIO) and VRAM
   through BAR0, with an allowlist, per-write before/after logging, all-ones
   guard, TSC-bounded polls, and HMAC-SHA256 replay protection.

These interfaces are for the sibling streams:
- `feat/imac-gpu-core` (`~/workspace/aienos-gpu-core`): uses `ck_gpu_bench` to
  probe and init the DCE/DPG display hardware.
- `feat/imac-gpu-shader` (`~/workspace/aienos-gpu-shader`): uses `ck_gpu_bench`
  to validate GCN shader block register access.

## Wire formats

### Ports

| Port  | Purpose       | Direction    | Auth          |
|-------|---------------|--------------|---------------|
| 47034 | Live swap     | Spark → iMac | Ed25519 (manifest) |
| 47035 | GPU bench     | Spark ↔ iMac | HMAC-SHA256   |

### Live-swap protocol (port 47034)

All integers big-endian. The payload is a signed slot object:
`CK_SLOT_UNIT` bytes (manifest, `native/boot/slot_format.h`) followed by the raw
kernel PE image.

```
PUSH packet (Spark → iMac):
  magic[4]  = "ASWP"
  version   = 1
  type      = 1 (DATA) | 2 (END)
  pad[2]    = 0
  nonce[8]  = session-unique random (echoed in ACKs)
  offset    u32 = byte offset in payload
  total     u32 = total payload bytes (manifest + image)
  len       u16 = bytes of data in this packet (0 for END)
  pad[6]    = 0
  data[len]    = payload bytes

ACK packet (iMac → Spark):
  magic[4]  = "ASWK"
  version   = 1
  status    = 0 (want_more) | 1 (verified, jumping) | 2 (fail)
  pad       = 0
  nonce[8]  = echoed from PUSH
  want      u32 = next expected offset (window ack)
  total     u32 = echoed
  text[n]   = NUL-terminated printable status (max 80 bytes)
```

Chunk size: 1024 bytes (NB_CHUNK, matches netboot). Window: 16 chunks.
The Spark resends missing chunks after a 200 ms timeout.

### GPU bench protocol (port 47035)

Authentication key: 32-byte HMAC-SHA256 key. In lab/QEMU builds the key is the
test key `AIENOS_BENCH_KEY` (16 zero bytes + 0x01..0x10). In production builds
it is set at `make imac-kernel CK_X86_BENCH_KEY=<32 hex bytes>` time, binding
the key into the signed image. The Spark tool reads the key from
`~/.local/state/aienos/bench-key.bin` (32 bytes) or `--key <file>`.

Replay protection: a 64-bit monotone counter. The kernel rejects any request
whose counter is ≤ the last accepted counter.

```
Request (Spark → iMac, UDP broadcast, port 47035):
  magic[4]  = "ABCH"
  version   u8 = 1
  cmd       u8 = BENCH_CMD_* (see below)
  pad[2]    = 0
  counter   u64 be = monotone replay counter (starts at 1)
  arg0      u32 be = register index (BAR5), PCI offset, or VRAM word offset
  arg1      u32 be = write value, or poll mask
  arg2      u32 be = poll expected value, or RMW mask
  arg3      u32 be = poll timeout ms, or RMW set bits
  hmac[32]  = HMAC-SHA256(key, magic..arg3)

Response (iMac → Spark, UDP unicast or broadcast, port 47035):
  magic[4]  = "ABCR"
  version   u8 = 1
  status    u8 = BENCH_OK | BENCH_ERR_*
  pad[2]    = 0
  counter   u64 be = echo
  tsc       u64 be = TSC when response was formed
  result    u32 be = value read (or 0 for write-only ops)
  before    u32 be = value before a write (0 for read-only ops)
  hmac[32]  = HMAC-SHA256(key, magic..before)
```

#### Commands

| Code | Name                | arg0       | arg1        | arg2      | arg3      |
|------|---------------------|------------|-------------|-----------|-----------|
| 0x00 | BENCH_CMD_PING      | 0          | 0           | 0         | 0         |
| 0x01 | BENCH_CMD_PCI_CFG   | cfg_offset | 0           | 0         | 0         |
| 0x02 | BENCH_CMD_BAR5_READ | reg_index  | 0           | 0         | 0         |
| 0x03 | BENCH_CMD_BAR5_WRITE| reg_index  | value       | 0         | 0         |
| 0x04 | BENCH_CMD_BAR0_READ | word_off   | 0           | 0         | 0         |
| 0x05 | BENCH_CMD_BAR0_WRITE| word_off   | value       | 0         | 0         |
| 0x06 | BENCH_CMD_BAR5_POLL | reg_index  | mask        | expected  | timeout_ms|
| 0x07 | BENCH_CMD_BAR5_RMW  | reg_index  | clear_mask  | set_bits  | 0         |

`reg_index` is a DWORD index into BAR5 (MMIO byte offset = reg_index × 4).
`word_off` is a DWORD offset into BAR0 VRAM (byte offset = word_off × 4).

#### Status codes

| Code | Name                  | Meaning                                |
|------|-----------------------|----------------------------------------|
| 0x00 | BENCH_OK              | success                                |
| 0x01 | BENCH_ERR_AUTH        | HMAC mismatch or counter replay        |
| 0x02 | BENCH_ERR_RANGE       | register not on allowlist              |
| 0x03 | BENCH_ERR_ALLONES     | read returned 0xFFFFFFFF (device gone) |
| 0x04 | BENCH_ERR_TIMEOUT     | TSC-bounded poll timed out             |
| 0x05 | BENCH_ERR_NODEV       | GPU not found or BAR not mapped        |
| 0x06 | BENCH_ERR_BUSY        | kernel not in bench-ready state        |

All-ones check: any read that returns 0xFFFFFFFF sets BENCH_ERR_ALLONES and
stops further batch operations. This catches a wedged PCIe link immediately.

#### Register allowlist

BAR5 writes are restricted to these DWORD-index ranges:
```
0x4800 – 0x4900  DCE 11.2 panel power sequencer + backlight PWM
0x4A00 – 0x4C00  DCE 11.2 CRTC / display engine registers
```
Any write outside these ranges returns BENCH_ERR_RANGE.

BAR5 reads: any DWORD index in [0, 65535] (256 KiB BAR5 range).
BAR0 reads and writes: any DWORD offset in [0, 65535] (256 KiB window; the
BAR0 aperture is 256 MiB but we restrict to the low 256 KiB for safety).

## Kernel build flags

| Flag                | Default | Purpose                                        |
|---------------------|---------|------------------------------------------------|
| `CK_X86_SWAP=1`     | 0       | Compile live-swap receiver (LAB BUILD)         |
| `CK_X86_BENCH=1`    | 0       | Compile GPU bench console (LAB BUILD)          |
| `CK_X86_BENCH_KEY=` | test    | 32 hex bytes; bench HMAC key bound in image    |

Both flags require `CK_X86_REPORT=1` (the lab build). The default
`make imac-kernel` produces neither; `make imac-kernel CK_X86_REPORT=1 CK_X86_SWAP=1 CK_X86_BENCH=1`
produces `IMACKERN-LAB.EFI` with all three.

## Spark tools

```
# Build
make -C native/kernel imac-kernel CK_X86_REPORT=1 CK_X86_SWAP=1 CK_X86_BENCH=1
make -C native/kernel ck-imac-swap ck-gpu-bench

# Push a new kernel (no firmware boot):
tools/ck_imac_swap push IMACKERN-LAB.EFI --key ~/.local/state/aienos/netldr-key.bin

# Run a bench script:
tools/ck_gpu_bench --key ~/.local/state/aienos/bench-key.bin read_reg.bench
```

### ck_imac_swap usage

```
ck_imac_swap push <IMACKERN.EFI> [--key <slot-signing-key>] [--target <ip>]
  Push a signed kernel to the running iMac. Wraps the EFI into a signed
  slot object, sends over UDP port 47034 in 1024-byte chunks (window 16).
  Prints a progress line per chunk and the new kernel's first report line.
  Default target: 255.255.255.255 (broadcast). Default key: uses the
  TEST key (only valid against a test-key-built NETLDR).

ck_imac_swap keygen <key-file> <pub-file>
  Generate a fresh Ed25519 key pair (calls the existing slot-tool logic).
```

### ck_gpu_bench usage

```
ck_gpu_bench [--key <file>] [--target <ip>] [--log <dir>] <script.bench>
  Run a bench script. Each line: <cmd> [args...]. Comments start with #.
  Logs every transaction to <log>/<timestamp>-<cmd>.json.
  Stops immediately on BENCH_ERR_ALLONES.

Commands in a .bench script:
  ping
  pci_cfg <offset>
  bar5_read <reg_index>
  bar5_write <reg_index> <value>
  bar0_read <word_off>
  bar0_write <word_off> <value>
  bar5_poll <reg_index> <mask> <expected> <timeout_ms>
  bar5_rmw <reg_index> <clear_mask> <set_bits>
```

## Honest limits

- A wrong GPU write can wedge the PCIe link; only the 4 s power hold (firmware
  boot) recovers. The register write allowlist and the before/after logging
  make this rare.
- The swap trampoline uses the AP trampoline page (`tramp_pa`, < 1 MiB). If
  the frame allocator did not get a page below 1 MiB, the swap is refused.
- QEMU swap test uses a disk-staged image (no QEMU model of the BCM57766);
  the UDP receive path is exercised only on real hardware.
- The GPU bench VRAM window covers only the first 256 KiB of BAR0 (not the
  full 256 MiB aperture) to bound accidental overwrite.
- No multi-kernel in-flight: if a swap is already in progress, the second
  request is refused.
