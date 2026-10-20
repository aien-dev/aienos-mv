/*
 * AGX text form. Printing is a port of Mesa src/asahi/isa/test/disasm.py
 * (MIT, Copyright 2025 Valve Corporation): map_value, map_mod, operand,
 * decode_float_immediate. Assembling inverts the printer by search and
 * accepts only results that print back to the input.
 */
#include "agx_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sbuf {
	char *p;
	size_t cap, n;
	int err;
};

static void sb_put(struct sbuf *s, const char *t)
{
	size_t l = strlen(t);
	if (s->n + l + 1 > s->cap) {
		s->err = 1;
		return;
	}
	memcpy(s->p + s->n, t, l + 1);
	s->n += l;
}

/* Python str() of the AGX minifloat: exact decimal, at least one fraction digit. */
static void fimm_str(uint8_t n, char *out, size_t cap)
{
	int neg = (n & 0x80) != 0;
	unsigned e = (n >> 4) & 7u, f = n & 0xfu;
	/* value = m * 2^k, k in [-6, 0]; scale to millionths (exact). */
	unsigned long long m = e == 0 ? f : (0x10u | f);
	int k = e == 0 ? -6 : (int)e - 7;
	unsigned long long micro = m * 1000000ull;
	for (int i = 0; i < -k; i++)
		micro /= 2;
	if (k > 0)
		micro <<= k;
	unsigned long long ip = micro / 1000000ull, fp = micro % 1000000ull;
	char frac[8];
	snprintf(frac, sizeof(frac), "%06llu", fp);
	int l = 6;
	while (l > 1 && frac[l - 1] == '0')
		frac[--l] = 0;
	snprintf(out, cap, "%s%llu.%s", neg ? "-" : "", ip, frac);
}

/* map_value: NULL means "prints nothing". */
static const char *map_value(uint64_t v, int enum_ref, char *tmp, size_t cap)
{
	const struct agx_enum *e = &agx_enums[enum_ref];
	for (unsigned i = 0; i < e->n; i++)
		if (e->values[i].value == v)
			return e->values[i].is_default ? NULL : e->values[i].name;
	snprintf(tmp, cap, "unk%llu", (unsigned long long)v);
	return tmp;
}

static void signed_str(uint64_t v, unsigned bits, char *out, size_t cap)
{
	if (bits > 0 && bits < 64 && (v & (1ull << (bits - 1))))
		snprintf(out, cap, "%lld", -(long long)((1ull << bits) - v));
	else
		snprintf(out, cap, "%llu", (unsigned long long)v);
}

int agx_print_operand(int enc, uint64_t v, char *buf, size_t cap)
{
	struct agx_operand o;
	struct sbuf s = {buf, cap, 0, 0};
	char tmp[48];
	if (cap)
		buf[0] = 0;
	if (agx_operand_resolve(enc, v, &o) != AGX_OK)
		return -1;
	if (o.kind == AGX_OPND_NONE)
		return 0;
	char suf[24] = "";
	if (o.abs)
		strcat(suf, ".abs");
	if (o.neg)
		strcat(suf, ".neg");
	if (o.sx)
		strcat(suf, ".sx");
	switch (o.kind) {
	case AGX_OPND_CF:
	case AGX_OPND_TS:
	case AGX_OPND_SS:
		snprintf(tmp, sizeof(tmp), "%s%lld", o.kind == AGX_OPND_CF ? "cf" : o.kind == AGX_OPND_TS ? "ts" : "ss",
			 (long long)o.base);
		sb_put(&s, tmp);
		break;
	case AGX_OPND_FIMM:
		fimm_str((uint8_t)o.base, tmp, sizeof(tmp));
		sb_put(&s, tmp);
		break;
	case AGX_OPND_IMM:
		snprintf(tmp, sizeof(tmp), "%lld", (long long)o.base);
		sb_put(&s, tmp);
		break;
	case AGX_OPND_SIGNED:
		signed_str((uint64_t)o.base, o.imm_bits, tmp, sizeof(tmp));
		sb_put(&s, tmp);
		break;
	case AGX_OPND_REG:
	case AGX_OPND_UNIFORM: {
		char prefix = o.kind == AGX_OPND_REG ? 'r' : 'u';
		sb_put(&s, o.discard ? "^" : o.cache ? "$" : "");
		for (unsigned i = 0; i < o.count; i++) {
			long long reg = o.base + (long long)i * (o.size / 16);
			if (i)
				sb_put(&s, "_");
			if (o.size == 16)
				snprintf(tmp, sizeof(tmp), "%c%lld%c", prefix, reg >> 1, (reg & 1) ? 'h' : 'l');
			else
				snprintf(tmp, sizeof(tmp), "%s%c%lld", o.size == 64 ? "d" : "", prefix, reg >> 1);
			sb_put(&s, tmp);
		}
		break;
	}
	default:
		return -1;
	}
	sb_put(&s, suf);
	return s.err ? -1 : (int)s.n;
}

