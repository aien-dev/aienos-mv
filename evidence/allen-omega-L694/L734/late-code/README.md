# Late CODE completion correction

Follow-on to initial application milestone b4852db, before independent review.
A late CODE and final SAY can arrive on the original wire job after the first
missing-code response queues AGAIN, but before that retry is submitted. The
original code retained the queued retry after successful completion, causing an
unnecessary new wire job. New native regression:182 assertions,3 failures,
exit3 (`latecode-red`), preserving the original source and output.

The successful-completion branch now clears queued recovery bytes. The unchanged
regression passes182/182 within the complete fresh native suite. `counts.json`
and `final.log` are the final source verdict:1274 checks pass,9 compiled mutants
rejected, original stdio/memory goldens and overflow refusal pass. This supersedes
the initial1253-check source qualification for the changed application file; the
initial receipts are preserved, not rewritten. Source inventory was checked
against actual bytes immediately before commit. Independent review pending;
transport, device, x86 and iMac qualification remain open.
