# Apple GPU (AGX) shader ISA: what AienOS decodes, and how we know

Ledger L214, step 2. Machine: MacBook Air Mac14,2, Apple M2 (T8112), GPU
G14G with 10 cores, macOS 26.6.2 (25G83), GPUCompiler.framework 32023.
Code: `native/gpu/agx/`. Prior art and licences: `docs/AGX_PRIOR_ART.md`.

Every claim is tagged:
- **OBSERVED**: measured on this machine, or the output of a test in this repo.
- **INFERRED**: taken from a named source, with its version.
- **UNKNOWN**: not settled. Each UNKNOWN row names the experiment that settles it.

## What is in `native/gpu/agx`

| Part | What it does | Hosted or freestanding |
|---|---|---|
| `agx_isa_tables.c` | Instruction patterns, field bits, operand encodings and enums. Generated from Mesa `AGX2.xml` at `ba3dacb9` through Mesa's own `isa.py`, plus the AienOS additions below. Covers 136 instruction entries, a superset of what vector add and matmul need. | freestanding data |
| `agx_isa.c` | `agx_decode` / `agx_encode` / `agx_choose_length` / `agx_operand_resolve`. Strict mode: unknown set bits and unresolvable operands are errors. | freestanding (`make lib-checks`: no heap, clock or I/O) |
| `agx_text.c` | `agx_print` (Mesa disassembler syntax, byte-identical to Mesa's `test/disasm.py`) and `agx_assemble` (text to bytes; result verified by decoding and printing it back). | hosted |
| `tools/agx_dis`, `tools/agx_as` | CLIs. `agx_as -c` checks a disassembly listing round trip. | hosted |
| `tests/agx_isa_test.c` | Round-trip, Mesa-vector, negative and coverage tests (`make test`, marker `AIENOS_AGX_NATIVE: PASS`). | hosted |

The generator (`gen_tables.py`, Python) is not in the repo: AienOS carries no
Python dependency. It runs Mesa's parser in a scratch checkout. The generated C
is committed with a header naming the Mesa commit. The generator is kept with
the L214 handoff (`~/handoffs-mac/l214-artifacts/gen_tables.py`). To
regenerate:
`python3 -I gen_tables.py <mesa>/src/asahi/isa ba3dacb9832bd073198d22c402f4e0d75ec3a09a > native/gpu/agx/agx_isa_tables.c`.

## Method (black box, nothing installed system-wide)

1. Only Command Line Tools are on this Mac, so there is no `metal` CLI and the
   Metal toolchain component cannot be installed (`xcodebuild` is absent).
   **OBSERVED.**
2. Each kernel is compiled at runtime through the public Metal API. Our build
   of applegpu's `metal-compile-tool` runs `newLibraryWithSource`, then
   `MTLBinaryArchive addComputePipelineFunctionsWithDescriptor`, then
   `serializeToURL`. `metal-archive-extractor` then pulls the `applegpu_g14g`
   main compute shader out of the archive. **OBSERVED.**
3. Three disassemblers read the same bytes:
   - ours;
   - Mesa's Python reference (`src/asahi/isa/test/disasm.py`, ba3dacb9);
   - applegpu's (`disassemble.py`, 28dd8046).

   Mesa's and applegpu's run in scratch only.

## Results (all OBSERVED, `make test` output)

Corpus: 18 kernels, 622 instructions (`tests/fixtures/corpus/`).

| Check | Result |
|---|---|
| Compiler bytes to decode to encode | 622/622 byte-identical |
| Compiler bytes to our text to assemble | 622/622 byte-identical (the brief's "compiler output to disassembler to re-assembled to identical bytes") |
| Our text to assemble to disassemble | 622/622 identical text (the brief's "assemble to disassemble to identical") |
| Our text vs Mesa's text, where Mesa decodes | 467/467 identical. Mesa stops at the first lload/lstore in 2 kernels and cannot decode the remaining 155 instructions. |
| Instruction boundaries vs applegpu | 622/622 identical offsets across all 18 kernels |
| Opcode names vs applegpu | Every pair is consistent. Example: our `mov` is applegpu `get_sr` when the source is a special register. Full pair table in the L214 handoff. |
| Mesa test vectors (`cases.txt`, 555) | 555/555 decode, print Mesa's text, and re-encode byte-identically |
| Mesa test vectors through the assembler | 533 byte-identical. The other 22 are equivalent encodings: 21 are Mesa using a longer form than needed (our assembler picks the shortest), and 1 is `load ..., 4095, ...`, where a signed and a zero-extended index print the same. |

### Coverage: distinct opcodes the compiler emitted

| Measure | Value |
|---|---|
| Distinct table entries in the corpus | 60 |
| Decoded strictly by AienOS | **60/60** (622/622 instructions) |
| Decodable with Mesa's AGX2.xml alone | 58/60 opcodes fully, plus `store` in all but 1 instance. 584/622 instructions. |
| Distinct applegpu mnemonics in the corpus | 61, all matched to one of our 60 entries |

The 60 entries are:
- integer: `ldimm16` `ldimm32` `mfsr` `iadd` `isub` `imul` `imadd`;
- conversion: `f_to_u32` `f_to_s32` `u32_to_f` `s32_to_f`;
- bit and shift: `shr` `bfi` `bfeil` `extr` `asr`;
- float: `ffma` `hfma` `fadd` `fmul`;
- logic and moves: `mov` `not` `and` `andn1` `xor` `or` `bitrev` `popcnt` `ffs`;
- float functions: `floor` `rsqrts` `rcp` `rsqrt` `sinr` `sinc` `log2` `exp2` `ceil` `trunc` `rint`;
- control flow: `stop` `jmp_any` `jmp_none` `pop_exec` `if_icmp` `while_icmp` `if_fcmp`;
- selects and SIMD: `icsel` `fcsel` `icmp_ballot` `fcmp_ballot` `shuffle`;
- memory and sync: `wait` `load` `atomic` `store` `lload` `lstore` `barrier` `simd`.

### The subset vector add and matmul need

These are OBSERVED in Apple's output for `vadd`, `vadd_i32`, `vadd_bounds`,
`matmul_naive`, `matmul_fixed`, `matmul_tiled` and `matmul_half`. There are 22
entries:
- moves and immediates: `mfsr` (thread position), `ldimm16`, `ldimm32`;
- integer and float arithmetic: `iadd` `imul` `imadd` `fadd` `ffma`;
- select: `icsel`;
- control flow: `if_icmp` `if_fcmp` `while_icmp` `jmp_any` `jmp_none` `pop_exec` `stop`;
- memory: `wait` `load` `store`;
- threadgroup tile (`matmul_tiled` only): `lload` `lstore` `barrier`.

The simulator (step 5) executes this subset.

The smallest program, `vadd` (7 instructions, 38 bytes), Apple's bytes:

```
   0: 72051004             mov      r1, thread_in_grid_x
   4: 0501200e00c01200     load     r0, du0, r1, i32, x, a
   c: 0511240e00c01200     load     r2, du2, r1, i32, x, a
  14: 3800                 wait     a
  16: 2a81c4022c00         fadd     r0, ^r2, ^r0
  1c: 4501280e00c01200     store    r0, du4, r1, i32, x, a
  24: 8800                 stop
```

`du0`, `du2` and `du4` are 64-bit uniform registers u0_u1, u2_u3 and u4_u5,
holding the three buffer addresses. Which uniform holds which buffer is set
by the driver's uniform upload; see `docs/AGX_FIRMWARE_INTERFACE.md`.

## AienOS additions: encodings Apple emits that AGX2.xml does not describe

| # | Instruction, bits | Observed values | Meaning | Status | Experiment that resolves it |
|---|---|---|---|---|---|
| A1 | `lload`/`lstore` bits 40:42 (AienOS modifier `tgu`, printed `tg1`..`tg7`) | 37 instances: 1 (30), 4 (6), 2 (1). Value 1 appears with an index offset of 0 or a negative byte offset (`-4` in `matmul_tiled`). Values 4 and 2 appear only with positive constant offsets (32, 16, 8, 4, 2, 1) in `reduce`. | Hypothesis: a one-hot scale (x1, x2, x4) applied to the constant index offset. | UNKNOWN (hypothesis INFERRED from the value pattern above) | Patch one `lload` in `reduce`'s archive from `tg4` to `tg1`, keeping the offset, run it on the GPU through Metal (applegpu `hwtestbed`/`metallib_replacer` style, user space, no reboot), and compare which threadgroup element is read. Repeat with `tg2`. |
| A2 | `lload`/`lstore` bit 47 (AienOS modifier `tg47`) | Set in all 37 instances, in both the 6- and 8-byte forms (25 and 12). Mesa's encoder never sets it, and its vectors leave it clear. | Unknown. Apple sets it unconditionally. | UNKNOWN | Clear bit 47 in a patched `reduce` and run as in A1. If the result is unchanged, the bit is a hint (cache or ordering); otherwise compare against the outputs. |
| A3 | `load`/`store` bit 26 (AienOS enum `b26`, printed `b26clr` when 0) | 0 in 1 of 59 device memory ops: `store r1, du2, r2, i32, x, a, b26clr` in `reduce` (the per-threadgroup result store, base in uniforms, index `threadgroup_in_grid_x`). 1 in the other 58. Mesa and applegpu both treat it as a constant 1. | Unknown. The candidate reading is that it relates to the index operand's interpretation, which is the only difference from the other stores. | UNKNOWN | Assemble `store` with bit 26 = 0 and = 1 and the same operands, then run both with index values above 2^31 and below 0. A difference in the written address settles it. Same user-space path as A1. |

Without A1 to A3, the strict decoder rejects those instructions. With them,
each bit is carried through decode, print, assemble and encode unchanged, so
they never silently lose information. The simulator (step 5) refuses to
execute A1 or A2 values it has no semantics for, rather than guess. The one
exception is A1 = 1 with A2 set, which it treats as a plain byte offset
(INFERRED from `matmul_tiled`). Step 5 checks that inference by comparing the
simulator's `matmul_tiled` output with the real GPU's output.

## Known limits

- The text form does not record the encoding length. The assembler always
  picks the shortest form; compiler output in the corpus always uses the
  shortest form, so byte identity holds. **OBSERVED.**
- All 136 entries come from Mesa's XML (4 of them, `load`, `store`, `lload`
  and `lstore`, carry the AienOS additions) and are only as right as Mesa is. Our tests prove agreement with Mesa's own disassembler and vectors, and
  with Apple's compiler on the corpus. They do not prove the semantics of
  opcodes the corpus never exercises. **INFERRED** (Mesa ba3dacb9).
- G14 (M2) is assumed to share the G13 (M1) shader ISA. Mesa's AGX2.xml makes
  that assumption, and every instruction Apple emitted here decodes under it.
  **OBSERVED** for the 60 opcodes in the corpus. UNKNOWN beyond them.
