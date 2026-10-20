/*
 * Apple GPU (AGX, G13/G14 family) shader instruction encoder/decoder.
 *
 * Table-driven: every instruction is an exact opcode pattern plus fields
 * (destinations, sources, immediates, modifiers) at fixed bit positions, and
 * every operand field is decoded through an "encoding" with cases (register,
 * uniform, immediate, ...). The tables in agx_isa_tables.c are generated from
 * Mesa's src/asahi/isa/AGX2.xml (MIT, Copyright 2025 Valve Corporation; see
 * THIRD_PARTY_NOTICES.md) plus AienOS additions for encodings Apple's own
 * compiler emits that AGX2.xml does not describe (marked "aienos" in the
 * table and listed in docs/AGX_ISA_NOTES.md).
 *
 * This file and agx_isa.c are freestanding: no heap, no clock, no I/O. The
 * text form (assembler and disassembler) lives in agx_text.c (hosted).
 */
#ifndef AIENOS_AGX_ISA_H
#define AIENOS_AGX_ISA_H

#include <stddef.h>
#include <stdint.h>

#define AGX_MAX_INS_BYTES 12
#define AGX_MAX_RANGES 6
#define AGX_MAX_FIELDS 24

/* A field's bits: ranges listed low-order first (bit 0 of the value comes
 * from r[0].start). Negative XML positions are already resolved against the
 * long form of the instruction. */
struct agx_range {
	uint8_t start;
	uint8_t size;
};

struct agx_bits {
	uint8_t n;
	struct agx_range r[AGX_MAX_RANGES];
};

struct agx_enum_value {
	uint8_t value;
	const char *name;  /* NULL: prints nothing */
	uint8_t is_default; /* default values print nothing */
};

struct agx_enum {
	const char *kind;
	uint8_t n;
	const struct agx_enum_value *values;
};

enum agx_field_type {
	AGX_FT_DEST = 0,
	AGX_FT_SRC = 1,
	AGX_FT_IMM = 2,
	AGX_FT_MOD = 3,
};

enum agx_imm_kind {
	AGX_IMM_INT = 0,
	AGX_IMM_SIGNED = 1,
	AGX_IMM_ENUM = 2,
};

struct agx_field {
	uint8_t type;      /* enum agx_field_type */
	uint8_t imm_kind;  /* IMM only: enum agx_imm_kind */
	uint8_t shift;     /* immediates inside operand cases */
	int16_t ref;       /* DEST/SRC: encoding index; IMM/MOD: enum index or -1 */
	const char *name;  /* DEST/SRC: encoding name; IMM/MOD: field name */
	const char *display; /* MOD: printed name of a 1-bit modifier */
	struct agx_bits bits;
};

struct agx_prop {
	const char *name; /* size16, size32, size64, count */
	int32_t value;
};

struct agx_case {
	uint64_t exact_mask;
	uint64_t exact_value;
	uint8_t has_imm;
	struct agx_field imm; /* name: reg, uniform, imm, fimm, cf, ts, ss */
	uint8_t nmods;
	const struct agx_field *mods;
	uint8_t nprops;
	const struct agx_prop *props;
};

struct agx_encoding {
	const char *name;
	uint8_t ncases;
	const struct agx_case *cases;
};

struct agx_ins_desc {
	const char *name;
	const char *display;
	uint8_t nlen;
	uint8_t len[2];
	int8_t length_bit; /* -1: fixed length */
	uint64_t exact_mask[2];
	uint64_t exact_value[2];
	uint64_t mask[2]; /* every bit the description accounts for */
	uint8_t nfields;   /* order: dests, sources, immediates, modifiers */
	const struct agx_field *fields;
	uint8_t aienos;    /* 1: entry or fields added by AienOS beyond AGX2.xml */
};

extern const struct agx_ins_desc agx_ins_table[];
extern const unsigned agx_ins_count;
extern const struct agx_encoding agx_encodings[];
extern const unsigned agx_encoding_count;
extern const struct agx_enum agx_enums[];
extern const unsigned agx_enum_count;

/* One decoded instruction: which table entry, its length, and the raw value
 * of every field (operand fields hold the operand's own bits). Encoding the
 * same struct reproduces the bytes exactly. */
struct agx_ins {
	uint16_t op;
	uint8_t len;
	uint64_t val[AGX_MAX_FIELDS];
};

enum agx_err {
	AGX_OK = 0,
	AGX_E_SHORT = -1,        /* buffer ends inside the instruction */
	AGX_E_NO_MATCH = -2,     /* no table entry matches the opcode bits */
	AGX_E_UNKNOWN_BITS = -3, /* matched, but bits outside every field are set */
	AGX_E_OPERAND = -4,      /* an operand matches no case of its encoding */
	AGX_E_RANGE = -5,        /* encode: a field value does not fit its bits */
	AGX_E_LENGTH = -6,       /* encode: bits set beyond the chosen length */
};

/* Decode one instruction. Returns its length in bytes (>0) or an agx_err.
 * Strict: unknown set bits and unresolvable operands are errors. When the
 * opcode matched, *out->op is set even on error (for coverage reports). */
int agx_decode(const uint8_t *buf, size_t avail, struct agx_ins *out);

/* Encode. Returns bytes written or an agx_err. ins->len selects the form. */
int agx_encode(const struct agx_ins *ins, uint8_t *out, size_t cap);

/* Set the length of ins to the shortest form whose extension bits are zero
 * (the length-bit field is updated). Returns the length or an agx_err. */
int agx_choose_length(struct agx_ins *ins);

/* Operand semantics, shared by the text layer and the simulator. */
enum agx_opnd_kind {
	AGX_OPND_NONE = 0, /* case with no value (operand absent) */
	AGX_OPND_REG,
	AGX_OPND_UNIFORM,
	AGX_OPND_IMM,
	AGX_OPND_SIGNED,
	AGX_OPND_FIMM,     /* 8-bit float immediate */
	AGX_OPND_CF,
	AGX_OPND_TS,
	AGX_OPND_SS,
};

struct agx_operand {
	uint8_t kind;    /* enum agx_opnd_kind */
	uint8_t size;    /* 16, 32 or 64 (registers) */
	uint8_t count;   /* consecutive registers */
	uint8_t cache;   /* $ hint */
	uint8_t discard; /* ^ hint */
	uint8_t abs, neg, sx;
	uint8_t imm_bits; /* width of the immediate (for SIGNED) */
	int64_t base;    /* register number in 16-bit halves, or the immediate */
	int16_t case_idx;
};

/* Resolve the raw bits of an operand field. Returns AGX_OK or AGX_E_OPERAND
 * (no case matches, invalid hint, or a misaligned wide register). */
int agx_operand_resolve(int enc, uint64_t v, struct agx_operand *o);

/* Value of a field's bits within an instruction byte buffer. */
uint64_t agx_bits_extract(const uint8_t *buf, const struct agx_bits *b);
void agx_bits_deposit(uint8_t *buf, const struct agx_bits *b, uint64_t v);
unsigned agx_bits_size(const struct agx_bits *b);

/* Value of a float immediate (AGX 8-bit minifloat), as a signed fraction
 * num / 64 * 2^exp; returned as f32 bits. */
uint32_t agx_fimm_to_f32(uint8_t n);

/* Lookups. */
int agx_find_field(const struct agx_ins_desc *d, const char *name);
int agx_find_enum(const char *kind);

#endif
