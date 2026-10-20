# Third-party attributions for native/gpu/agx

## Mesa: src/asahi/isa (MIT)

Source: https://gitlab.freedesktop.org/mesa/mesa, commit
`ba3dacb9832bd073198d22c402f4e0d75ec3a09a`, files `src/asahi/isa/AGX2.xml`,
`src/asahi/isa/isa.py`, `src/asahi/isa/test/disasm.py`,
`src/asahi/isa/test/cases.txt`. Copyright 2025 Valve Corporation.
SPDX-License-Identifier: MIT.

Used for:

- `agx_isa_tables.c` is generated from `AGX2.xml` through Mesa's own
  `isa.py` parser (instruction patterns, field bit positions, operand
  encodings, enums). Entries marked `.aienos = 1` carry AienOS additions
  observed on Apple hardware; see `docs/AGX_ISA_NOTES.md`.
- `agx_isa.c` operand resolution and `agx_text.c` printing are C ports of
  `test/disasm.py` (`map_value`, `map_mod`, `operand`,
  `decode_float_immediate`), so the text form matches Mesa's disassembler.
- `tests/fixtures/mesa_cases.txt` is a copy of `test/cases.txt` with a
  two-line provenance header.

MIT License text (as for SPDX `MIT`):

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice (including the next
    paragraph) shall be included in all copies or substantial portions of the
    Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

## dougallj/applegpu (BSD-3-Clause): cross-check only

Source: https://github.com/dougallj/applegpu, commit `28dd8046`. No applegpu
code or table is included here. Its disassembler was run in a scratch
directory as an independent oracle for instruction boundaries and opcode
names (results in `docs/AGX_ISA_NOTES.md`), and its `compiler_explorer`
helpers (Metal API runtime compile, archive extraction) produced the corpus
binaries.

## Corpus binaries

`tests/fixtures/corpus/*.bin` are Apple's Metal compiler output for the
AienOS-written kernels next to them (`*.metal`), compiled at runtime through
the public Metal API on a MacBook Air (Mac14,2, Apple M2, GPU G14G),
macOS 26.6.2 (25G83), GPUCompiler.framework 32023. They are test data: raw
machine code of our own shaders, used only to check the decoder.
