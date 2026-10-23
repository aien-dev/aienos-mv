# Immutable-source acceptance closure, WIP

Implementation source commit: a19ed49 (full ID in handoff).
Reconstructed through `git archive a19ed49 native/omega/allen` plus committed
wire/stdio fixtures into fresh target/allen-omega-L694/immutable-source.
No inherited binary or receipt was treated as fresh acceptance.

Executed command:

```sh
quiet-build target/allen-omega-L694/immutable-source/native/omega/allen/gate.sh \
 /home/drakestapleton/handoffs/allen-omega-L694/build/final-native1
```

Exit0. All13 test/render/fixture programs and the standalone Allen executable
compile successfully to nonempty static Linux AArch64 ELF before execution.
879 native behavioral checks,0 failed. Memory golden comparison0; stdio golden
comparison0; actual native rendering exit0. Five compiled mutants fail the original
gates with exits3/2/2/7/1. final/ contains exact per-check outputs and SHA256s of
all executable and source inputs. source-closure.log verifies the archived inputs
also match the current implementation. No code changed after source commit.

Source-only compiler bootstrap at pinned bridge0fc7d866 succeeds, exit0,
stage1==stage2 and supplied compiler hash==rebuilt hash191b776e. bootstrap.log and
compiler.sha256 preserve the result. Shell/readelf/hash tools are external tools,
not a product runtime or a compiler implementation fallback.

Strict repository verification exit101: four unchanged Rust courier tests require
loopback sockets prohibited by the sandbox. Formatting passed. Separate
`cargo clippy --offline --all-targets -- -D warnings` exit0, zero warnings.
Strict suite is FAIL, not waived, not an Omega gate.

Publishing: original worktree Git metadata cannot be written under managed
permissions. A bare publication metadata directory was initialized in the permitted
handoff root from the baseline, using the existing worktree as its explicit
--work-tree. No second worktree or worker was created. Implementation commit and
closure commit are recorded there on feat/allen-omega-L694. Origin is the original
`git@github.com:aien-dev/aienux.git`. Push exit128 because github.com does not
resolve; push.log preserves it. Remote exact hash verification is unavailable.
A prerequisite-based Git bundle permits parent publication without rewriting the
implementation. No PR, merge, issue closure, service or deployment action occurred.

Required target and remaining portable gates:
[provider handoff](../../docs/superpowers/specs/2026-10-23-allen-omega-platform-gates.md),
[module limits](../../native/omega/allen/README.md). Verdict WIP / TARGET NOT_RUN /
REMOTE PUBLICATION BLOCKED. This is neither whole Allen replacement nor live.
