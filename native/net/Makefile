# AIENOS M6-A hosted network stack, in C. No Rust, no Python, no outside
# library: SHA-256 is the in-house copy in ../argus/sha256.c.
# Protocol objects import no heap, clock or I/O; tests use hosted libc.
# Hosted only: native binding waits for a C kernel, production keys for M5.
#
#   make            build out/libaienos_net.a
#   make test       protocol tests + control-transport loss/recovery + virtio-net
#                   (QEMU config dumps, simulated hostile device) + lib-checks
#                   + diff-check (pinned C/Rust-reference agreement digest)
#   make sanitize   the same tests under ASan+UBSan (Linux)
#   make receipt    run everything on a clean tree, write receipts/<sha256>.txt
#   make diff-corpus   write out/diff_corpus.bin and out/diff_c.txt for the
#                      external Rust-reference driver (see README.md)
#   make secure     M6-B: X25519 RFC 7748 + secure transport tests, ASan/UBSan,
#                   guard mutants; prints AIENOS_NET_SECURE: PASS/FAIL
#   make mutants    delete each GUARD line in x25519.c / aienos_sec.c / aienos_ctl.c; tests must fail
#   make clean

CC ?= cc
AR ?= ar
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -fstack-protector-strong
OUT ?= out
SEED ?= 0x6a5e11
ITERS ?= 200000

# Differential corpus (fixed) and the output digest both C and the unmodified
# Rust reference produced on it. Change only with a new recorded Rust run.
DIFF_SEED = 0xa1e5
DIFF_COUNT = 200000
DIFF_DIGEST = 9ddf33a859ade2d92d0e86a213d29d682708b0ac308239a7b6f003dbf9a5cdfe
# The same corpus through the default build. It differs from the Rust digest
# only on records the reference refused as InvalidRegion, because the C parser
# no longer region-checks vendor caps of cfg_type outside 1..4 (QEMU PCI_CFG,
# Lane 26). The Rust digest above is still checked with a reference-compat
# build (out/net_diff_ref). C-only pin, recorded 2026-10-01; no Rust run.
DIFF_DIGEST_FIXED = 034151b28953e23cbcdc2b805885ea88887b3da52e46369e8c01bd5b276241cb

SRCS = aienos_net.c aienos_virtio_pci.c aienos_virtio_net.c aienos_ctl.c x25519.c aienos_sec.c
# M6-B links (never edits) the in-house AES-GCM-SIV/HMAC and Ed25519 code.
CRYPTO_SRCS = ../crypto/aes.c ../crypto/polyval.c ../crypto/gcm_siv.c ../crypto/hmac.c ../crypto/ct.c
SIG_SRCS = ../sig/ed25519.c ../sig/sha512.c ../sig/ct.c
DEP_SRCS = ../argus/sha256.c $(CRYPTO_SRCS) $(SIG_SRCS)
DEP_OBJS = $(OUT)/sha256.o $(OUT)/crypto_aes.o $(OUT)/crypto_polyval.o $(OUT)/crypto_gcm_siv.o \
           $(OUT)/crypto_hmac.o $(OUT)/crypto_ct.o $(OUT)/sig_ed25519.o $(OUT)/sig_sha512.o $(OUT)/sig_ct.o
OBJS = $(SRCS:%.c=$(OUT)/%.o) $(DEP_OBJS)
SEC_GUARD_FILES = x25519.c aienos_sec.c aienos_ctl.c
LIB = $(OUT)/libaienos_net.a
SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g

all: $(LIB)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/%.o: %.c $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/sha256.o: ../argus/sha256.c ../argus/sha256.h | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ ../argus/sha256.c
$(OUT)/crypto_%.o: ../crypto/%.c $(wildcard ../crypto/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OUT)/sig_%.o: ../sig/%.c $(wildcard ../sig/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(OUT)/net_test: tests/net_test.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)
$(OUT)/ctl_test: tests/ctl_test.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)
$(OUT)/net_diff: tests/net_diff.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)
$(OUT)/vnet_test: tests/vnet_test.c $(LIB)
	$(CC) $(CFLAGS) -I. -o $@ $< $(LIB)
