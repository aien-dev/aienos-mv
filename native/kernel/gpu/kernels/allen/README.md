# Small Allen on the iMac GPU (L389)

The small Allen is a 0.8-billion-parameter student (Qwen3.5-0.8B architecture, taught by the 35B on the
Spark). These files run it one token at a time on the iMac's Polaris 10 GPU (gfx803) with our own GCN4
kernels. Nothing here comes from an outside GPU library: `ka` assembles the instructions through
`../../gcn_enc.c`, and `../../gcn_interp.c` runs them on the host for testing.

| File | What it is | Floats? |
|---|---|---|
| `allen_kern.c/.h` | the 13 kernels: matvec (bf16 weights), matvec with residual add, three norms (zero-centred RMS, gated RMS, L2), embedding, DeltaNet conv step, gated delta rule step, RoPE, attention with output gate, SwiGLU, copy, argmax | no (kernel side) |
| `allen_plan.c/.h` | where the model and every buffer sit in GPU memory, and the 388 launches that make one token | no (kernel side) |
| `ka.c/.h`, `kmath.c/.h` | the small assembler, and exact f32 maths (exp, divide, sqrt, log1p, SiLU, sigmoid, softplus) built only from IEEE add/mul/fma | no (kernel side) |
| `allen_ref.c/.h`, `fmath_ref.c/.h` | the host mirror: the same IEEE operations in the same order | yes (host test) |
| `allen_test.c` | runs every launch on the interpreter and the mirror side by side and compares the bytes | yes (host test) |
| `math_acc.c` | exhaustive accuracy sweep of the maths against the C library | yes (host test) |

Build and run: `make allen_test`, then `./allen_test MODEL.agp1 IDS.txt NEW [THREADS] [both|interp|mirror]`.
`MODEL.agp1` comes from `allen_train export-gpu <model-dir> <out>` (aien-sovereign-core crate `aien-train-allen`).

## Rules the kernels keep

- Every rounding is IEEE binary32 round-to-nearest-even with denormals kept (`FLOAT_MODE` 0xF0).
  No approximate instruction (`v_exp`, `v_rcp`, `v_rsq`, `v_sqrt`, ...) and no denormal-flushing
  `v_mac`/`v_mad`; the interpreter counts both and the test fails if either runs.
- One wavefront per workgroup, `M0 = -1` before any LDS access, waits before every reuse of a register a
  load or store still owns (the interpreter flags each hazard).

## Proof on the interpreter (2026-10-21, base weights)

Pass rule written before the run (in `allen_kern.h` and `allen_test.c`): after every launch, the work and
state region of the interpreter's memory must equal the mirror's byte for byte (0 ulp). The brief's rule for
the whole model: the generated tokens must equal the host reference (`qwen38_check generate`) on the same
weights.

| Check | Result |
|---|---|
| launches compared, 56 prompt + 32 generated tokens | 33,756, every one byte-identical to the mirror |
| generated tokens vs host reference | 32 of 32 identical |
| top-1 logit vs host reference (reported, not a pass rule) | largest difference 2.2e-5 |
| maths accuracy vs glibc (`math_acc`) | 1 to 4 ulp worst case per routine |
| bytes moved per token (interpreter count) | 1,772,284,358 (1.34 GB in matvec, 0.35 GB in residual matvec, 76 MB DeltaNet state) |

Evidence: `evidence/small-allen-gcn-20261021/` at the repository root.

## Not done yet

- Hardware: needs bundle 1 booted on the iMac and the bench live swap (no firmware reboots). Read-only first:
  VRAM size and memory clock, then tok/s with a calc receipt against the bandwidth read from the chip.
- The trained (Drake-data) student is private and is never committed; only base-weight evidence lives here.
