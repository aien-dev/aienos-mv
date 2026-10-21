# apple.mk -- Apple Silicon platform layer (L250, docs/APPLE_SILICON_PLATFORM.md).
# Included at the end of the Makefile.
#
#   make apple-test        host tests of apple/ (fixtures tests/fixtures/apple)
#                          -> CK_APPLE_HOST: PASS
#   make apple-sanitize    same under ASan + UBSan
#   make apple-mutants     each TEST-ONLY mutant of apple/ must turn its test red
#   make CK_APPLE=1        also builds the raw arm64 Image with the device-tree
#                          handoff: $(OUT)/apple/Image (and `test` also runs
#                          apple-test). Without CK_APPLE=1 nothing here is linked
#                          into any image: the UEFI/ACPI images are unchanged.

APPLE_DIR := $(KDIR)/apple
APPLE_FIX := $(KDIR)/tests/fixtures/apple
APPLE_HCFLAGS = $(HCFLAGS) -I$(APPLE_DIR) -DCK_APPLE_HOST_TEST=1 -DAPPLE_FIXTURES=\"$(APPLE_FIX)\"
APPLE_HOST_SRCS := $(APPLE_DIR)/fdt.c $(APPLE_DIR)/apple_family.c $(KDIR)/core/fmt.c
APPLE_HDRS := $(wildcard $(APPLE_DIR)/*.h) $(KDIR)/tests/ck_test.h
APPLE_TESTS := test_apple_fdt

$(OUT)/host-apple/%: $(KDIR)/tests/%.c $(APPLE_HOST_SRCS) $(APPLE_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(APPLE_HCFLAGS) -o $@ $< $(APPLE_HOST_SRCS)

$(OUT)/host-apple-san/%: $(KDIR)/tests/%.c $(APPLE_HOST_SRCS) $(APPLE_HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(APPLE_HCFLAGS) $(SANFLAGS) -o $@ $< $(APPLE_HOST_SRCS)

apple-test: $(addprefix $(OUT)/host-apple/,$(APPLE_TESTS)) apple-mutants
	@fail=0; for t in $(addprefix $(OUT)/host-apple/,$(APPLE_TESTS)); do $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "CK_APPLE_HOST: PASS"; else echo "CK_APPLE_HOST: FAIL"; exit 1; fi

apple-sanitize: $(addprefix $(OUT)/host-apple-san/,$(APPLE_TESTS))
	@fail=0; for t in $^; do ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "CK_APPLE_HOST: PASS (sanitize)"; else echo "CK_APPLE_HOST: FAIL (sanitize)"; exit 1; fi

# Mutant -> the test that must turn red. NAMEOFF: property-name offset not
# bounds-checked; RANGES_IDENTITY: every bus `ranges` read as identity;
# GUESS: an unknown Apple SoC takes the first table row instead of a refusal.
APPLE_MUTANTS := CK_FDT_MUTANT_NAMEOFF:test_apple_fdt CK_FDT_MUTANT_RANGES_IDENTITY:test_apple_fdt \
	CK_APPLE_MUTANT_GUESS:test_apple_fdt
apple-mutants: $(APPLE_HOST_SRCS) $(APPLE_HDRS)
	@mkdir -p $(OUT)/host-apple-mut; fail=0; \
	for mt in $(APPLE_MUTANTS); do m=$${mt%%:*}; t=$${mt##*:}; b=$(OUT)/host-apple-mut/$${t}_$$m; \
	  if ! $(HOSTCC) $(APPLE_HCFLAGS) -D$$m -o $$b $(KDIR)/tests/$$t.c $(APPLE_HOST_SRCS); then \
	    echo "apple mutant $$m: does not build (FAIL)"; fail=1; continue; fi; \
	  if $$b > $$b.log 2>&1; then echo "apple mutant $$m: SURVIVED $$t (FAIL)"; fail=1; \
	  elif grep -q "^$$t: FAIL" $$b.log; then echo "apple mutant $$m: killed by $$t ($$(grep -c '^  FAIL ' $$b.log) checks failed)"; \
	  else echo "apple mutant $$m: no FAIL verdict (crashed?) (FAIL)"; fail=1; fi; \
	done; \
	for mt in $(APPLE_MUTANTS); do m=$${mt%%:*}; \
	  if $(KCC) $(KCFLAGS) -I$(APPLE_DIR) -D$$m -fsyntax-only $(APPLE_DIR)/fdt.c $(APPLE_DIR)/apple_family.c 2>/dev/null; then \
	    echo "apple mutant $$m accepted in a kernel build (FAIL)"; fail=1; fi; \
	done; \
	if [ $$fail = 0 ]; then echo "CK_APPLE_MUTANTS: PASS"; else echo "CK_APPLE_MUTANTS: FAIL"; exit 1; fi

.PHONY: apple-test apple-sanitize apple-mutants

ifeq ($(strip $(CK_APPLE)),1)
test: apple-test
endif
