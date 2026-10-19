---
name: aienos-basics
description: How to build, test and gate AIENOS (the C kernel in native/, the frozen Rust scaffolding in crates/), the invariants that must not move, where receipts live, and the paths agents must not touch. Read before editing anything in this repository.
---

# AIENOS basics

AIENOS is the AIEN operating system kernel for the NVIDIA DGX Spark. Status on `main`: experimental, pre-alpha, nothing physically qualified. The C kernel in `native/` is the active work; the Rust in `crates/` is frozen scaffolding and reference. Status and sequencing for the whole system belong to aien-architecture (read `docs/PLAN_AUTHORITY.md` and `ROADMAP.md` before trusting any plan here).

## Build and host tests

The C kernel needs only gcc, binutils, make and a shell (README.md, "Build and verify"):

```bash
make -C native/kernel test        # C kernel host tests
make -C native/kernel sanitize    # same under ASan and UBSan
make -C native/capability test    # capability authority (docs/CAPABILITY_ABI.md)
bash scripts/cap_abi_isolation_test.sh  # C and Rust capability ABIs stay separate
make -C native/store test mutants # torn-write-safe Store record
```

Each other suite under `native/` (argus, crypto, disk, m5, net, sig) has its own Makefile with `test`, `sanitize` and, for most, `mutants`.

## Gates

`tests/gates/run.sh <step>` is the one local entry point; each `tests/gates/*.gate` manifest names a step and what it `requires`.

- Host steps: `ck-host-tests` (`make -C native/kernel test sanitize`), `ck-stage-tests`, `ck-gate-selftest`, `ck-hold-selftest`, `ck-owner-keys`, `native-net`, `native-sig`, `native-crypto`, `native-m5`, `native-disk`, `native-argus`, `native-capability`, `store-native`, `crypto-boundary`, `infer-crate`.
- QEMU steps: `ck-qemu-gates` (runs `bash scripts/ck_gates.sh`), `disk-qemu`, `ck-b7b-probe`, `verify-all` (`bash scripts/verify_all.sh`, the legacy Rust workspace; needs the `aarch64-unknown-uefi` and `aarch64-unknown-none` targets).
- Always run on every commit (`.aien/always`): `CRUMB-VERIFY`, `AIENOS-CRYPTO-BOUNDARY`, `AIENOS-ZERO-DISK-SECRETS`.
- `scripts/ck_gates.sh` refuses a dirty tree. Its child scripts take the QEMU gate lock and report NOT_RUN while another gate run holds it or while the machine quiet flag (`~/workspace/.spark-quiet`) is set. Never raise the quiet flag yourself.
- `native/kernel/GATES.md` says what each emulator gate checks.

The DGX Spark is the test judge for pull requests (aien-architecture ADR 0033). The hosted workflow `.github/workflows/aien-verification.yml` does not build the code; it checks the bundle the Spark signed for the commit.

## Invariants that must not move

From `AGENTS.md` and `CONTRIBUTING.md`:

- Keep the three proofs separate: Store v1 structural integrity (no key material), M5 keyed authentication, anti-rollback freshness. Never merge or shortcut them.
- Store v1 superblock bytes 172 to 4095 stay zero forever. No tags, MACs or encryption headers in the superblock; cryptographic metadata lives in Store objects (kinds 21, 22, 23).
- No content hash (`ObjectId`) or Store generation in AEAD AAD or MAC input. The chunk AAD layout in `AGENTS.md` section 3 is fixed.
- Streaming decryption never writes unauthenticated plaintext to the caller: decrypt into a private scratchpad, verify the tag in constant time, then copy; zeroize on failure.
- Tag, MAC and key comparisons are constant time; keys and scratchpads are zeroized on drop.
- Language rule (aien-architecture ADR 0024): Rust is scaffolding, Omega is the destination, C or assembly only where hardware, boot, ABI or measurement justifies it. No Python, no CUDA toolkit, no CUDA-shaped APIs, no systemd or Linux init.
- The trusted base builds offline from inspectable source; no proprietary binaries, cloud services or network fetches in the build.
- A QEMU PASS qualifies nothing physical. Work that needs the real Spark is labelled `needs-hardware` and run by maintainers.
- Changes to the root of trust, security boundaries, persistent identity, irreversible data formats or recovery guarantees need maintainer and operator sign-off (`docs/MILESTONES.md`).

## Receipts and evidence

- `evidence/` holds hardware and emulator evidence. `scripts/ck_gates.sh` writes `evidence/ck_gates_<sha256 of content>.json` (receipt says `"physical": "NOT_RUN"`); an existing receipt is never overwritten.
- Signed Spark judge results for each commit are published in `aien-dev/aien-receipts`.

## Do not touch

Agents must not edit `.crumb` files, `crumb.lock`, `.aien/`, `tests/gates/`, `.github/workflows/`, `.forgejo/` or `tools/aien-test/`, and must not run `crumb compile`. Those are pipeline paths; a person changes them on purpose. The hook `tools/agent-hooks/protected-paths.sh` enforces this.

## Pull requests

One concern per pull request. Lead the description with the commands you ran and their output. No em dashes or en dashes in docs or comments.