$(OUT)/net_diff_ref: tests/net_diff.c $(SRCS) $(DEP_SRCS) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) -DAIENOS_VIRTIO_PCI_REFERENCE_COMPAT -o $@ tests/net_diff.c $(SRCS) $(DEP_SRCS)
$(OUT)/x25519_test: tests/x25519_test.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)
$(OUT)/sec_test: tests/sec_test.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)

test: lib-checks $(OUT)/net_test $(OUT)/ctl_test $(OUT)/vnet_test $(OUT)/x25519_test $(OUT)/sec_test diff-check
	./$(OUT)/net_test $(SEED) $(ITERS)
	./$(OUT)/ctl_test
	./$(OUT)/x25519_test
	./$(OUT)/sec_test
	./$(OUT)/vnet_test

# Protocol objects must not allocate, read a clock or do their own I/O.
lib-checks: $(OBJS)
	@for o in $(SRCS:%.c=$(OUT)/%.o); do \
	  if nm -u $$o | awk '{print $$NF}' | sed 's/^_//' | grep -qxE 'malloc|calloc|realloc|free|clock_gettime|time|gettimeofday|open|read|write|recv|recvfrom|send|sendto|socket|fopen|printf|rand|random|getrandom'; then \
	    echo "lib-checks FAIL: $$o allocates, reads a clock, does I/O or uses ambient randomness"; exit 1; fi; done
	@if grep -n 'CT_IDENTITY_[A-Z]* ' aienos_ctl.h | grep -v 'CT_IDENTITY_TEST' | grep -q .; then \
	  echo "lib-checks FAIL: an identity kind other than TEST is defined (production keys wait for M5)"; exit 1; fi
	@grep -q 'NOT-FOR-PRODUCTION' aienos_ctl.c || { echo "lib-checks FAIL: TEST label missing"; exit 1; }
	@grep -q "NOT-FOR-PRODUCTION" aienos_sec.c || { echo "lib-checks FAIL: M6-B TEST label missing"; exit 1; }
	@echo "lib-checks PASS (no heap, clock, I/O or ambient randomness; TEST identity only)"

diff-corpus: $(OUT)/net_diff $(OUT)/net_diff_ref
	./$(OUT)/net_diff gen $(DIFF_SEED) $(DIFF_COUNT) > $(OUT)/diff_corpus.bin
	./$(OUT)/net_diff run < $(OUT)/diff_corpus.bin > $(OUT)/diff_c.txt
	./$(OUT)/net_diff_ref run < $(OUT)/diff_corpus.bin > $(OUT)/diff_ref.txt
	@echo "corpus $$(sha256sum $(OUT)/diff_corpus.bin | cut -d' ' -f1)"
	@echo "c-out  $$(sha256sum $(OUT)/diff_c.txt | cut -d' ' -f1)"

diff-check: diff-corpus
	@got=$$(sha256sum $(OUT)/diff_ref.txt | cut -d' ' -f1); \
	if [ "$$got" = "$(DIFF_DIGEST)" ]; then echo "NET_RUST_DIFFERENTIAL_PINNED: PASS ($(DIFF_COUNT) records, reference-compat build)"; \
	else echo "NET_RUST_DIFFERENTIAL_PINNED: FAIL (got $$got, pinned $(DIFF_DIGEST))"; exit 1; fi
	@got=$$(sha256sum $(OUT)/diff_c.txt | cut -d' ' -f1); \
	if [ "$$got" = "$(DIFF_DIGEST_FIXED)" ]; then echo "NET_DIFFERENTIAL_FIXED_PINNED: PASS (C-only pin)"; \
	else echo "NET_DIFFERENTIAL_FIXED_PINNED: FAIL (got $$got, pinned $(DIFF_DIGEST_FIXED))"; exit 1; fi
	@paste -d'\t' $(OUT)/diff_ref.txt $(OUT)/diff_c.txt | awk -F'\t' \
	  '$$1 != $$2 { d++; if ($$1 != "V Err InvalidRegion") bad++ } \
	   END { if (NR != $(DIFF_COUNT) || bad) { print "NET_DIFFERENTIAL_DIVERGENCE: FAIL (" bad+0 " records differ outside the PCI_CFG fix)"; exit 1 } \
	         print "NET_DIFFERENTIAL_DIVERGENCE: PASS (" d+0 " records differ, all reference InvalidRegion)" }'

