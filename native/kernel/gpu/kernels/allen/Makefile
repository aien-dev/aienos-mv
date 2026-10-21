# Host tests for the small Allen GPU kernels (L389). Kernel-side files (allen_kern, allen_plan, ka, kmath) use no floats;
# the mirror (allen_ref, fmath_ref) and the tests are host-only. -ffp-contract=off keeps the mirror's roundings exact.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -ffp-contract=off
G = ../..
SRC = allen_kern.c allen_ref.c allen_plan.c ka.c kmath.c fmath_ref.c $(G)/gcn_enc.c $(G)/gcn_interp.c

allen_test: allen_test.c $(SRC)
	$(CC) $(CFLAGS) -I. -I$(G) $^ -lm -lpthread -o $@

math_acc: math_acc.c fmath_ref.c
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

clean:
	rm -f allen_test math_acc
.PHONY: clean
