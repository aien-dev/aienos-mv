# L734: native application binding

This increment joins runtime prompt admission, intent/JOB construction, courier
scheduling, CODE assembly and SAY/WAIT consumption in handwritten seeded Omega.
It is an executed provider-facing core, not actual network/device attachment.
The existing development stdio main remains separate. No live service, machine,
physical disk, compiler or kernel provider was changed.

## Actual results

`full.log`, `full.exit` and `counts.json`: 1253 native checks pass, including
161 application checks. Nine separately compiled behavioral mutants are rejected:
stale3, CRC2, hash2, readback7, session1, keepalive40, ACK-stop27, pending-wire1,
application-CODE3. Each executable was built through the pinned compiler and
checked for nonempty static AArch64 EXEC before execution. Memory and stdio
goldens, and the overflow refusal regression, remain passing.

`source.sha256` was checked against the current source bytes before this commit.
`binaries.sha256` inventories actual compiled executables in the fresh handoff
output directory. Build orchestration and log/hash collection use existing host
tools; product and executable test/probe logic are Omega only.

## RED/GREEN and construction failures

- Initial `red.om` did not compile: text identifiers longer than the selected
  compiler's eight-byte global-name limit returned error13. Tests were corrected
  to the actual grammar. This was not counted as behavioral RED.
- `red2` is a compilable stub: 131 executed assertions,103 failures, exit103.
- Initial real implementation `green1.om` exceeded the selected compiler's
  sixteen-slot expression budget (error12). Extracting the retry-admission helper
  preserved behavior; `green2` executed133 checks with exit0. The two additional
  executed assertions are conditional send-result checks skipped by the stub.
- Pending-output regression: a code-completion LINE could cause saytake to write
  AGAIN into Outb while a previously offered JOB was awaiting send-result
  reporting. `pending-red` executed154 checks with one failure. The application
  now preserves the complete outgoing bytes/length/error around reply handling;
  `pending-green` executes154 checks, zero failures. The final compiled
  pendingwire mutant independently reintroduces this defect and fails.
- Stale-inner-CODE regression: an accepted current transport envelope containing
  an old CODE ID could populate the old assembly buffer during a newer chat,
  before heard rejected its logical ID. `stalecode-red` executes161 checks with
  three state-mutation failures. The application now validates active code-kind
  and assembly/conversation binding before calling codetake. The same cases pass
  in the full suite, and the compiled appcode mutant fails those three checks.

Tests exercise real serialized protocol bytes through the application calls:
chat prompt->JOB->LINE->retained answer, immediate code acknowledgement,
missing-code AGAIN after GOT, distinct conversation/wire IDs, stale inner reply
refusal, complete CRC-checked code before display, cancellation/reset, failed send,
WAIT, slow notice, silence timeout, backward clocks and invalid admission.

Independent review pending. The earlier completed strict legacy repository suite
is retained in ../parent-strict; it is not silently relabeled as qualification
of this new native source. Native socket/clock/device integration, full portable
parity, x86 EFI/QEMU and physical iMac execution remain open under L694.
