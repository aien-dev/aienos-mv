# Preserved failure and recovery record

## Compiler/test development
- Unsized `view`/`edit` arrays failed with diagnostic 16; final signatures use fixed capacities. Passing a global array as a parameter failed with diagnostic 19; final tests pass local arrays. These match pinned compiler source lines 2221 and 2295; no compiler modification was made.
- Early wrong descriptor types and unsupported source composition failed with diagnostic 7; corrected in Omega and the hosted suite passes on both hosts.
- A wrong-rights mutant initially had an overly broad predicate; corrected and rebuilt; final mutant is killed with exit 11.

## Receipt/orchestration errors (not product failures)
- An early Spark measurement fixture was shell-quoted incorrectly and contained only `eval`; its input/output/trace/192-KiB RSS are retained in `failures/wrong-fixture/` and excluded from accepted measurements. The corrected CR/LF/NUL fixture is 37 bytes and matches the original CLI.
- A first Pi measurement generator was malformed by nested shell quoting (`SyntaxError: invalid decimal literal`) before product assertions; it was corrected via a direct Python byte writer and verified at 37 bytes.
- One source-manifest check ran from the receipts-only directory and failed because source files were not present there; it was rerun from the fresh extracted source tree and all 16 hashes passed. The source provenance manifest's 24 entries also pass. A broader 183-entry archive inventory check reports three mismatches outside this task's scope (`README.md`, `tools/bootstrap.sh`, `tools/build.sh`); the full output is retained in `failures/archive-inventory-verify.log`, and those files were not changed during L716.
- A short-write host-orchestration attempt first failed with `NameError: Path is not defined`; the missing host utility import was added, then the full suite passed. Omega now owns the byte/EOF assertions in `shortwrite-receipt.om`; the host script only sets up/drains the descriptor and passes captured bytes to the Omega assertion program.
- A premature-EOF mutant input was first emitted as literal slash text; it was replaced by verified one-byte NUL (`00`), and the compiled mutant exits 21.

## Accepted results
- Exact source commit `f91754159061b387b85328a570fd8c1b1bd32043` passes the hosted suite on Spark and Pi 5. The old-stub RED test exits 10; short-write receipt RED/GREEN are 72/0; wrong-rights and premature-EOF mutants exit 11 and 21.
- Original exact-math suites `gate`, `oracle`, `sign-fixtures`, `whitespace`, `checks`, and `robustness` all exit 0.
