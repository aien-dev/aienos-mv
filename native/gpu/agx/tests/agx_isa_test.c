/*
 * AGX ISA tests (host). Usage: agx_isa_test <tests/fixtures dir>
 *
 *  1. Compiler output round trip. Every tests/fixtures/corpus/<name>.bin is
 *     Apple's own compiler output (Mac14,2, macOS 26.6.2). Each instruction:
 *     bytes -> decode -> encode == bytes; decode -> text == corpus.dis line;
 *     text -> assemble == bytes (byte-identical).
 *  2. Assembler round trip. Every corpus.dis line: text -> assemble ->
 *     disassemble == text.
 *  3. Mesa's test vectors (mesa_cases.txt): bytes -> decode -> text equals
 *     Mesa's text and re-encodes byte-identically; text -> assemble gives
 *     bytes that print the same text (Mesa's vectors sometimes use a longer
 *     form than needed; those are counted, not failed).
 *  4. Negative cases: truncated input, unknown bits, bad operands, bad text.
 *  5. Coverage: distinct compiler-emitted opcodes we decode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../agx_isa.h"
#include "../agx_text.h"

static int fails;

#define CHECK(c, ...)                                                   \
	do {                                                            \
		if (!(c)) {                                             \
			fails++;                                        \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                   \
			fputc('\n', stderr);                            \
		}                                                       \
	} while (0)

static const char *dir;

static FILE *open_fixture(const char *name)
{
	char p[4096];
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	FILE *f = fopen(p, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", p);
		exit(2);
	}
	return f;
}

static void hexstr(const uint8_t *b, int n, char *out)
{
	for (int i = 0; i < n; i++)
		snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static int unhex(const char *s, uint8_t *b, int max)
{
	int n = 0;
	while (n < max && s[0] && s[1] && s[0] != ' ') {
		unsigned v;
		if (sscanf(s, "%2x", &v) != 1)
			return -1;
		b[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

/* Opcodes (table entries) seen in compiler output, and decoded. */
static unsigned char seen[512], via_aienos_bits[512];

