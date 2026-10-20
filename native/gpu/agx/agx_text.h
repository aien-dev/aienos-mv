/*
 * AGX assembler and disassembler text form (hosted).
 *
 * Syntax is Mesa's disassembler syntax (src/asahi/isa/test/disasm.py, MIT):
 *   "<mnemonic padded to 8> dest, srcs..., immediates..., modifiers..."
 * Registers: r5 (32-bit), r5l / r5h (16-bit halves), dr4 (64-bit), u2 uniform,
 * r0_r1 (consecutive), prefix ^ = last use (discard), $ = keep in cache,
 * suffix .abs .neg .sx. Our printer must equal Mesa's byte for byte on every
 * instruction Mesa decodes; tests/agx_isa_test.c checks that against fixtures.
 */
#ifndef AIENOS_AGX_TEXT_H
#define AIENOS_AGX_TEXT_H

#include <stddef.h>
#include "agx_isa.h"

/* Disassemble one decoded instruction. Returns chars written (excl. NUL) or <0. */
int agx_print(const struct agx_ins *ins, char *buf, size_t cap);

/* Print one operand (raw field value through its encoding). */
int agx_print_operand(int enc, uint64_t v, char *buf, size_t cap);

/* Assemble one line. Picks the shortest encoding; the result is verified by
 * decoding it and printing it back to the same tokens. Returns length in
 * bytes or <0 (AGX_E_NO_MATCH when the text cannot be assembled). */
int agx_assemble(const char *line, uint8_t *out, size_t cap);

#endif
