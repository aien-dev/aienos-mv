# Third-party attributions for native/kernel/gpu

## AMD GCN3 ISA Reference

Source: AMD "Graphics Core Next Architecture, Generation 3" ISA Reference Guide
(publicly available from AMD developers.amd.com).

Used for: instruction encoding formats and opcode tables in gcn_enc.h / gcn_enc.c.
The encoding tables are independently implemented; no AMD code is included.

## Linux amdgpu driver register definitions

The following register offsets and PM4 opcode values in pm4.h are derived from
the Linux amdgpu driver source code, which is licensed under the MIT License:

  drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_0_d.h
  drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_0_sh_mask.h
  drivers/gpu/drm/amd/amdgpu/amdgpu_ring.h  (PM4 IT_OPCODE values)

Copyright AMD. Licensed under the MIT License (see below).

Registers cited:
  mmCOMPUTE_PGM_LO, mmCOMPUTE_PGM_HI, mmCOMPUTE_PGM_RSRC1/2
  mmCOMPUTE_USER_DATA_0, mmCOMPUTE_NUM_THREAD_X/Y/Z
  mmCOMPUTE_START_X/Y/Z

PM4 opcodes:
  PM4_IT_SET_SH_REG (0x76), PM4_IT_DISPATCH_DIRECT (0x15),
  PM4_IT_ACQUIRE_MEM (0x58), PM4_IT_RELEASE_MEM (0x49),
  PM4_IT_WRITE_DATA (0x37), PM4_IT_NOP (0x10)

MIT License text:
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.

## Oracle tool (test-time only, not in build)

llvm-mc-18 (LLVM AMDGPU backend) is used as an oracle in gcn_enc_test.c to
verify byte-exact instruction encodings.  It is installed on the Spark under
/usr/bin/llvm-mc-18 via the system package manager and is never compiled into
the repo build.  LLVM is licensed under the Apache License 2.0 with LLVM
Exceptions.