/* Token for field f, or "" when it prints nothing (sets *absent). */
static int field_token(const struct agx_ins_desc *d, unsigned f, uint64_t v, char *tok, size_t cap, int *absent)
{
	const struct agx_field *fd = &d->fields[f];
	char tmp[48];
	*absent = 0;
	tok[0] = 0;
	switch (fd->type) {
	case AGX_FT_DEST:
	case AGX_FT_SRC: {
		int n = agx_print_operand(fd->ref, v, tok, cap);
		if (n < 0)
			return -1;
		if (n == 0) {
			if (fd->type == AGX_FT_DEST)
				snprintf(tok, cap, "_");
			else
				*absent = 1;
		}
		return 0;
	}
	case AGX_FT_IMM: {
		const char *t;
		if (fd->imm_kind == AGX_IMM_ENUM) {
			t = map_value(v, fd->ref, tmp, sizeof(tmp));
		} else if (fd->imm_kind == AGX_IMM_SIGNED) {
			signed_str(v, agx_bits_size(&fd->bits), tmp, sizeof(tmp));
			t = tmp;
		} else {
			snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
			t = tmp;
		}
		if (!t)
			*absent = 1;
		else
			snprintf(tok, cap, "%s", t);
		return 0;
	}
	case AGX_FT_MOD: {
		if (strcmp(fd->name, "length") == 0) {
			*absent = 1;
			return 0;
		}
		const char *t = fd->ref >= 0 ? map_value(v, fd->ref, tmp, sizeof(tmp)) : (v ? fd->display : NULL);
		if (!t)
			*absent = 1;
		else
			snprintf(tok, cap, "%s", t);
		return 0;
	}
	}
	return -1;
}

int agx_print(const struct agx_ins *ins, char *buf, size_t cap)
{
	const struct agx_ins_desc *d = &agx_ins_table[ins->op];
	struct sbuf s = {buf, cap, 0, 0};
	char tok[96];
	int first = 1;
	if (cap)
		buf[0] = 0;
	/* Python: f'{display:<8}{end}'.strip() with end = ' ' + ', '.join(...) */
	sb_put(&s, d->display);
	for (size_t l = strlen(d->display); l < 8; l++)
		sb_put(&s, " ");
	/* Print in Python's group order: dests, srcs, imms, mods. Table order is
	 * already grouped that way. */
	for (unsigned f = 0; f < d->nfields; f++) {
		int absent;
		if (field_token(d, f, ins->val[f], tok, sizeof(tok), &absent) < 0)
			return -1;
		if (absent)
			continue;
		sb_put(&s, first ? " " : ", ");
		sb_put(&s, tok);
		first = 0;
	}
	if (s.err)
		return -1;
	/* strip trailing spaces (no operands) */
	while (s.n && buf[s.n - 1] == ' ')
		buf[--s.n] = 0;
	return (int)s.n;
}

/* ---------------- assembler ---------------- */

#define MAX_TOK 24
#define MAX_CAND 64

struct asm_state {
	const struct agx_ins_desc *d;
	unsigned op;
	char toks[MAX_TOK][64];
	unsigned ntok;
	struct agx_ins ins;
	uint8_t out[16];
	int outlen;
};

