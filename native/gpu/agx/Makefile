# Apple GPU (AGX, M1/M2 family) shader ISA: table-driven decoder/encoder,
# Mesa-syntax assembler/disassembler, and tools. Tables are generated from
# Mesa's AGX2.xml (MIT) plus AienOS additions; see docs/AGX_ISA_NOTES.md.
# Library objects (ISA core, UAT page tables, the simulated AGX and the driver
# job builder) import no heap, clock or I/O; the text layer, tools and tests
# use hosted libc. Simulator and driver: docs/AGX_SIMULATOR.md.
#
#   make            build out/libaienos_agx.a, out/agx_dis, out/agx_as
#   make test       ISA round-trip, Mesa-vector, negative and coverage tests,
#                   then the simulated-AGX job tests (host)
#   make sanitize   the same test under ASan+UBSan (Linux)
#   make gpu-ref    run every agx_cases.h case on this Mac's GPU through Metal
#                   (macOS only) and check it against the C reference
#   make clean

CC ?= cc
AR ?= ar
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -fstack-protector-strong
OUT ?= out

LIB_SRCS = agx_isa.c agx_isa_tables.c agx_uat.c agx_sim.c agx_job.c
TEXT_SRCS = agx_text.c
HDRS = agx_isa.h agx_text.h agx_fw.h agx_uat.h agx_sim.h agx_job.h agx_cases.h
LDLIBS = -lm
LIB_OBJS = $(LIB_SRCS:%.c=$(OUT)/%.o)
TEXT_OBJS = $(TEXT_SRCS:%.c=$(OUT)/%.o)
LIB = $(OUT)/libaienos_agx.a
TEST = $(OUT)/agx_isa_test
SIM_TEST = $(OUT)/agx_sim_test
TOOLS = $(OUT)/agx_dis $(OUT)/agx_as
SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g

all: $(LIB) $(TOOLS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/%.o: %.c $(HDRS) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJS) $(TEXT_OBJS)
	$(AR) rcs $@ $^

$(OUT)/agx_%: tools/agx_%.c $(LIB) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)

$(TEST): tests/agx_isa_test.c $(LIB) $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/agx_isa_test.c $(LIB)

$(SIM_TEST): tests/agx_sim_test.c $(LIB) $(HDRS)
	$(CC) $(CFLAGS) -o $@ tests/agx_sim_test.c $(LIB) $(LDLIBS)

test: $(TEST) $(SIM_TEST) $(TOOLS) lib-checks
	./$(TEST) tests/fixtures
	./$(SIM_TEST) tests/fixtures/corpus

# The library must not allocate, read a clock, or do its own I/O: the kernel
# uses the ISA core, page tables and job builder as they are.
lib-checks: $(LIB_OBJS)
	@bad=$$(nm -u $(LIB_OBJS) | awk 'NF {print $$NF}' | sed 's/^_//' | grep -xE 'malloc|calloc|realloc|free|aligned_alloc|posix_memalign|mmap|clock_gettime|time|gettimeofday|open|openat|read|write|pread|pwrite|fopen|fread|fwrite|printf|fprintf|snprintf|puts|getrandom'); \
	if [ -n "$$bad" ]; then echo "lib-checks FAIL: AGX library imports $$bad"; exit 1; fi
	@echo "lib-checks PASS (no heap, clock or I/O in the AGX library)"

sanitize: | $(OUT)
ifeq ($(shell uname -s),Linux)
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/agx_isa_test_asan tests/agx_isa_test.c $(LIB_SRCS) $(TEXT_SRCS)
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/agx_isa_test_asan tests/fixtures
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/agx_sim_test_asan tests/agx_sim_test.c $(LIB_SRCS) $(LDLIBS)
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/agx_sim_test_asan tests/fixtures/corpus
else
	@echo "sanitize: Linux only"
endif

# Real-GPU runner (macOS, Objective-C because Metal is the only user-space path
# to the GPU there). Deployment target 13.0 so the same binary runs in the
# macOS 13.5 install used for the hypervisor trace.
$(OUT)/agx_metal_run: tools/agx_metal_run.m agx_cases.h | $(OUT)
	$(CC) -fobjc-arc -mmacosx-version-min=13.0 -O2 -Wall -Wextra -Werror -o $@ tools/agx_metal_run.m \
		-framework Metal -framework Foundation

gpu-ref:
ifeq ($(shell uname -s),Darwin)
	$(MAKE) $(OUT)/agx_metal_run
	@for c in vadd vadd_i32 vadd_bounds saxpy matmul_naive matmul_tiled; do \
		./$(OUT)/agx_metal_run tests/fixtures/corpus $$c | head -1 || exit 1; done
else
	@echo "gpu-ref: macOS only"
endif

clean:
	rm -rf $(OUT)

.PHONY: all test lib-checks sanitize gpu-ref clean