sanitize: | $(OUT)
ifeq ($(shell uname -s),Linux)
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/net_test_asan tests/net_test.c $(SRCS) $(DEP_SRCS)
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/ctl_test_asan tests/ctl_test.c $(SRCS) $(DEP_SRCS)
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/net_diff_asan tests/net_diff.c $(SRCS) $(DEP_SRCS)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/net_test_asan $(SEED) 50000
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/ctl_test_asan
	./$(OUT)/net_diff_asan gen $(DIFF_SEED) 20000 > $(OUT)/asan_corpus.bin
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/net_diff_asan run < $(OUT)/asan_corpus.bin > /dev/null
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/x25519_test_asan tests/x25519_test.c $(SRCS) $(DEP_SRCS)
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/sec_test_asan tests/sec_test.c $(SRCS) $(DEP_SRCS)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/x25519_test_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/sec_test_asan
	$(CC) $(CFLAGS) $(SANFLAGS) -o $(OUT)/vnet_test_asan tests/vnet_test.c $(SRCS) $(DEP_SRCS)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./$(OUT)/vnet_test_asan
	@echo "NET_SANITIZE: PASS"
else
	@echo "sanitize: Linux only"
endif

# Mutants (native/sig pattern): delete one "GUARD:name" line at a time from
# the M6-B sources and the Lane 19 resend jitter in aienos_ctl.c; the X25519,
# secure transport and control-transport tests must then fail. A
# mutant that compiles and passes all three tests SURVIVED, and fails this target.
NET_MUT_TEST_OBJS = $(OUT)/t_x25519_test.o $(OUT)/t_sec_test.o $(OUT)/t_ctl_test.o
$(OUT)/t_%_test.o: tests/%_test.c $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) -Wno-error -I. -c -o $@ $<

mutants: $(OBJS) $(NET_MUT_TEST_OBJS) | $(OUT)
	@killed=0; total=0; survived=""; list=""; \
	for f in $(SEC_GUARD_FILES); do \
	  for g in $$(grep -o 'GUARD:[a-z0-9-]*' $$f | cut -d: -f2); do \
	    case " $$list " in *" $$g "*) echo "mutant $$g: guard tag appears twice, parallel runs would collide"; survived="$$survived $$g"; continue ;; esac; \
	    total=$$((total+1)); mdir=$(OUT)/mut_$$g; rm -rf $$mdir; mkdir -p $$mdir; list="$$list $$g"; \
	    ( cp *.h $$mdir/; \
	      sed "/GUARD:$$g \*\//d" $$f > $$mdir/$$f; \
	      if cmp -s $$f $$mdir/$$f; then echo "mutant $$g: guard line not found" > $$mdir/msg; echo "$$g" > $$mdir/surv; exit 0; fi; \
	      others=""; for s in $(SRCS); do [ "$$s" = "$$f" ] || others="$$others $(OUT)/$${s%.c}.o"; done; \
	      if ! $(CC) $(CFLAGS) -Wno-error -I. -c -o $$mdir/m.o $$mdir/$$f 2>/dev/null || \
	         ! $(CC) $(CFLAGS) -Wno-error -o $$mdir/x25519_t $(OUT)/t_x25519_test.o $$mdir/m.o $$others $(DEP_OBJS) 2>/dev/null || \
	         ! $(CC) $(CFLAGS) -Wno-error -o $$mdir/sec_t $(OUT)/t_sec_test.o $$mdir/m.o $$others $(DEP_OBJS) 2>/dev/null || \
	         ! $(CC) $(CFLAGS) -Wno-error -o $$mdir/ctl_t $(OUT)/t_ctl_test.o $$mdir/m.o $$others $(DEP_OBJS) 2>/dev/null; then \
	        echo "mutant $$g: does not compile (counts as not killed)" > $$mdir/msg; echo "$$g" > $$mdir/surv; exit 0; fi; \
	      case "$$f" in \
	        x25519.c)     order="$$mdir/x25519_t $$mdir/sec_t $$mdir/ctl_t" ;; \
	        aienos_sec.c) order="$$mdir/sec_t $$mdir/x25519_t $$mdir/ctl_t" ;; \
	        *)            order="$$mdir/ctl_t $$mdir/x25519_t $$mdir/sec_t" ;; \
	      esac; \
	      pass=1; for b in $$order; do if ! ./$$b >/dev/null 2>&1; then pass=0; break; fi; done; \
	      if [ $$pass -eq 1 ]; then echo "mutant $$g: SURVIVED" > $$mdir/msg; echo "$$g" > $$mdir/surv; \
	      else echo "mutant $$g: killed" > $$mdir/msg; : > $$mdir/killed; fi \
	    ) & \
	  done; \
	done; \
	wait; \
	for g in $$list; do \
	  mdir=$(OUT)/mut_$$g; \
	  if [ -f $$mdir/msg ]; then cat $$mdir/msg; else echo "mutant $$g: no result recorded (counts as not killed)"; fi; \
	  if [ -f $$mdir/killed ] && [ ! -f $$mdir/surv ]; then killed=$$((killed+1)); else survived="$$survived $$g"; fi; \
	done; \
	echo "NET_SECURE_MUTANTS: $$killed/$$total killed"; \
	[ -z "$$survived" ] && [ $$total -ge 12 ] || { echo "NET_SECURE_MUTANTS: FAIL (not killed:$$survived)"; exit 1; }