static int split_tokens(const char *line, char *mn, size_t mncap, char toks[MAX_TOK][64], unsigned *ntok)
{
	while (*line == ' ' || *line == '\t')
		line++;
	size_t i = 0;
	while (*line && *line != ' ' && *line != '\t' && *line != '\n') {
		if (i + 1 >= mncap)
			return -1;
		mn[i++] = *line++;
	}
	mn[i] = 0;
	*ntok = 0;
	while (*line) {
		while (*line == ' ' || *line == '\t' || *line == '\n')
			line++;
		if (!*line)
			break;
		const char *end = strchr(line, ',');
		size_t l = end ? (size_t)(end - line) : strlen(line);
		while (l && (line[l - 1] == ' ' || line[l - 1] == '\t' || line[l - 1] == '\n' || line[l - 1] == '\r'))
			l--;
		if (*ntok >= MAX_TOK || l >= 64)
			return -1;
		memcpy(toks[*ntok], line, l);
		toks[*ntok][l] = 0;
		(*ntok)++;
		line = end ? end + 1 : line + strlen(line);
	}
	return 0;
}

static int parse_ll(const char *t, long long *v)
{
	char *e;
	if (!*t)
		return 0;
	*v = strtoll(t, &e, 10);
	return *e == 0;
}

static uint64_t bits_mask(const struct agx_bits *b)
{
	uint64_t m = 0;
	for (unsigned i = 0; i < b->n; i++)
		for (unsigned j = 0; j < b->r[i].size; j++)
			m |= 1ull << (b->r[i].start + j);
	return m;
}

/* Candidate operand values for a token: search over cases, base candidates
 * derived from the token, and every assignment of the case's modifiers and
 * of field bits no case constrains (fields can share instruction bits, e.g.
 * csel's dest size bit is also the top bit of its compare operands, so the
 * free bits must be able to agree with the other field); keep values that
 * print back to exactly the token. */
static unsigned operand_candidates(int enc, unsigned width, const char *tok, uint64_t *cand, unsigned max)
{
	const struct agx_encoding *e = &agx_encodings[enc];
	unsigned n = 0;
	char buf[96];

	/* base candidates from the token text */
	long long bases[300];
	unsigned nb = 0;
	const char *t = tok;
	if (*t == '^' || *t == '$')
		t++;
	char core[64];
	snprintf(core, sizeof(core), "%s", t);
	char *dot = strchr(core, '.');
	/* floats contain a dot too: keep a copy before cutting suffixes */
	char whole[64];
	snprintf(whole, sizeof(whole), "%s", core);
	if (dot && (strcmp(dot, ".abs") == 0 || strncmp(dot, ".abs.", 5) == 0 || strcmp(dot, ".neg") == 0 ||
		    strncmp(dot, ".neg.", 5) == 0 || strcmp(dot, ".sx") == 0))
		*dot = 0;
	char *us = strchr(core, '_');
	if (us)
		*us = 0;
	long long num;
	const char *c = core;
	if (*c == 'd')
		c++;
	if ((*c == 'r' || *c == 'u') && c[1] >= '0' && c[1] <= '9') {
		char *end;
		long long w = strtoll(c + 1, &end, 10);
		if (*end == 'h')
			bases[nb++] = 2 * w + 1;
		else
			bases[nb++] = 2 * w;
	} else if ((strncmp(core, "cf", 2) == 0 || strncmp(core, "ts", 2) == 0 || strncmp(core, "ss", 2) == 0) &&
		   parse_ll(core + 2, &num)) {
		bases[nb++] = num;
	} else if (parse_ll(core, &num)) {
		bases[nb++] = num;
	} else {
		/* float immediate (maybe with suffixes): try all 256 encodings */
		for (unsigned i = 0; i < 256; i++)
			bases[nb++] = i;
	}
	(void)whole;

	for (unsigned ci = 0; ci < e->ncases && n < max; ci++) {
		const struct agx_case *cs = &e->cases[ci];
		if (!cs->has_imm)
			continue;
		unsigned isz = agx_bits_size(&cs->imm.bits);
		unsigned mbits = 0;
		uint64_t used = cs->exact_mask | bits_mask(&cs->imm.bits);
		for (unsigned m = 0; m < cs->nmods; m++) {
			mbits += agx_bits_size(&cs->mods[m].bits);
			used |= bits_mask(&cs->mods[m].bits);
		}
		uint64_t freem = (width >= 64 ? ~0ull : ((1ull << width) - 1)) & ~used;
		unsigned fbits = 0;
		for (uint64_t x = freem; x; x &= x - 1)
			fbits++;
		if (mbits > 12)
			continue;
		if (mbits + fbits > 12) {
			freem = 0; /* too many to search: leave them zero */
			fbits = 0;
		}
		mbits += fbits;
		for (unsigned b = 0; b < nb && n < max; b++) {
			long long base = bases[b];
			uint64_t iv;
			if (cs->imm.imm_kind == AGX_IMM_SIGNED || base < 0)
				iv = (uint64_t)base & (isz >= 64 ? ~0ull : ((1ull << isz) - 1));
			else
				iv = (uint64_t)base;
			if (cs->imm.shift) {
				if (iv & ((1ull << cs->imm.shift) - 1))
					continue;
				iv >>= cs->imm.shift;
			}
			if (isz < 64 && (iv >> isz))
				continue;
			for (uint64_t ma = 0; ma < (1ull << mbits) && n < max; ma++) {
				uint8_t img[8] = {0};
				for (unsigned k = 0; k < 8; k++)
					img[k] = (uint8_t)(cs->exact_value >> (8 * k));
				agx_bits_deposit(img, &cs->imm.bits, iv);
				uint64_t rest = ma;
				for (unsigned m = 0; m < cs->nmods; m++) {
					unsigned sz = agx_bits_size(&cs->mods[m].bits);
					agx_bits_deposit(img, &cs->mods[m].bits, rest & ((1ull << sz) - 1));
					rest >>= sz;
				}
				uint64_t v = 0;
				for (unsigned k = 0; k < 8; k++)
					v |= (uint64_t)img[k] << (8 * k);
				for (uint64_t x = freem; x; x &= x - 1) {
					if (rest & 1)
						v |= x & -x;
					rest >>= 1;
				}
				if ((v & cs->exact_mask) != cs->exact_value)
					continue;
				if (agx_print_operand(enc, v, buf, sizeof(buf)) <= 0 || strcmp(buf, tok) != 0)
					continue;
				int dup = 0;
				for (unsigned q = 0; q < n; q++)
					if (cand[q] == v)
						dup = 1;
				if (!dup)
					cand[n++] = v;
			}
		}
	}
	return n;
}

