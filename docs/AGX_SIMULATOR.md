# Simulated AGX: the device AienOS's Apple GPU driver is written against first

Ledger L214, step 5. Code: `native/gpu/agx/agx_{fw,uat,sim,job}.{h,c}`, test
`native/gpu/agx/tests/agx_sim_test.c`, gate `AIENOS-NATIVE-AGX`
(`tests/gates/run.sh native-agx`, marker `AIENOS_AGX_SIM: PASS`).

The approach is the same as the GB10 simulator (ledger L193 on the Spark):
the driver writes the real bytes into simulated physical memory, and a host
model of the device reads them the way the hardware does. Here the "device" is
the M2's GPU firmware plus the GPU itself, so the model covers the firmware's
compute work queue, the command stream, the shader-core state words and an
interpreter for the shader instructions vector add and matrix multiply need.

Every claim below is tagged **OBSERVED** (seen on this MacBook Air: M2, G14G,
macOS, Metal), **INFERRED** (from the named source) or **UNKNOWN** (with the
experiment that resolves it).

## 1. Run it

```
make -C native/gpu/agx test        # ISA suite, then the simulator suite
make -C native/gpu/agx sanitize    # both under ASan + UBSan (Linux)
make -C native/gpu/agx gpu-ref     # the same six cases on the real GPU (macOS)
```

## 2. What a job goes through

```
driver (agx_job.c)                     simulated AGX (agx_sim.c)
------------------                     -------------------------
page tables, TTBAT, VM per slot  --->  every read/write goes through the UAT:
                                       firmware structures via context 0 with
                                       firmware rights, everything else via the
                                       job's slot with GPU rights
code + USC words + uniforms + CDM
  in the user VM (GPU read-only)
RunCompute + microsequence in the
  ring slot's kernel page
ring[wptr] = RunCompute, cpu_wptr
RunWorkQueueMsg  ------------------->  QueueInfo -> RingState -> ring[rptr]
                                       -> RunCompute (tag 3) -> microsequence
                                          StartCompute .. WaitForIdle ..
                                          FinalizeCompute, RetireStamp
                                       -> JobParameters1/2: CDM base/end,
                                          usc_exec_base
                                       -> CDM: launch, barrier, terminate
                                       -> USC words: Uniform, Shared, Shader,
                                          Registers, No preshader
                                       -> threadgroups of 32-lane SIMD groups
                                       <- stamps, gpu_rptr/doneptr, event
agx_compute_done(stamp)  <-----------  (or: error_count++, error event, no stamp)
```

The firmware is a black box on the real chip. The model only does what the
driver can observe: it consumes the queue, honours the fields the driver sets,
writes the stamps, and moves the ring pointers. What it does not model is
listed in section 7.

## 3. Results