# M6-B verdict. Hosted only: proves the protocol code, not a native binding,
# real keys, timing behaviour or a formal protocol proof (see README.md).
secure:
	@ok=1; \
	$(MAKE) --no-print-directory test || ok=0; \
	$(MAKE) --no-print-directory sanitize || ok=0; \
	$(MAKE) --no-print-directory mutants || ok=0; \
	if [ $$ok = 1 ]; then echo "AIENOS_NET_SECURE: PASS"; else echo "AIENOS_NET_SECURE: FAIL"; exit 1; fi

receipt:
	sh ./receipt.sh

# ---- ADR 0021 device contract: contract/virtio_net.devc -> contract/gen/ ----
#   make contract-gen     regenerate contract/gen/ (needs cargo; host tool crates/aienos-devc)
#   make contract-check   regenerate in memory, FAIL on any difference (drift)
#   make contract-test    the generated driver through the same hosted test as the hand one
#                         (tests/vnet_test.c), the fuzzer against both drivers, and the
#                         differential register trace: both drivers byte-identical
#   make contract-sanitize   the same under ASan+UBSan
# The kernel image never runs cargo: it compiles the committed contract/gen/ C.
DEVC ?= cargo run -q --manifest-path ../../Cargo.toml -p aienos-devc --
CONTRACT = contract/virtio_net.devc
GEN = contract/gen
GEN_DRV = $(GEN)/driver/vnet_contract.c
GEN_MODEL = $(GEN)/model/vnet_model.c
GEN_FUZZ = $(GEN)/model/vnet_fuzz.c
GEN_H = $(wildcard $(GEN)/*/*.h)
INC_GEN = -I$(GEN)/driver -I. -I$(GEN)/model
INC_HAND = -I. -I$(GEN)/model
HAND_DRV = aienos_virtio_net.c
PCI_NET = aienos_virtio_pci.c aienos_net.c ../argus/sha256.c
FUZZ_SEED ?= 0x6a5e11
FUZZ_WALKS ?= 20000
# The recording of the hand driver that `aienos-devc covers` and the Rust tests read.
# (its first lines: bring-up, queue setup and the first datapath calls; the full
# trace is 2 MB and is compared hand vs generated in out/ instead)
TRACE_FIXTURE = contract/traces/virtio_net_hand.trace
TRACE_FIXTURE_LINES = 4000

contract-gen:
	$(DEVC) gen $(CONTRACT) $(GEN)
contract-check:
	$(DEVC) check $(CONTRACT) $(GEN)
	$(DEVC) covers $(CONTRACT) $(TRACE_FIXTURE)
contract-trace-update: $(OUT)/vnet_trace_hand
	mkdir -p contract/traces
	./$(OUT)/vnet_trace_hand | head -n $(TRACE_FIXTURE_LINES) > $(TRACE_FIXTURE)

# $(1) = suffix, $(2) = extra flags
define contract_bins
$(OUT)/vnet_test_contract$(1): tests/vnet_test.c $(GEN_DRV) $(PCI_NET) $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(2) $(INC_GEN) -o $$@ tests/vnet_test.c $(GEN_DRV) $(PCI_NET)
$(OUT)/vnet_fuzz_contract$(1): $(GEN_FUZZ) $(GEN_DRV) $(GEN_MODEL) aienos_virtio_pci.c $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(2) $(INC_GEN) -o $$@ $(GEN_FUZZ) $(GEN_DRV) $(GEN_MODEL) aienos_virtio_pci.c
$(OUT)/vnet_fuzz_hand$(1): $(GEN_FUZZ) $(HAND_DRV) $(GEN_MODEL) aienos_virtio_pci.c $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(2) $(INC_HAND) -o $$@ $(GEN_FUZZ) $(HAND_DRV) $(GEN_MODEL) aienos_virtio_pci.c
$(OUT)/vnet_trace_contract$(1): tests/vnet_trace.c $(GEN_DRV) $(GEN_MODEL) aienos_virtio_pci.c $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(2) $(INC_GEN) -o $$@ tests/vnet_trace.c $(GEN_DRV) $(GEN_MODEL) aienos_virtio_pci.c
$(OUT)/vnet_trace_hand$(1): tests/vnet_trace.c $(HAND_DRV) $(GEN_MODEL) aienos_virtio_pci.c $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(2) $(INC_HAND) -o $$@ tests/vnet_trace.c $(HAND_DRV) $(GEN_MODEL) aienos_virtio_pci.c
endef
$(eval $(call contract_bins,,))
$(eval $(call contract_bins,_san,$(SANFLAGS)))

# $(1) = suffix
define contract_run
	./$(OUT)/vnet_test_contract$(1)
	./$(OUT)/vnet_fuzz_contract$(1) $(FUZZ_SEED) $(FUZZ_WALKS)
	./$(OUT)/vnet_fuzz_hand$(1) $(FUZZ_SEED) $(FUZZ_WALKS)
	./$(OUT)/vnet_trace_hand$(1) > $(OUT)/trace_hand$(1).txt
	./$(OUT)/vnet_trace_contract$(1) > $(OUT)/trace_contract$(1).txt
	cmp $(OUT)/trace_hand$(1).txt $(OUT)/trace_contract$(1).txt
	@echo "VNET_TRACE_DIFF: PASS ($$(wc -l < $(OUT)/trace_hand$(1).txt) lines identical, hand vs generated)"
	head -n $(TRACE_FIXTURE_LINES) $(OUT)/trace_hand$(1).txt | cmp - $(TRACE_FIXTURE)
	@echo "VNET_TRACE_FIXTURE: PASS (first $(TRACE_FIXTURE_LINES) lines match $(TRACE_FIXTURE); aienos-devc covers checks it against the contract)"
endef
contract-test: $(OUT)/vnet_test_contract $(OUT)/vnet_fuzz_contract $(OUT)/vnet_fuzz_hand $(OUT)/vnet_trace_contract $(OUT)/vnet_trace_hand
	$(call contract_run,)
contract-sanitize: $(OUT)/vnet_test_contract_san $(OUT)/vnet_fuzz_contract_san $(OUT)/vnet_fuzz_hand_san $(OUT)/vnet_trace_contract_san $(OUT)/vnet_trace_hand_san
	$(call contract_run,_san)

# Timing twin: both drivers, same loop, same model, same flags (-O2). Run it under
# quiet-measure; scripts/vnet_contract_timing.sh writes the receipt.
$(OUT)/vnet_bench_hand: tests/vnet_bench.c $(HAND_DRV) $(GEN_MODEL) aienos_virtio_pci.c $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(INC_HAND) -o $@ tests/vnet_bench.c $(HAND_DRV) $(GEN_MODEL) aienos_virtio_pci.c
$(OUT)/vnet_bench_contract: tests/vnet_bench.c $(GEN_DRV) $(GEN_MODEL) aienos_virtio_pci.c $(GEN_H) $(wildcard *.h) | $(OUT)
	$(CC) $(CFLAGS) $(INC_GEN) -o $@ tests/vnet_bench.c $(GEN_DRV) $(GEN_MODEL) aienos_virtio_pci.c
contract-bench: $(OUT)/vnet_bench_hand $(OUT)/vnet_bench_contract

clean:
	rm -rf $(OUT)

.PHONY: all test lib-checks diff-corpus diff-check sanitize mutants secure receipt clean contract-gen contract-check contract-test contract-sanitize contract-bench contract-trace-update