/* Value of an operand field that prints nothing (case without a value). */
static int operand_absent_value(int enc, uint64_t *v)
{
	const struct agx_encoding *e = &agx_encodings[enc];
	for (unsigned ci = 0; ci < e->ncases; ci++)
		if (!e->cases[ci].has_imm) {
			struct agx_operand o;
			if (agx_operand_resolve(enc, e->cases[ci].exact_value, &o) == AGX_OK && o.kind == AGX_OPND_NONE) {
				*v = e->cases[ci].exact_value;
				return 1;
			}
		}
	return 0;
}

static int verify(struct asm_state *st)
{
	struct agx_ins t = st->ins;
	t.op = (uint16_t)st->op;
	if (agx_choose_length(&t) < 0)
		return 0;
	uint8_t bytes[16];
	int n = agx_encode(&t, bytes, sizeof(bytes));
	if (n <= 0)
		return 0;
	struct agx_ins back;
	if (agx_decode(bytes, (size_t)n, &back) != n)
		return 0;
	char text[256], mn[32], toks[MAX_TOK][64];
	unsigned nt;
	if (agx_print(&back, text, sizeof(text)) < 0)
		return 0;
	if (split_tokens(text, mn, sizeof(mn), toks, &nt) < 0)
		return 0;
	if (strcmp(mn, st->d->display) != 0 || nt != st->ntok)
		return 0;
	for (unsigned i = 0; i < nt; i++)
		if (strcmp(toks[i], st->toks[i]) != 0)
			return 0;
	memcpy(st->out, bytes, (size_t)n);
	st->outlen = n;
	return 1;
}

