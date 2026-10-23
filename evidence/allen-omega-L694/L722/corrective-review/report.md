# L723 independent corrective review — L722 stdio overflow

**Result: no findings in the scoped correction.** Reviewed exact commit `13d02ab4b7145579f6d3faa53442167bfcdcd7bc` against parent `7f4cb7aa54164f71e2e26b0b834119af3e3839e5`. Checkout was clean and at the requested commit. The runtime change replaces the overflow-flag set with immediate status `65`; therefore neither newline handling nor any subsequent command can clear the condition and continue dispatching. The gate adds a generated 8193-byte command followed by `ASK hi`, requiring status 65 and empty stdout/stderr. No unrelated behavior or production files were changed in this review.

## Independent rebuild and probes

Rebuilt standalone `main.om` plus the exact pinned core at commit source with pinned `omc` SHA-256 `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`. `build-one.sh` succeeded; ELF was static AArch64 EXEC. Rebuilt executable SHA-256: `be4ff5265e6e46962c9a35008bcf8a9127c7d9fbcc00db58f9a40c8a4008fb5d`.

- **8193-byte line + newline + later valid command:** independently compiled the committed Omega input generator and ran it through the rebuilt executable. Input was 8201 bytes total; process returned `65`, stdout `0` bytes, stderr `0` bytes. The later `ASK hi` was not dispatched.
- **8193-byte line ending at EOF (no newline):** independently authored/compiled an Omega generator. Input was 8193 bytes; process returned `65`, stdout/stderr each `0` bytes.
- **8192-byte line boundary, followed by newline and valid command:** independently authored/compiled an Omega generator. Input was 8200 bytes and spans the runtime's 8192-byte read buffer boundary. Process returned `65`, stdout/stderr each `0` bytes. This establishes refusal at the exact-size line boundary case, but does not distinguish overflow detection from downstream refusal of the extremely long `ASK` payload; no stronger interpretation is claimed.
- **Positive control:** ran the established valid stdio fixture through the rebuilt executable; returned `0` and stdout/stderr matched their committed goldens byte-for-byte (`cmp` passed).

The first two overflow cases directly exercise the immediate-return branch, including a subsequent command in the newline case. Exact-source broad suite was not duplicated; parent brief records its existing 879-check/five-mutant run. No iMac, target, live-service, or full qualification claim is made.

## Artifacts

Review report: `~/handoffs/L723-review/report.md`.
Independent sources, generated fixtures, rebuilt executable, and captured outputs: `~/handoffs/L723-review/rebuild/`. These are review artifacts only; no source-tree edits were made.