**OBSERVED** (`make test` on this Mac, and on the Spark under GCC with ASan and
UBSan, see the handoff for the run): the six Apple-compiled kernels in
`tests/fixtures/corpus` (the bytes Metal's compiler produced on this machine),
submitted through `agx_job.c` and run by the model, produce outputs that equal
the C reference word for word and hash to exactly the value the real GPU
produced through Metal (`make gpu-ref`, same inputs, FNV-1a 64 over the whole
output buffer; re-run on 2026-10-20 after this step, all six MATCH):

| Case | Real GPU (Metal) | Simulated AGX | Threadgroups | SIMD-group instructions |
|---|---|---|---|---|
| vadd | 756c2c450553232e | 756c2c450553232e | 16 | 224 |
| vadd_i32 | 86155d9e444cd3f4 | 86155d9e444cd3f4 | 16 | 224 |
| vadd_bounds | 2f7e701fd70bfe49 | 2f7e701fd70bfe49 | 16 | 352 |
| saxpy | 0a638a1562db4a85 | 0a638a1562db4a85 | 16 | 224 |
| matmul_naive | cf1cc1653f9d0535 | cf1cc1653f9d0535 | 4 | 10784 |
| matmul_tiled | cf1cc1653f9d0535 | cf1cc1653f9d0535 | 4 | 8384 |

`vadd_bounds` runs 1024 threads over 1000 elements, so the hash also covers
the 24 untouched sentinel words. The two matmuls use loops, nested execution
masks, threadgroup memory and barriers. 22 distinct instructions run:
`ldimm16 ldimm32 mfsr iadd imul imadd ffma fadd stop jmp_any jmp_none pop_exec
if_icmp while_icmp if_fcmp icsel wait load store lload lstore barrier`.

Fault paths, all checked in the same test (177 checks):

- MMU: a store into the unmapped guard page after a buffer, a store into a
  buffer mapped GPU read-only, and VM 2 using VM 1's addresses each fault with
  the right slot, address, access and UAT error. No stamp lands,
  `error_count` goes up, an error event is logged, and the next job on the
  queue still runs.
- Refused encodings: `b26clr` (A3), `tgu` = 2 (A1) and a USC Preshader word
  each fault as unsupported at the right instruction. Undecodable code, and a
  shader that uses threadgroup memory with none allocated, are refused by the
  driver before submission.
- Bounds: `matmul_tiled` with 1792 bytes of threadgroup memory faults; it
  needs Metal's 2048 (see 5.2). A USC Registers word below the shader's use
  faults.
- Malformed commands: a bad microsequence header, an indirect CDM launch, a
  CDM stream without a terminate, a doorbell whose write pointer disagrees
  with the ring, and a non-compute pipe are all refused.
- Watchdog: a jump to itself stops at the step limit.
- Ring: ten jobs through a four-entry ring (two wraps, stamps in order); with
  the firmware stalled, three jobs fit, the fourth is refused as ring full,
  and one doorbell then retires all three.
- UAT: map over a mapping, unaligned, out of range, outside DRAM, unmap and
  remap, translation across a page, unbound slot, firmware-only and GPU-only
  memory, allocator exhaustion.

## 4. Metal's argument convention (what the driver must load)

| Claim | Status | Source |
|---|---|---|
| Buffer k's 64-bit address is in uniform halves 4k..4k+3 (`du0`, `du2`, `du4`, ...). | INFERRED from Apple's compiled code (vadd loads `du0`, `du2`, stores `du4`); OBSERVED to give the real GPU's results above. | corpus disassembly, section 3 |
| A `constant T &x [[buffer(k)]]` value is preloaded right after the address table (saxpy's alpha at `u6` with 3 buffers, vadd_bounds' and matmul's n at `u8` with 4). The constant's own table slot still exists. | INFERRED (same evidence). On macOS a preshader or the driver loads it; ours loads it with a USC Uniform word directly. | corpus disassembly |
| The uniform count in the CDM launch word, the USC Uniform word layout (start and size in halves, address >> 2) and the USC Registers count (groups of 8 32-bit registers, 0 = 256). | INFERRED | Mesa `cmdbuf.xml` at ba3dacb9 |
| Threadgroup memory size comes from Metal's `staticThreadgroupMemoryLength` (2048 for matmul_tiled, 0 otherwise). | OBSERVED | `make gpu-ref` prints it as `tg_mem` (Metal's `staticThreadgroupMemoryLength`) |

Whether macOS feeds constants through a preshader or a direct Uniform word on
this chip is **UNKNOWN**; the trace (docs/MACBOOK_AGX_TRACE_PROCEDURE.md,
rows U3 and U4) shows the USC words macOS actually submits.

## 5. Instruction semantics the model uses

Encodings and operand decoding come from `agx_isa.c` (Mesa `AGX2.xml`, MIT,
plus the AienOS additions A1 to A3 in docs/AGX_ISA_NOTES.md). Semantics:

| Instruction(s) | Semantics in the model | Status |
|---|---|---|
| `if_icmp`, `if_fcmp`, `while_icmp`, `while_fcmp`, `pop_exec` | Per-lane nesting counter in r0l; then exec = (r0l == 0). if: `v != 0 ? v += n : (!cond ? v = 1)`; while: `v < n ? v = (cond ? 0 : n)`; pop: `v = max(0, v - n)`. The execution mask is separate state: other instructions may overwrite r0 (vadd loads into r0) without changing which lanes run. | INFERRED (applegpu pseudocode and its `corestate.exec`, BSD-3); OBSERVED consistent with all six results |
| `jmp_any`, `jmp_none` | Branch to the instruction's own pc + signed 32-bit offset if any / no lane is active. | INFERRED (applegpu); the offsets in vadd_bounds and both matmuls land on instruction starts |
| `icsel`, `fcsel` | `D = cond(A, B) ? X : Y` | INFERRED (Mesa) |
| integer conditions | bits 0..1 eq/lt/gt, bit 2 signed, bit 3 invert; compared at 32 bits unless a 64-bit operand | INFERRED (Mesa enum values) |
| float conditions | eq, lt, gt, gte, lte false on NaN, bit 3 inverts. The `n` forms (`fltn`, `fgtn`) are refused: NaN rule UNKNOWN | INFERRED; UNKNOWN for `n` forms. Experiment: run `if_fcmp` with each form on NaN inputs through Metal. |
| `iadd`, `isub`, `imul`, `imadd`, `imsub` | `A + (B << shift)`, `A * B + (C << shift)`, wrapping, truncated to the destination size. Integer `saturate` is refused. | INFERRED (Mesa); OBSERVED for vadd_i32 (wrapping) |
| `fadd`, `fmul`, `ffma` | binary32 IEEE, `ffma` fused (one rounding); 16-bit operands converted from and to binary16 (round to nearest even); `saturate` clamps to [0, 1]. Denormal and NaN handling of the hardware is not modelled. | INFERRED (Mesa); the corpus is exact either way (agx_cases.h picks inputs so) |
| `load`, `store` | address = base (64-bit) + (index x element size) << shift; index zero-extended unless the operand is sign-extended; i8/i16/i32; contiguous masks only; `b26clr` refused | element scaling OBSERVED (results); shift INFERRED (Mesa); A3 UNKNOWN |
| `lload`, `lstore` | `tgu` = 1 with `tg47` set and an immediate index: address = base (byte address in a 16-bit register) + index x element size. Anything else refused. | INFERRED, 5.2 |
| `mfsr` | thread/threadgroup positions, sizes, SIMD lane and group indices, active-lane counts | INFERRED (Mesa sr enum); OBSERVED for the ones the corpus reads |
| `wait`, `barrier`, `stop` | loads complete at once; barrier holds a SIMD group until every group of the threadgroup is at a barrier or stopped | INFERRED |

### 5.1 Correction carried from step 2

docs/AGX_ISA_NOTES.md said the simulator would treat the `lload`/`lstore`
index with `tg1` as a plain byte offset. That was wrong. See 5.2.

### 5.2 The `lload`/`lstore` index unit (A1 = 1)

matmul_tiled addresses both 16 x 16 float tiles with index `-4` on bases
computed as `16 + 4 * (...)` bytes, and Metal reports 2048 bytes of
threadgroup memory for it (**OBSERVED**). Reading the index in elements
(-4 x 4 bytes) places the tiles in exactly [0, 2048). Reading it in bytes
places the top of the second tile at 2060, past what Metal allocates. The
model therefore uses elements (**INFERRED** from that exact fit), and the
test shows the dependency: with 2048 bytes the job matches the real GPU, with
1792 it faults on bounds.

The hash comparison alone cannot tell the two readings apart, because every
access in matmul_tiled uses the same index, so either reading shifts all
accesses by the same 12 bytes. **Experiment that settles it** (user space, no
reboot): patch a copy of matmul_tiled so one `lstore` uses index `-3` while
its partner `lload` keeps `-4` and its base is 4 bytes lower, run it through
Metal (`tools/agx_metal_run.m` with a replaced pipeline, applegpu
`hwtestbed` style), and check whether the stored element is read back.

## 6. Driver side (`agx_job.c`)

What it does, freestanding (no heap, clock or I/O; `make lib-checks`):

- Page allocator, TTBAT, a kernel page-table tree (context 0's ttb1 and every
  user context's ttb1), user trees per context (ttb0), mapping with
  drm/asahi's named protections. Every user allocation gets an unmapped guard
  page after it.
- The compute queue: QueueInfo, RingState, an 8-byte-per-entry ring, one
  16 KiB kernel page per ring slot for RunCompute and its microsequence, a
  queue stamp and a firmware stamp.
- `agx_shader_scan`: decodes the code, finds the highest register half (for
  the USC Registers word), whether threadgroup memory is used, and the end of
  the code. Undecodable code is refused.
- `agx_compute_submit`: code, USC words, uniforms and CDM stream in one
  GPU-read-only block of the job's VM; RunCompute and microsequence in the
  ring slot; ring entry, then `cpu_wptr`, then the doorbell message through a
  submit hook (the simulator in tests, the RTKit channel on hardware).
- `agx_compute_done`: the stamp, compared with wraparound.

What the kernel port still needs (not in this step): freeing job memory when
its stamp lands (the bump allocator never frees), a real RTKit channel for
the doorbell and for firmware events, the firmware's InitData and boot, TLB
invalidation after unmap, and the event slot handling the firmware performs on
completion. Those are in docs/AGX_FIRMWARE_INTERFACE.md and need the trace.

## 7. Not modelled, and the experiment for each

| Item | Status | Resolves it |
|---|---|---|
| Firmware boot, InitData, RTKit mailbox and channel rings | UNKNOWN in the model by design; layouts INFERRED in docs/AGX_FIRMWARE_INTERFACE.md | The attended trace (docs/MACBOOK_AGX_TRACE_PROCEDURE.md) |
| Which RunCompute and microsequence fields the firmware actually reads | UNKNOWN. The model reads the fields drm/asahi fills that a compute job's outcome depends on, and checks StartCompute/FinalizeCompute agree on queue and slot. | Trace rows U3 and U4: diff macOS's RunCompute against ours |
| Whether the hardware enforces read-only and firmware-only mappings the way the model does | UNKNOWN (INFERRED from drm/asahi's protection names) | Trace row U2, then a deliberate store to a GPU read-only buffer on the chip |
| Timestamps, power states, preemption, context switch, TLB maintenance | not modelled | Kernel port, after the trace |
| Instructions outside the 28 the model implements (`isub`, `imsub`, `mov`, `fmul`, `fcsel` and `while_fcmp` are implemented but not run by the corpus) | refused at run time | Extend with a corpus kernel that uses them, compared against `make gpu-ref` |
| Hardware NaN and denormal behaviour, the `n` float conditions | not modelled / refused | Metal kernels with NaN and denormal inputs under `make gpu-ref` |

## 8. Sources and licences

No code was copied from any of these. The model and driver are AienOS's own C;
the sources supplied layouts, encodings and semantics, cited at each use.

- Mesa `src/asahi/compiler/AGX2.xml` and `src/asahi/genxml/cmdbuf.xml` at
  ba3dacb9, MIT: instruction encodings, CDM and USC word layouts.
- drm/asahi (Asahi Linux kernel driver, GPL-2.0-only OR MIT, used under the
  MIT option) at 94fb23346d52: UAT format, firmware structure layouts and
  offsets (docs/AGX_FW_LAYOUT_G14_V13_5.md).
- dougallj `applegpu` at 28dd8046, BSD-3-Clause, "Copyright (c) 2021, Dougall
  Johnson": execution-mask and branch pseudocode, re-expressed in C. Its
  licence text is in that repository's `LICENSE`.