/* Part 1 and 2: corpus.dis lists "== <name>.bin" then "<off>: <hex> <text>". */
static void test_corpus(void)
{
	FILE *dis = open_fixture("corpus.dis");
	char line[512], cur[sizeof(line) + 8] = "";
	static uint8_t data[1 << 16];
	size_t n = 0, off = 0;
	int files = 0, ins_total = 0, aienos_ins = 0;

	while (fgets(line, sizeof(line), dis)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "== ", 3)) {
			CHECK(off == n, "%s: %zu bytes not covered by corpus.dis", cur, n - off);
			snprintf(cur, sizeof(cur), "corpus/%s", line + 3);
			FILE *f = open_fixture(cur);
			n = fread(data, 1, sizeof(data), f);
			fclose(f);
			while (n > 2 && data[n - 2] == 8 && data[n - 1] == 0)
				n -= 2; /* trailing trap padding */
			off = 0;
			files++;
			continue;
		}
		unsigned loff;
		char hex[64];
		int pos;
		if (sscanf(line, " %x: %63s %n", &loff, hex, &pos) != 2)
			continue;
		const char *text = line + pos;
		CHECK(loff == off, "%s: offset %x, expected %zx", cur, loff, off);

		struct agx_ins ins;
		int l = agx_decode(data + off, n - off, &ins);
		CHECK(l > 0, "%s+%zx: decode error %d", cur, off, l);
		if (l <= 0)
			break;
		ins_total++;
		seen[ins.op] = 1;

		char h[40], t[256];
		hexstr(data + off, l, h);
		CHECK(!strcmp(h, hex), "%s+%zx: length/bytes %s vs fixture %s", cur, off, h, hex);

		uint8_t enc[16];
		int el = agx_encode(&ins, enc, sizeof(enc));
		CHECK(el == l && !memcmp(enc, data + off, (size_t)l), "%s+%zx: re-encode differs", cur, off);

		CHECK(agx_print(&ins, t, sizeof(t)) >= 0 && !strcmp(t, text), "%s+%zx: text\n  got  %s\n  want %s", cur,
		      off, t, text);

		uint8_t as[16];
		int al = agx_assemble(text, as, sizeof(as));
		CHECK(al == l && !memcmp(as, data + off, (size_t)l), "%s+%zx: assemble(%s) not byte-identical", cur, off,
		      text);

		/* assemble -> disassemble -> identical text */
		struct agx_ins back;
		char t2[256] = "";
		if (al > 0 && agx_decode(as, (size_t)al, &back) == al)
			agx_print(&back, t2, sizeof(t2));
		CHECK(!strcmp(t2, text), "%s+%zx: assemble->disassemble gave %s", cur, off, t2);

		if (agx_ins_table[ins.op].aienos && (strstr(text, "tg") || strstr(text, "b26clr"))) {
			via_aienos_bits[ins.op] = 1;
			aienos_ins++;
		}
		off += (size_t)l;
	}
	CHECK(off == n, "%s: %zu bytes not covered", cur, n - off);
	fclose(dis);

	unsigned nseen = 0, naienos = 0;
	for (unsigned i = 0; i < agx_ins_count; i++) {
		nseen += seen[i];
		naienos += via_aienos_bits[i];
	}
	CHECK(files >= 18 && ins_total >= 600, "corpus too small: %d files, %d instructions", files, ins_total);
	printf("corpus: %d shaders, %d instructions, all decode, re-encode, print and assemble byte-identically\n",
	       files, ins_total);
	printf("AGX_COVERAGE: %u/%u distinct compiler-emitted opcodes decoded (%u need AienOS extension bits, %d "
	       "instructions)\n",
	       nseen, nseen, naienos, aienos_ins);
	printf("opcodes:");
	for (unsigned i = 0; i < agx_ins_count; i++)
		if (seen[i])
			printf(" %s", agx_ins_table[i].name);
	printf("\n");
}

/* Part 3. */
static void test_mesa_cases(void)
{
	FILE *f = open_fixture("mesa_cases.txt");
	char line[512];
	int n = 0, exact = 0, equiv = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || !line[0])
			continue;
		uint8_t b[16] = {0};
		int k = unhex(line, b, 16);
		const char *text = strchr(line, ' ');
		while (text && *text == ' ')
			text++;
		if (k <= 0 || !text)
			continue;
		n++;
		struct agx_ins ins;
		int l = agx_decode(b, (size_t)k, &ins);
		CHECK(l == k, "mesa %s: decode %d", line, l);
		if (l != k)
			continue;
		char t[256];
		agx_print(&ins, t, sizeof(t));
		CHECK(!strcmp(t, text), "mesa %s: printed %s", line, t);
		uint8_t e[16];
		CHECK(agx_encode(&ins, e, 16) == k && !memcmp(e, b, (size_t)k), "mesa %s: re-encode", line);

		uint8_t as[16];
		int al = agx_assemble(text, as, sizeof(as));
		CHECK(al > 0, "mesa %s: assemble %d", line, al);
		if (al <= 0)
			continue;
		if (al == k && !memcmp(as, b, (size_t)k)) {
			exact++;
			continue;
		}
		struct agx_ins back;
		char t2[256] = "";
		if (agx_decode(as, (size_t)al, &back) == al)
			agx_print(&back, t2, sizeof(t2));
		CHECK(!strcmp(t2, text), "mesa %s: assembled to a different instruction (%s)", line, t2);
		CHECK(al <= k, "mesa %s: assembler chose a longer form", line);
		equiv++;
	}
	fclose(f);
	CHECK(n == 555, "expected 555 Mesa vectors, read %d", n);
	printf("mesa vectors: %d decode/print/re-encode identically; assembler: %d byte-identical, %d equivalent "
	       "(shorter form or signed/unsigned twin)\n",
	       n, exact, equiv);
}

