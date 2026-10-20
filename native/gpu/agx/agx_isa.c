/*
 * AGX instruction decode/encode over the generated tables. Freestanding.
 * Semantics follow Mesa's src/asahi/isa/isa.py and test/disasm.py (MIT):
 * first table entry whose opcode bits match wins (table order is AGX2.xml
 * order, aliases first); the length comes from the entry's length bit.
 */
#include "agx_isa.h"

static int str_eq(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static unsigned bit_at(const uint8_t *buf, unsigned bit)
{
	return (buf[bit >> 3] >> (bit & 7)) & 1u;
}

static void set_bit(uint8_t *buf, unsigned bit, unsigned v)
{
	uint8_t m = (uint8_t)(1u << (bit & 7));
	if (v)
		buf[bit >> 3] |= m;
	else
		buf[bit >> 3] &= (uint8_t)~m;
}

unsigned agx_bits_size(const struct agx_bits *b)
{
	unsigned n = 0;
	for (unsigned i = 0; i < b->n; i++)
		n += b->r[i].size;
	return n;
}

uint64_t agx_bits_extract(const uint8_t *buf, const struct agx_bits *b)
{
	uint64_t v = 0;
	unsigned o = 0;
	for (unsigned i = 0; i < b->n; i++)
		for (unsigned j = 0; j < b->r[i].size; j++, o++)
			v |= (uint64_t)bit_at(buf, b->r[i].start + j) << o;
	return v;
}

void agx_bits_deposit(uint8_t *buf, const struct agx_bits *b, uint64_t v)
{
	unsigned o = 0;
	for (unsigned i = 0; i < b->n; i++)
		for (unsigned j = 0; j < b->r[i].size; j++, o++)
			set_bit(buf, b->r[i].start + j, (unsigned)((v >> o) & 1u));
}

/* Field bits relative to a 64-bit operand value (operand cases). */
static uint64_t val_extract(uint64_t x, const struct agx_bits *b)
{
	uint64_t v = 0;
	unsigned o = 0;
	for (unsigned i = 0; i < b->n; i++) {
		uint64_t m = b->r[i].size >= 64 ? ~0ull : ((1ull << b->r[i].size) - 1);
		v |= ((x >> b->r[i].start) & m) << o;
		o += b->r[i].size;
	}
	return v;
}

static void words_to_bytes(const uint64_t w[2], uint8_t out[16])
{
	for (unsigned i = 0; i < 16; i++)
		out[i] = (uint8_t)(w[i >> 3] >> ((i & 7) * 8));
}

int agx_find_field(const struct agx_ins_desc *d, const char *name)
{
	for (unsigned i = 0; i < d->nfields; i++)
		if (str_eq(d->fields[i].name, name) && d->fields[i].type == AGX_FT_MOD)
			return (int)i;
	return -1;
}

int agx_find_enum(const char *kind)
{
	for (unsigned i = 0; i < agx_enum_count; i++)
		if (str_eq(agx_enums[i].kind, kind))
			return (int)i;
	return -1;
}

static unsigned popcount64(uint64_t x)
{
	unsigned n = 0;
	while (x) {
		x &= x - 1;
		n++;
	}
	return n;
}

int agx_operand_resolve(int enc, uint64_t v, struct agx_operand *o)
{
	const struct agx_encoding *e = &agx_encodings[enc];
	for (unsigned ci = 0; ci < e->ncases; ci++) {
		const struct agx_case *c = &e->cases[ci];
		if ((v & c->exact_mask) != c->exact_value)
			continue;

		int64_t count = 1, mask = -1, zx = -1, size64 = 0, size32 = 0, hint = -1;
		unsigned cache = 0, abs = 0, neg = 0, sx = 0;
		for (unsigned i = 0; i < c->nprops; i++) {
			const struct agx_prop *p = &c->props[i];
			if (str_eq(p->name, "count"))
				count = p->value;
			else if (str_eq(p->name, "size64"))
				size64 = p->value;
			else if (str_eq(p->name, "size32"))
				size32 = p->value;
		}
		for (unsigned i = 0; i < c->nmods; i++) {
			const struct agx_field *m = &c->mods[i];
			int64_t x = (int64_t)val_extract(v, &m->bits);
			if (str_eq(m->name, "count"))
				count = x;
			else if (str_eq(m->name, "mask"))
				mask = x;
			else if (str_eq(m->name, "zx"))
				zx = x;
			else if (str_eq(m->name, "size64"))
				size64 = x;
			else if (str_eq(m->name, "size32"))
				size32 = x;
			else if (str_eq(m->name, "hint"))
				hint = x;
			else if (str_eq(m->name, "cache"))
				cache = x != 0;
			else if (str_eq(m->name, "abs"))
				abs = x != 0;
			else if (str_eq(m->name, "neg"))
				neg = x != 0;
			else if (str_eq(m->name, "sx"))
				sx = x != 0;
			/* "lu" and others carry no printed meaning */
		}
		if (count == 0)
			count = 4;
		if (mask >= 0)
			count = popcount64((uint64_t)mask);
		if (zx >= 0)
			sx = (unsigned)(zx ^ 1);

		unsigned discard = 0;
		if (hint >= 0) {
			if (hint == 3)
				discard = 1;
			else if (hint == 2)
				cache = 1;
			else if (hint != 1)
				return AGX_E_OPERAND;
		}
		if (cache && discard)
			return AGX_E_OPERAND;

		o->size = size64 ? 64 : size32 ? 32 : 16;
		o->count = (uint8_t)count;
		o->cache = (uint8_t)cache;
		o->discard = (uint8_t)discard;
		o->abs = (uint8_t)abs;
		o->neg = (uint8_t)neg;
		o->sx = (uint8_t)sx;
		o->case_idx = (int16_t)ci;
		o->imm_bits = 0;
		o->base = 0;

		if (!c->has_imm) {
			o->kind = AGX_OPND_NONE;
			return AGX_OK;
		}
		o->base = (int64_t)(val_extract(v, &c->imm.bits) << c->imm.shift);
		o->imm_bits = (uint8_t)agx_bits_size(&c->imm.bits);
		const char *k = c->imm.name;
		if (c->imm.imm_kind == AGX_IMM_SIGNED)
			o->kind = AGX_OPND_SIGNED;
		else if (str_eq(k, "reg"))
			o->kind = AGX_OPND_REG;
		else if (str_eq(k, "uniform"))
			o->kind = AGX_OPND_UNIFORM;
		else if (str_eq(k, "imm"))
			o->kind = AGX_OPND_IMM;
		else if (str_eq(k, "fimm"))
			o->kind = AGX_OPND_FIMM;
		else if (str_eq(k, "cf"))
			o->kind = AGX_OPND_CF;
		else if (str_eq(k, "ts"))
			o->kind = AGX_OPND_TS;
		else if (str_eq(k, "ss"))
			o->kind = AGX_OPND_SS;
		else
			return AGX_E_OPERAND;

		if ((o->kind == AGX_OPND_CF || o->kind == AGX_OPND_TS || o->kind == AGX_OPND_SS) && o->count != 1)
			return AGX_E_OPERAND;
		/* Wide registers start on an even 16-bit half. */
		if ((o->kind == AGX_OPND_REG || o->kind == AGX_OPND_UNIFORM) && o->size != 16 && (o->base & 1))
			return AGX_E_OPERAND;
		return AGX_OK;
	}
	return AGX_E_OPERAND;
}

static unsigned length_index(const struct agx_ins_desc *d, const uint8_t *buf)
{
	if (d->nlen < 2 || d->length_bit < 0)
		return 0;
	return bit_at(buf, (unsigned)d->length_bit);
}

int agx_decode(const uint8_t *buf, size_t avail, struct agx_ins *out)
{
	uint8_t raw[16] = {0};
	for (size_t i = 0; i < avail && i < sizeof(raw); i++)
		raw[i] = buf[i];

	for (unsigned op = 0; op < agx_ins_count; op++) {
		const struct agx_ins_desc *d = &agx_ins_table[op];
		unsigned len = d->len[length_index(d, raw)];
		if (len == 0)
			continue;
		uint8_t r[16] = {0};
		for (unsigned i = 0; i < len && i < 16; i++)
			r[i] = raw[i];
		uint8_t em[16], ev[16], mk[16];
		words_to_bytes(d->exact_mask, em);
		words_to_bytes(d->exact_value, ev);
		words_to_bytes(d->mask, mk);
		int ok = 1;
		for (unsigned i = 0; i < 16; i++)
			if ((r[i] & em[i]) != ev[i])
				ok = 0;
		if (!ok)
			continue;

		out->op = (uint16_t)op;
		out->len = (uint8_t)len;
		if (len > avail)
			return AGX_E_SHORT;
		for (unsigned i = 0; i < 16; i++)
			if (r[i] & (uint8_t)~mk[i])
				return AGX_E_UNKNOWN_BITS;
		for (unsigned f = 0; f < d->nfields; f++) {
			const struct agx_field *fd = &d->fields[f];
			out->val[f] = agx_bits_extract(r, &fd->bits);
			if (fd->type == AGX_FT_DEST || fd->type == AGX_FT_SRC) {
				struct agx_operand o;
				if (agx_operand_resolve(fd->ref, out->val[f], &o) != AGX_OK)
					return AGX_E_OPERAND;
			}
		}
		return (int)len;
	}
	return AGX_E_NO_MATCH;
}

static int encode_full(const struct agx_ins *ins, uint8_t img[16])
{
	const struct agx_ins_desc *d = &agx_ins_table[ins->op];
	words_to_bytes(d->exact_value, img);
	for (unsigned f = 0; f < d->nfields; f++) {
		const struct agx_field *fd = &d->fields[f];
		unsigned sz = agx_bits_size(&fd->bits);
		if (sz < 64 && (ins->val[f] >> sz) != 0)
			return AGX_E_RANGE;
		agx_bits_deposit(img, &fd->bits, ins->val[f]);
	}
	return AGX_OK;
}

int agx_encode(const struct agx_ins *ins, uint8_t *out, size_t cap)
{
	if (ins->op >= agx_ins_count)
		return AGX_E_NO_MATCH;
	const struct agx_ins_desc *d = &agx_ins_table[ins->op];
	uint8_t img[16];
	int rc = encode_full(ins, img);
	if (rc != AGX_OK)
		return rc;
	unsigned len = d->len[length_index(d, img)];
	if (len == 0 || len != ins->len)
		return AGX_E_LENGTH;
	for (unsigned i = len; i < 16; i++)
		if (img[i])
			return AGX_E_LENGTH;
	if (cap < len)
		return AGX_E_SHORT;
	for (unsigned i = 0; i < len; i++)
		out[i] = img[i];
	return (int)len;
}

int agx_choose_length(struct agx_ins *ins)
{
	const struct agx_ins_desc *d = &agx_ins_table[ins->op];
	if (d->nlen < 2 || d->length_bit < 0) {
		ins->len = d->len[0];
		return ins->len;
	}
	int lf = agx_find_field(d, "length");
	if (lf < 0)
		return AGX_E_LENGTH;
	uint8_t img[16];
	ins->val[lf] = 0;
	int rc = encode_full(ins, img);
	if (rc != AGX_OK)
		return rc;
	int fits = 1;
	for (unsigned i = d->len[0]; i < 16; i++)
		if (img[i])
			fits = 0;
	if (fits) {
		ins->len = d->len[0];
		return ins->len;
	}
	ins->val[lf] = 1;
	ins->len = d->len[1];
	return ins->len;
}

uint32_t agx_fimm_to_f32(uint8_t n)
{
	uint32_t sign = (n & 0x80u) ? 0x80000000u : 0;
	unsigned e = (n >> 4) & 7u, f = n & 0xfu;
	uint32_t m;
	int k;
	if (e == 0) {
		m = f;
		k = -6;
	} else {
		m = 0x10u | f;
		k = (int)e - 7;
	}
	if (m == 0)
		return sign;
	int p = 31;
	while (!(m & (1u << p)))
		p--;
	uint32_t exp = (uint32_t)(p + k + 127);
	uint32_t mant = (m << (23 - p)) & 0x7fffffu;
	return sign | (exp << 23) | mant;
}
