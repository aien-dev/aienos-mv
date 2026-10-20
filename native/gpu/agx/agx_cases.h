/*
 * Deterministic compute cases shared by the real-GPU runner
 * (tools/agx_metal_run.m) and the AGX simulator tests, so that both start from
 * identical inputs and their outputs can be compared bit for bit.
 *
 * Float inputs are small multiples of 0.25 and matmul inputs are small
 * integers, so every result is exact in binary32 whatever the summation order:
 * a mismatch means a wrong element, never rounding.
 */
#ifndef AIENOS_AGX_CASES_H
#define AIENOS_AGX_CASES_H

#include <stddef.h>
#include <stdint.h>

enum agx_case_kind {
	AGX_CASE_VADD_F32,   /* c[i] = a[i] + b[i], float */
	AGX_CASE_VADD_I32,   /* c[i] = a[i] + b[i], int (wrapping) */
	AGX_CASE_VADD_BOUNDS, /* if (i < n) c[i] = a[i] + b[i]; grid > n */
	AGX_CASE_SAXPY,      /* y[i] = alpha * x[i] + y[i] */
	AGX_CASE_MATMUL,     /* C = A * B, N x N float */
};

struct agx_case {
	const char *name;      /* also the kernel function name in corpus/<name>.metal */
	enum agx_case_kind kind;
	uint32_t n;            /* element count, or N for matmul */
	uint32_t grid[3];      /* threads in the grid */
	uint32_t tg[3];        /* threads per threadgroup */
};

static const struct agx_case agx_cases[] = {
	{ "vadd", AGX_CASE_VADD_F32, 1024, { 1024, 1, 1 }, { 64, 1, 1 } },
	{ "vadd_i32", AGX_CASE_VADD_I32, 1024, { 1024, 1, 1 }, { 64, 1, 1 } },
	{ "vadd_bounds", AGX_CASE_VADD_BOUNDS, 1000, { 1024, 1, 1 }, { 64, 1, 1 } },
	{ "saxpy", AGX_CASE_SAXPY, 1024, { 1024, 1, 1 }, { 64, 1, 1 } },
	{ "matmul_naive", AGX_CASE_MATMUL, 32, { 32, 32, 1 }, { 16, 16, 1 } },
	{ "matmul_tiled", AGX_CASE_MATMUL, 32, { 32, 32, 1 }, { 16, 16, 1 } },
};
#define AGX_NCASES (sizeof(agx_cases) / sizeof(agx_cases[0]))

#define AGX_CASE_SAXPY_ALPHA 2.5f
#define AGX_CASE_SENTINEL 0xdeadbeefu /* prefill of every output buffer */

static inline float agx_case_af(uint32_t i) { return (float)((i * 37u) % 251u) * 0.25f - 31.0f; }
static inline float agx_case_bf(uint32_t i) { return (float)((i * 101u) % 113u) * 0.5f - 28.0f; }
static inline int32_t agx_case_ai(uint32_t i) { return (int32_t)(i * 2654435761u); }
static inline int32_t agx_case_bi(uint32_t i) { return (int32_t)((i * 40503u) ^ 0x5bd1e995u); }
static inline float agx_case_ma(uint32_t i) { return (float)((int)((i * 7u) % 9u) - 4); }
static inline float agx_case_mb(uint32_t i) { return (float)((int)((i * 5u) % 7u) - 3); }

/* Buffer sizes in 32-bit words: inputs a, b and the output. */
static inline uint32_t agx_case_words(const struct agx_case *c)
{
	if (c->kind == AGX_CASE_MATMUL)
		return c->n * c->n;
	return c->grid[0] > c->n ? c->grid[0] : c->n;
}

/* Fill inputs a, b and output o (all agx_case_words long). */
static inline void agx_case_fill(const struct agx_case *c, uint32_t *a, uint32_t *b, uint32_t *o)
{
	uint32_t w = agx_case_words(c);
	for (uint32_t i = 0; i < w; i++) {
		union { float f; int32_t s; uint32_t u; } x, y;
		switch (c->kind) {
		case AGX_CASE_VADD_I32:
			x.s = agx_case_ai(i);
			y.s = agx_case_bi(i);
			break;
		case AGX_CASE_MATMUL:
			x.f = agx_case_ma(i);
			y.f = agx_case_mb(i);
			break;
		default:
			x.f = agx_case_af(i);
			y.f = agx_case_bf(i);
			break;
		}
		a[i] = x.u;
		b[i] = y.u;
		o[i] = AGX_CASE_SENTINEL;
	}
	if (c->kind == AGX_CASE_SAXPY) /* y is read and written: it is the output */
		for (uint32_t i = 0; i < w; i++)
			o[i] = b[i];
}

static inline float agx_case_f(uint32_t u) { union { uint32_t u; float f; } x = { u }; return x.f; }
static inline uint32_t agx_case_u(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }

/* Plain C reference result, computed into o (already filled by agx_case_fill). */
static inline void agx_case_reference(const struct agx_case *c, const uint32_t *a, const uint32_t *b, uint32_t *o)
{
	switch (c->kind) {
	case AGX_CASE_VADD_F32:
	case AGX_CASE_VADD_BOUNDS:
		for (uint32_t i = 0; i < c->n; i++)
			o[i] = agx_case_u(agx_case_f(a[i]) + agx_case_f(b[i]));
		break;
	case AGX_CASE_VADD_I32:
		for (uint32_t i = 0; i < c->n; i++)
			o[i] = a[i] + b[i];
		break;
	case AGX_CASE_SAXPY:
		for (uint32_t i = 0; i < c->n; i++)
			o[i] = agx_case_u(AGX_CASE_SAXPY_ALPHA * agx_case_f(a[i]) + agx_case_f(b[i]));
		break;
	case AGX_CASE_MATMUL:
		for (uint32_t r = 0; r < c->n; r++)
			for (uint32_t col = 0; col < c->n; col++) {
				float acc = 0.0f;
				for (uint32_t k = 0; k < c->n; k++)
					acc += agx_case_f(a[r * c->n + k]) * agx_case_f(b[k * c->n + col]);
				o[r * c->n + col] = agx_case_u(acc);
			}
		break;
	}
}

/* FNV-1a 64 over the output words, little-endian bytes. */
static inline uint64_t agx_case_hash(const uint32_t *o, uint32_t words)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint32_t i = 0; i < words; i++)
		for (int k = 0; k < 4; k++) {
			h ^= (o[i] >> (8 * k)) & 0xff;
			h *= 0x100000001b3ull;
		}
	return h;
}

#endif