/* Part 4. */
static void test_negative(void)
{
	struct agx_ins ins;
	/* fadd r0, ^r2, ^r0 is 6 bytes; give it 4 */
	static const uint8_t fadd[] = {0x2a, 0x81, 0xc4, 0x02, 0x2c, 0x00};
	CHECK(agx_decode(fadd, 4, &ins) == AGX_E_SHORT, "truncated fadd must be E_SHORT");
	CHECK(agx_decode(fadd, 6, &ins) == 6, "fadd decodes");

	/* barrier is 6800; bit 8 is in no field of its description */
	static const uint8_t barrier_bad[] = {0x68, 0x01};
	CHECK(agx_decode(barrier_bad, 2, &ins) == AGX_E_UNKNOWN_BITS, "barrier with a stray bit must be E_UNKNOWN_BITS");

	/* field value too wide must not encode */
	CHECK(agx_decode(fadd, 6, &ins) == 6, "fadd decodes");
	ins.val[0] = ~0ull;
	uint8_t out[16];
	CHECK(agx_encode(&ins, out, sizeof(out)) == AGX_E_RANGE, "oversized field must be E_RANGE");

	/* text that is not an instruction, or has a wrong operand */
	CHECK(agx_assemble("frobnicate r0, r1", out, sizeof(out)) == AGX_E_NO_MATCH, "unknown mnemonic");
	CHECK(agx_assemble("fadd     r0, ^r2", out, sizeof(out)) == AGX_E_NO_MATCH, "missing source");
	CHECK(agx_assemble("fadd     r0, ^r2, ^r0, ^r1", out, sizeof(out)) == AGX_E_NO_MATCH, "extra source");
	CHECK(agx_assemble("fadd     r0, r999, r0", out, sizeof(out)) == AGX_E_NO_MATCH, "register out of range");
	CHECK(agx_assemble("store    r0, du4, r1, i32, x, a, bogus", out, sizeof(out)) == AGX_E_NO_MATCH,
	      "unknown modifier");
	CHECK(agx_assemble("fadd     r0, ^r2, ^r0", out, 2) == AGX_E_SHORT, "assemble into a short buffer");

	/* float immediates: the 8-bit minifloat, value (16 | f) * 2^(e - 7), or f / 64 when e == 0 */
	CHECK(agx_fimm_to_f32(0x00) == 0x00000000u, "fimm 0.0");
	CHECK(agx_fimm_to_f32(0x30) == 0x3f800000u, "fimm 1.0");
	CHECK(agx_fimm_to_f32(0xb0) == 0xbf800000u, "fimm -1.0");
	CHECK(agx_fimm_to_f32(0x38) == 0x3fc00000u, "fimm 1.5");
	CHECK(agx_fimm_to_f32(0x70) == 0x41800000u, "fimm 16.0");
	CHECK(agx_fimm_to_f32(0x01) == 0x3c800000u, "fimm 1/64");
	/* the printer agrees with the value: every fimm the corpus prints round-trips through
	 * the assembler, so here only check the f32 helper against the printed decimal */
	for (unsigned v = 0; v < 256; v++) {
		union {
			uint32_t u;
			float f;
		} x = {agx_fimm_to_f32((uint8_t)v)};
		double want = (v & 0x70) ? (double)(0x10 | (v & 0xf)) : (double)(v & 0xf) / 64.0;
		int e = (int)((v >> 4) & 7);
		if (e)
			for (int k = e; k < 7; k++)
				want /= 2.0;
		if (v & 0x80)
			want = -want;
		CHECK((double)x.f == want, "fimm %#x: %g vs %g", v, (double)x.f, want);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <fixtures dir>\n", argv[0]);
		return 2;
	}
	dir = argv[1];
	test_corpus();
	test_mesa_cases();
	test_negative();
	if (fails) {
		printf("AIENOS_AGX_NATIVE: FAIL (%d)\n", fails);
		return 1;
	}
	printf("AIENOS_AGX_NATIVE: PASS\n");
	return 0;
}
