/*
 * agx_metal_run: run one of the agx_cases.h compute cases on the real GPU
 * through the public Metal API (macOS only; user space, changes nothing on the
 * machine). The kernel source is compiled at runtime from
 * tests/fixtures/corpus/<case>.metal.
 *
 * Prints the output hash (FNV-1a 64) and whether it equals the plain C
 * reference, so its result is the real-GPU ground truth the AGX simulator is
 * compared against. With --repeat it is also the workload run under the m1n1
 * hypervisor trace (docs/MACBOOK_AGX_TRACE_PROCEDURE.md).
 *
 *   agx_metal_run [--repeat N] [--sleep-ms M] [--out FILE] <corpus-dir> <case>
 *
 * Exit status 0 only when every run matched the reference.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../agx_cases.h"

static int usage(void)
{
	fprintf(stderr, "usage: agx_metal_run [--repeat N] [--sleep-ms M] [--out FILE] <corpus-dir> <case>\ncases:");
	for (size_t i = 0; i < AGX_NCASES; i++)
		fprintf(stderr, " %s", agx_cases[i].name);
	fprintf(stderr, "\n");
	return 2;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		long repeat = 1, sleep_ms = 0;
		const char *out_path = NULL;
		int ai = 1;
		for (; ai < argc && argv[ai][0] == '-'; ai++) {
			if (!strcmp(argv[ai], "--repeat") && ai + 1 < argc)
				repeat = strtol(argv[++ai], NULL, 0);
			else if (!strcmp(argv[ai], "--sleep-ms") && ai + 1 < argc)
				sleep_ms = strtol(argv[++ai], NULL, 0);
			else if (!strcmp(argv[ai], "--out") && ai + 1 < argc)
				out_path = argv[++ai];
			else
				return usage();
		}
		if (argc - ai != 2 || repeat < 1 || sleep_ms < 0)
			return usage();
		const struct agx_case_desc *c = NULL;
		for (size_t i = 0; i < AGX_NCASES; i++)
			if (!strcmp(agx_cases[i].name, argv[ai + 1]))
				c = &agx_cases[i];
		if (!c)
			return usage();

		NSString *src_path = [NSString stringWithFormat:@"%s/%s.metal", argv[ai], c->name];
		NSError *err = nil;
		NSString *src = [NSString stringWithContentsOfFile:src_path encoding:NSUTF8StringEncoding error:&err];
		if (!src) {
			fprintf(stderr, "cannot read %s\n", src_path.UTF8String);
			return 1;
		}
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (!dev) {
			fprintf(stderr, "no Metal device\n");
			return 1;
		}
		id<MTLLibrary> lib = [dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&err];
		id<MTLFunction> fn = lib ? [lib newFunctionWithName:@(c->name)] : nil;
		id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
		if (!ps) {
			fprintf(stderr, "compile failed: %s\n", err.localizedDescription.UTF8String);
			return 1;
		}

		uint32_t words = agx_case_words(c);
		size_t bytes = (size_t)words * 4;
		uint32_t *ref = calloc(words, 4), *ra = calloc(words, 4), *rb = calloc(words, 4);
		if (!ref || !ra || !rb)
			return 1;
		agx_case_fill(c, ra, rb, ref);
		agx_case_reference(c, ra, rb, ref);
		uint64_t want = agx_case_hash(ref, words);

		id<MTLCommandQueue> q = [dev newCommandQueue];
		int bad = 0;
		uint64_t got = 0;
		id<MTLBuffer> out = nil;
		for (long r = 0; r < repeat; r++) {
			id<MTLBuffer> a = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
			id<MTLBuffer> b = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
			out = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
			agx_case_fill(c, a.contents, b.contents, out.contents);
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
			[enc setComputePipelineState:ps];
			uint32_t n = c->n;
			float alpha = AGX_CASE_SAXPY_ALPHA;
			if (c->kind == AGX_CASE_SAXPY) {
				[enc setBuffer:a offset:0 atIndex:0];
				[enc setBuffer:out offset:0 atIndex:1];
				[enc setBytes:&alpha length:4 atIndex:2];
			} else {
				[enc setBuffer:a offset:0 atIndex:0];
				[enc setBuffer:b offset:0 atIndex:1];
				[enc setBuffer:out offset:0 atIndex:2];
				if (c->kind == AGX_CASE_VADD_BOUNDS || c->kind == AGX_CASE_MATMUL)
					[enc setBytes:&n length:4 atIndex:3];
			}
			[enc dispatchThreads:MTLSizeMake(c->grid[0], c->grid[1], c->grid[2])
				threadsPerThreadgroup:MTLSizeMake(c->tg[0], c->tg[1], c->tg[2])];
			[enc endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			if (cb.status != MTLCommandBufferStatusCompleted) {
				fprintf(stderr, "run %ld: command buffer failed: %s\n", r, cb.error.localizedDescription.UTF8String);
				return 1;
			}
			got = agx_case_hash(out.contents, words);
			if (got != want)
				bad++;
			if (sleep_ms && r + 1 < repeat)
				usleep((useconds_t)sleep_ms * 1000);
		}

		const uint32_t *o = out.contents;
		printf("case %s device \"%s\" words %u runs %ld gpu_hash %016llx ref_hash %016llx tg_mem %lu %s\n",
		       c->name, dev.name.UTF8String, words, repeat, (unsigned long long)got, (unsigned long long)want,
		       (unsigned long)ps.staticThreadgroupMemoryLength, bad ? "MISMATCH" : "MATCH");
		printf("first words: %08x %08x %08x %08x\n", o[0], o[1], o[2], o[3]);
		if (out_path) {
			FILE *f = fopen(out_path, "wb");
			if (!f || fwrite(o, 4, words, f) != words || fclose(f)) {
				fprintf(stderr, "cannot write %s\n", out_path);
				return 1;
			}
		}
		free(ref);
		free(ra);
		free(rb);
		return bad ? 1 : 0;
	}
}
