# L631: legacy hosted C mirror refusal checks

This is maintenance of an existing external oracle, not a new Omega runtime or
an additional native port. Native cotplan/bootsim and their compiler are unchanged.

L625 identified an unspecified-argument-evaluation-order dependency: the selftest
passed both an operation writing `why` and the value of `why` to expect_refused.
The fix evaluates each operation into `refusal_rc` first, then checks its reason.
All35 textual call sites, including two reused mutation macros, are sequenced.

The original GCC build passes at O0 and O2; files cot-mirror-red.txt and
cot-mirror-red-o0.txt are retained **passing baselines**, despite their filenames.
The explicit opposite-order probe captures the previous reason before evaluating
the operation, modeling another order allowed by the original call expressions.
It compiles and fails50 assertions (exit1). This is a forced-order probe, not a
claim that the observed GCC executable itself failed. Its source is retained as
cot-mirror-order-probe.c and output as cot-mirror-order-red.txt.

Final code passes selftest with GCC on Spark and Apple clang17 on macOS, both at
O0 and O2, all with -Wall -Wextra -Werror. Clang initially rejected the existing
option-parser loop because it incremented its index in both body and header.
Both increments were consolidated into a single `i += 2`, preserving option/value
pair traversal. No warning was disabled for final qualification. Five actual CLI
cases compare status/stdout/stderr exactly against the original executable:
two successful encodes, missing value, unknown option and missing required files.
Results are in cot-mirror-cli-parity.json. Initial no-Werror exploration was not
used as final acceptance; final clang receipt names include `final`.

Reproduce the Spark selftest from repository root:

```
for opt in 0 2; do
  gcc -std=gnu11 -O$opt -Wall -Wextra -Werror -Inative/argus \
    native/kernel/tools/ck_gsp_cot_encode.c native/argus/sha256.c \
    -o build/cot-mirror-fixed-o$opt
  build/cot-mirror-fixed-o$opt selftest
done
```

For the forced-order probe substitute
`evidence/omega-sequencer/cot-mirror-order-probe.c` for the tool source and expect
exit1/50 failures. For macOS substitute clang for gcc (identical source/header
copies were compiled with `-I.` in isolated scratch).

The native packet encoder's L625 static disposition is retained separately.
Neither this repair nor that review authorizes hardware access or driver removal.