static int match_fields(struct asm_state *st, unsigned f, unsigned ti)
{
	const struct agx_ins_desc *d = st->d;
	if (f == d->nfields)
		return ti == st->ntok && verify(st);
	const struct agx_field *fd = &d->fields[f];
	const char *tok = ti < st->ntok ? st->toks[ti] : NULL;
	char tmp[48];

	switch (fd->type) {
	case AGX_FT_DEST:
	case AGX_FT_SRC: {
		if (tok && !(fd->type == AGX_FT_DEST && strcmp(tok, "_") == 0)) {
			uint64_t cand[MAX_CAND];
			unsigned nc = operand_candidates(fd->ref, agx_bits_size(&fd->bits), tok, cand, MAX_CAND);
			for (unsigned i = 0; i < nc; i++) {
				st->ins.val[f] = cand[i];
				if (match_fields(st, f + 1, ti + 1))
					return 1;
			}
		}
		uint64_t av;
		if (operand_absent_value(fd->ref, &av)) {
			st->ins.val[f] = av;
			if (fd->type == AGX_FT_DEST) {
				if (tok && strcmp(tok, "_") == 0 && match_fields(st, f + 1, ti + 1))
					return 1;
			} else if (match_fields(st, f + 1, ti)) {
				return 1;
			}
		}
		return 0;
	}
	case AGX_FT_IMM: {
		unsigned sz = agx_bits_size(&fd->bits);
		uint64_t lim = sz >= 64 ? ~0ull : ((1ull << sz) - 1);
		if (fd->imm_kind == AGX_IMM_ENUM) {
			const struct agx_enum *e = &agx_enums[fd->ref];
			for (uint64_t v = 0; v <= lim && v < 256; v++) {
				const char *t = map_value(v, fd->ref, tmp, sizeof(tmp));
				st->ins.val[f] = v;
				if (t && tok && strcmp(t, tok) == 0 && match_fields(st, f + 1, ti + 1))
					return 1;
				if (!t && match_fields(st, f + 1, ti))
					return 1;
			}
			(void)e;
			return 0;
		}
		long long x;
		if (!tok || !parse_ll(tok, &x))
			return 0;
		uint64_t v = (uint64_t)x & lim;
		char back[48];
		if (fd->imm_kind == AGX_IMM_SIGNED)
			signed_str(v, sz, back, sizeof(back));
		else
			snprintf(back, sizeof(back), "%llu", (unsigned long long)v);
		if (strcmp(back, tok) != 0)
			return 0;
		st->ins.val[f] = v;
		return match_fields(st, f + 1, ti + 1);
	}
	case AGX_FT_MOD: {
		if (strcmp(fd->name, "length") == 0) {
			st->ins.val[f] = 0;
			return match_fields(st, f + 1, ti);
		}
		if (fd->ref < 0) {
			if (tok && strcmp(tok, fd->display) == 0) {
				st->ins.val[f] = 1;
				if (match_fields(st, f + 1, ti + 1))
					return 1;
			}
			st->ins.val[f] = 0;
			return match_fields(st, f + 1, ti);
		}
		unsigned sz = agx_bits_size(&fd->bits);
		for (uint64_t v = 0; v < (1ull << sz); v++) {
			const char *t = map_value(v, fd->ref, tmp, sizeof(tmp));
			st->ins.val[f] = v;
			if (t && tok && strcmp(t, tok) == 0 && match_fields(st, f + 1, ti + 1))
				return 1;
		}
		for (uint64_t v = 0; v < (1ull << sz); v++) {
			const char *t = map_value(v, fd->ref, tmp, sizeof(tmp));
			st->ins.val[f] = v;
			if (!t && match_fields(st, f + 1, ti))
				return 1;
		}
		return 0;
	}
	}
	return 0;
}

int agx_assemble(const char *line, uint8_t *out, size_t cap)
{
	static struct asm_state st;
	char mn[32];
	memset(&st, 0, sizeof(st));
	if (split_tokens(line, mn, sizeof(mn), st.toks, &st.ntok) < 0)
		return AGX_E_NO_MATCH;
	for (unsigned op = 0; op < agx_ins_count; op++) {
		const struct agx_ins_desc *d = &agx_ins_table[op];
		if (strcmp(d->display, mn) != 0)
			continue;
		st.d = d;
		st.op = op;
		memset(&st.ins, 0, sizeof(st.ins));
		if (match_fields(&st, 0, 0)) {
			if ((size_t)st.outlen > cap)
				return AGX_E_SHORT;
			memcpy(out, st.out, (size_t)st.outlen);
			return st.outlen;
		}
	}
	return AGX_E_NO_MATCH;
}
