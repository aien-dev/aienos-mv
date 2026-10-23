# USV1 native reconstruction and host boundary

The application and behavioral probes are Omega. Existing Linux utilities compose source, arrange real file descriptors and inspect bytes. They are external orchestration, not a new first-party runtime or test harness. Run commands in a fresh private output directory, not a shared checkout. This recipe does not install anything or touch services.

## Inputs

Set `SRC` to this directory's immutable Git export, `OMC` to the qualified compiler, `CORE` and `CLI` to the unchanged public math sources, and `OUT` to a fresh user-owned directory. The evidence bundle contains the exact consumed core/CLI under `inputs/seed/math/`.

Expected SHA-256:

| Input | SHA-256 |
|---|---|
| Compiler | `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9` |
| core.om | `b70391a7a38464cd63ff796748565baf3dc2f73afaed90cbc2b94774dba02db6` |
| cli.om | `145db7e5184efbc5076ce27e93ddc7822e3f509c793f04f2413e0fd29f3343d0` |

Verify these exact values before building. Every compile must exit 0 and produce a nonempty ELF64 AArch64 EXEC; a compiler error or empty file is never a passing probe or caught mutant. Hash the compiler again before subsequent builds if it is not immutable.

```sh
sha256sum "$OMC" "$CORE" "$CLI"
mkdir -m 700 "$OUT"
cd "$OUT"
cat "$CORE" "$SRC/services.om" "$SRC/math-stream.om" | quiet-build "$OMC" > application
cat "$CORE" "$CLI" | quiet-build "$OMC" > reference
chmod 700 application reference
readelf -h application reference
printf 'eval 1/3+1/6\r\neval 1+\neval 7/-9\neval 2+2' > requests.in
./application < requests.in > application.out
./reference < requests.in > reference.out
cmp application.out reference.out
```

Both runs and `cmp` must exit 0. This includes refusal followed by successful recovery and EOF after a partial final frame. These are the same math semantics, not a second arithmetic implementation.

## Direct native assertions

Each composition below must compile successfully, be checked with `readelf -h`, then run. The native probe's nonzero exit is an assertion failure.

```sh
cat "$SRC/services.om" "$SRC/tests/facade-test.om" | quiet-build "$OMC" > boundaries
cat "$SRC/services.om" "$SRC/tests/facade-io.om" | quiet-build "$OMC" > binary-io
cat "$SRC/services.om" "$SRC/tests/facade-shortread.om" | quiet-build "$OMC" > shortread
cat "$SRC/services.om" "$SRC/tests/input-error.om" | quiet-build "$OMC" > input-error
cat "$SRC/services.om" "$SRC/tests/output-error.om" | quiet-build "$OMC" > output-error
cat "$SRC/services.om" "$SRC/tests/facade-stderr.om" | quiet-build "$OMC" > stderr-probe
chmod 700 boundaries binary-io shortread input-error output-error stderr-probe
readelf -h boundaries binary-io shortread input-error output-error stderr-probe
./boundaries </dev/null
printf '\000' > nul.in
./binary-io < nul.in > binary.out
printf 'OK\n' > expected.out
cmp binary.out expected.out
printf 'A\000B' > short.in
./shortread < short.in
./input-error <&-
./output-error 1>&- 2>/dev/null
./output-error >/dev/full 2>/dev/null
./stderr-probe >/dev/null 2>stderr.out
printf E > expected-stderr.out
cmp stderr.out expected-stderr.out
```

All executions and comparisons above must exit 0. Invalid handle/range/overflow and zero-length assertions run inside `boundaries`. Closed-descriptor and `/dev/full` probes assert returned errors inside Omega; they do not merely expect a crashed process.

## Actual short write with existing host tools

Requires installed `socat` with `pipesz` and `nonblock`. No fallback to a mock. The FIFO remains unread until the writer exits, so scheduling does not change whether the second write meets a full pipe. `socat` configures the shared open-file description without sending data; its exit must be 0. Descriptor 4 is opened read-only before descriptor 3 is closed; no writer is left during the drain.

```sh
cat "$SRC/services.om" "$SRC/tests/facade-shortwrite.om" | quiet-build "$OMC" > shortwrite
cat "$SRC/services.om" "$SRC/tests/shortwrite-receipt.om" | quiet-build "$OMC" > receipt
chmod 700 shortwrite receipt
readelf -h shortwrite receipt
mkfifo short.pipe
exec 3<>short.pipe
socat -u /dev/null FD:1,pipesz=4096,nonblock >&3
./shortwrite >&3
# Require exit 0 before proceeding: native code checked short progress then I/O refusal.
exec 4<short.pipe
exec 3>&-
dd bs=4096 status=none <&4 > short.out
exec 4<&-
./receipt < short.out
wc -c short.out
```

`receipt` must exit 0 and the file must contain 4096 bytes. Omega checks every byte is `A` and then checks EOF. Preserve the writer's own exit separately; a successful drain must not hide a failed writer.

Negative byte control: change the first byte of a copy of `short.out`, then feed it to `receipt`; the native assertion must exit 72, not 0. Preserve both files and statuses.

```sh
cp short.out wrong.out
printf B | dd of=wrong.out bs=1 count=1 conv=notrunc status=none
./receipt < wrong.out
# Expected exit 72; this is an intentional RED control.
```

## Compiled behavioral mutants

```sh
cat "$SRC/tests/mutants/wrong-read-rights.om" "$SRC/tests/facade-test.om" | quiet-build "$OMC" > mutant-rights
cat "$SRC/tests/mutants/premature-eof.om" "$SRC/tests/facade-io.om" | quiet-build "$OMC" > mutant-eof
chmod 700 mutant-rights mutant-eof
readelf -h mutant-rights mutant-eof
./mutant-rights </dev/null
# Expected exit 11, after successful compilation.
./mutant-eof < nul.in
# Expected exit 21, after successful compilation.
```

## Coverage and limits

The original immutable replay at `f91754159061b387b85328a570fd8c1b1bd32043` also exercised 4096-byte core refusal/recovery, the 65536-read transport bound, the 65536-request session bound, and six unchanged original exact-math suites. Their raw logs remain in `evidence/UNIVERSAL-ADAPTER/L716/`; they are historical runs of those exact native inputs, not fresh runs of this documentation commit. The removed worker-authored shell/Python harness is not an approved current dependency. Parent receipts separately identify exactly which commands were rerun using existing utilities.

Spark and Pi5 evidence qualifies the tested hosted Linux/AArch64 cells only. Both machines share an ISA. Native Aienux, GPU execution, two-language integration, public installer attachment and latency remain separate open gates. No host evidence authorizes a native boot or a merge.

Rollback: the commands create only files under the explicitly chosen `OUT`. Close descriptors 3/4 if an interrupted command left them open; remove only that owned output directory using the established cleanup tool. Nothing was installed or activated.
