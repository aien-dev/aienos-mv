# L725: native Omega courier lifecycle

Partial host-native milestone under L694. This is not socket, Ethernet, model,
x86 EFI or physical iMac qualification. The provider must supply actual monotonic
milliseconds, link state, datagram bytes and whole-datagram send results. All new
product and executable test source is handwritten seeded Omega. Existing shell
and host tools only build, inspect, run, compare and inventory the artifacts.

## Executed evidence

- `full.log` and `full.exit`: complete native gate returned 0. `counts.json`
  inventories 1092 executed passing checks, including 213 scheduler checks and
  the original 879 checks. Existing memory/stdio goldens and stdio overflow
  refusal remain passing.
- Seven compiled behavioral mutants were rejected: stale 3, CRC 2, hash 2,
  readback 7, session 1, keepalive 40, ACK-stop 27. Compilation and nonempty static
  AArch64 EXEC checks precede every run, including the mutants.
- `source.sha256` binds all consumed native-tree files; parent compared each
  listed hash against source immediately before committing. `binaries.sha256`
  binds actual native artifacts under the named handoff output directory.
- The final scheduler tests cover 4096-byte jobs across five datagrams with
  byte-by-byte payload comparisons, failed-send retry, retry deadlines, HELLO/HI
  readiness/expiry, ACK and LINE retry stop, duplicate GOT, queue backpressure,
  cancellation, next-job reset, malformed/stale sessions and backward clocks.

## RED and fixture corrections retained

1. `red.om`/`red.exit`: initial test assembly omitted runtime.om, which code.om
   references. Compiler error 3 is a fixture setup error, not behavioral RED.
2. `red2.om`/`red2.log`/`red2.exit`: compiled Omega stub, 78 failures among 108
   checks. The first real implementation then ran with six failures in packet
   inspection (`green1.log`). Diagnosis: the existing test logger itself writes
   to Outb, overwriting pending datagrams before test inspection.
3. The test-only `test` wrapper now preserves Outb/Outlen/Err around logging.
   `red3` reran the compilable stub: 76 of 108 checks fail, exit 76. The same
   unchanged production implementation passes all 108 (`green2`). No assertion
   or production refusal was removed to resolve the fixture defect.
4. Expanded maximum-capacity/backpressure coverage passes 213 checks (`green3`),
   then the combined native gate passes all 1092. The two new compiled mutants
   prove the real timing and acknowledgement assertions detect behavioral faults.

The original L719 finding and L722 RED/GREEN remain retained. The separate L723
corrective review is archived under `../L722/corrective-review`, with exact-source
closure and binary identity checked, its three boundary probes rerun by the
parent, and positive control compared byte-for-byte. That review clears only the
stdio correction; it does not review this new scheduler.

Independent scheduler review remains pending. The broad legacy repository strict
suite runs separately; its outcome is not inferred from this native gate. No
READY/merge/deployment/full-platform claim is made here.
