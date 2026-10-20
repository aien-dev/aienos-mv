# AGX ISA test fixtures

- `corpus/<name>.metal`: AienOS-written compute kernels (vector add, matmul
  variants, reductions, conversions, bit ops, SIMD-group ops, atomics, math).
- `corpus/<name>.bin`: the main compute shader machine code Apple's compiler
  produced for that kernel. Produced on a MacBook Air (Mac14,2, M2, GPU G14G),
  macOS 26.6.2 (25G83), GPUCompiler.framework 32023, by compiling the source
  at runtime through the Metal API (`newLibraryWithSource`,
  `MTLBinaryArchive` `addComputePipelineFunctionsWithDescriptor`, `serializeToURL`) and extracting
  the main shader from the archive (`applegpu` compiler-explorer tools, run in
  scratch). Trailing `0800` padding is part of the file; the tests trim it.
- `corpus.dis`: expected disassembly in Mesa's syntax. For every instruction
  Mesa's own disassembler decodes (467 of 622 at Mesa ba3dacb9), the line is
  byte-for-byte Mesa's text. The other 155 follow an lload/lstore or the
  bit-26 store that Mesa rejects (Mesa stops at the first one); they use the
  AienOS additions described in `docs/AGX_ISA_NOTES.md`.
- `mesa_cases.txt`: Mesa's `src/asahi/isa/test/cases.txt` (MIT, see
  `../../THIRD_PARTY_NOTICES.md`).

To add a shader: compile it on Apple Silicon macOS the same way, append its
`agx_dis` output to `corpus.dis`, review every line that Mesa does not
decode, and run `make test`.
