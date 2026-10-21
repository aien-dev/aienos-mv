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
APPLE_HOST_SRCS := $(APPLE_DIR)/fdt.c $(APPLE_DIR)/apple_family.c $(APPLE_DIR)/aic.c $(APPLE_DIR)/apple_smp.c $(KDIR)/core/fmt.c
APPLE_HDRS := $(wildcard $(APPLE_DIR)/*.h) $(KDIR)/tests/ck_test.h
APPLE_TESTS := test_apple_fdt test_apple_aic test_apple_smp

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
# AIC: DIE_STRIDE die blocks 4 bytes off; V3_OFFSET AICv3 IRQ_CFG at the AIC2
# offset; IPI_ACK_LATE fast IPI acked after its handler; NO_ENABLE AIC2/3
# CONFIG enable never set. SMP: NO_CLEAN release not cleaned to memory; NO_SEV
# parked cores never woken; NO_RSV_CHECK release slot outside /memreserve/ accepted.
APPLE_MUTANTS := CK_FDT_MUTANT_NAMEOFF:test_apple_fdt CK_FDT_MUTANT_RANGES_IDENTITY:test_apple_fdt \
	CK_APPLE_MUTANT_GUESS:test_apple_fdt CK_AIC_MUTANT_DIE_STRIDE:test_apple_aic CK_AIC_MUTANT_V3_OFFSET:test_apple_aic \
	CK_AIC_MUTANT_IPI_ACK_LATE:test_apple_aic CK_AIC_MUTANT_NO_ENABLE:test_apple_aic \
	CK_SMP_MUTANT_NO_CLEAN:test_apple_smp CK_SMP_MUTANT_NO_SEV:test_apple_smp CK_SMP_MUTANT_NO_RSV_CHECK:test_apple_smp
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
	  if $(KCC) $(KCFLAGS) -I$(APPLE_DIR) -D$$m -fsyntax-only $(APPLE_DIR)/fdt.c $(APPLE_DIR)/apple_family.c $(APPLE_DIR)/aic.c $(APPLE_DIR)/apple_smp.c 2>/dev/null; then \
	    echo "apple mutant $$m accepted in a kernel build (FAIL)"; fail=1; fi; \
	done; \
	if [ $$fail = 0 ]; then echo "CK_APPLE_MUTANTS: PASS"; else echo "CK_APPLE_MUTANTS: FAIL"; exit 1; fi

.PHONY: apple-test apple-sanitize apple-mutants

ifeq ($(strip $(CK_APPLE)),1)
# Order-only: the main `test` recipe runs every normal prerequisite ($^) as
# a test binary, so apple-test must not be one of them.
test: | apple-test
# CK_APPLE never changes BOOTAA64.EFI (scripts/ck_apple_flag_check.sh), so it
# is not handed down to child tools: scripts/native_package.sh (package-test)
# refuses every unrecorded CK_* in its environment or MAKEFLAGS.
unexport CK_APPLE
MAKEOVERRIDES := $(filter-out CK_APPLE=%,$(MAKEOVERRIDES))
endif

# ---- CK_APPLE=1: raw arm64 Image with the device-tree handoff ----
# Own OUT subdirectory and own objects: nothing here mixes with, or is linked
# into, the UEFI/ACPI images. CK_APPLE_TEST_E2H=1 is the TEST-ONLY QEMU image
# that sets HCR_EL2.E2H itself, so QEMU runs the E2H path Apple CPUs force.
ifeq ($(strip $(CK_APPLE)),1)
ifeq ($(strip $(CK_APPLE_TEST_E2H)),1)
ifneq ($(strip $(CK_HARDWARE_STAGING)),)
$(error CK_APPLE_TEST_E2H is TEST-only and cannot be combined with CK_HARDWARE_STAGING)
endif
ifeq ($(strip $(CK_APPLE_TEST_MUTANT)),cnthctl)
APPLE_OUT := $(OUT)/apple-test-e2h-mutant-cnthctl
APPLE_FLAGS := -DCK_APPLE_TEST_E2H=1 -DCK_APPLE_MUTANT_CNTHCTL_NVHE=1
else
APPLE_OUT := $(OUT)/apple-test-e2h
APPLE_FLAGS := -DCK_APPLE_TEST_E2H=1
endif
else
APPLE_OUT := $(OUT)/apple
APPLE_FLAGS :=
endif
APPLE_KC := $(APPLE_DIR)/apple_main.c $(APPLE_DIR)/fdt.c $(APPLE_DIR)/apple_family.c $(APPLE_DIR)/apple_console.c \
	$(APPLE_DIR)/aic.c $(APPLE_DIR)/aic_live.c $(APPLE_DIR)/apple_smp.c $(KDIR)/core/fmt.c $(KDIR)/core/libc_min.c
APPLE_KS := $(APPLE_DIR)/apple_entry.S
APPLE_OBJS := $(patsubst $(KDIR)/%.c,$(APPLE_OUT)/obj/%.o,$(APPLE_KC)) $(patsubst $(KDIR)/%.S,$(APPLE_OUT)/obj/%.o,$(APPLE_KS))
APPLE_KCFLAGS := $(filter-out $(SMP_TEST_FLAGS) $(CK_EXTRA_CFLAGS),$(KCFLAGS)) -I$(APPLE_DIR) $(APPLE_FLAGS)
APPLE_LDFLAGS := -nostdlib -pie --no-dynamic-linker -z norelro -z noexecstack -z max-page-size=4096 \
	--no-warn-rwx-segments -T $(APPLE_DIR)/apple_image.ld

$(APPLE_OUT)/obj/%.o: $(KDIR)/%.c $(APPLE_HDRS) $(KDIR)/core/fmt.h
	@mkdir -p $(dir $@)
	$(KCC) $(APPLE_KCFLAGS) -c -o $@ $<
$(APPLE_OUT)/obj/%.o: $(KDIR)/%.S
	@mkdir -p $(dir $@)
	$(KCC) $(APPLE_KCFLAGS) -c -o $@ $<

# Link, then check the image before anyone boots it: only RELATIVE
# relocations, the "ARM\x64" magic at 0x38, a branch at offset 0.
$(APPLE_OUT)/Image: $(APPLE_OBJS) $(APPLE_DIR)/apple_image.ld
	$(KLD) $(APPLE_LDFLAGS) -o $(APPLE_OUT)/apple.elf $(APPLE_OBJS)
	@bad=$$($(KREADELF) -rW $(APPLE_OUT)/apple.elf | awk '/^[0-9a-f]+ +[0-9a-f]+ R_AARCH64/ && $$3 != "R_AARCH64_RELATIVE"' | wc -l); \
	if [ "$$bad" != 0 ]; then echo "apple Image: $$bad non-RELATIVE relocations (refused)"; exit 1; fi
	$(KOBJCOPY) -O binary $(APPLE_OUT)/apple.elf $@
	@magic=$$(od -An -tx4 -j56 -N4 $@ | tr -d ' '); op=$$(od -An -tx1 -j3 -N1 $@ | tr -d ' '); \
	if [ "$$magic" != 644d5241 ] || [ "$$op" != 14 ]; then echo "apple Image: header check FAILED (magic=$$magic op=$$op)"; exit 1; fi; \
	echo "apple Image: $@ $$(stat -c %s $@) bytes, header ok (ARM\\x64, b at 0, only RELATIVE relocations)"

apple-image: $(APPLE_OUT)/Image
all: apple-image
.PHONY: apple-image
endif
