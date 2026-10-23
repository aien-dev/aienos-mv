# L618 causal simulator review disposition

Reviewed functional commit: `5697de6e201499cd5e1db8717109c0bb317bfe66`.
Published implementation/replay: `8fc965a23b80cdb713c0c5dc7812adee10bc0a50`.
Reviewer: `deleg_7c8036af`, `sa-0-61728e02`.

The available original gateway transcript is preserved verbatim as
`L618-review-transcript.log`. It contains gateway truncation markers, not a
reconstructed complete report. Its visible final verdict reports no concrete
blockers after static inspection of the causal model, adapter, pinned C model
and boot routine, shared core, test harnesses, caller gate, docs and receipts.
The reviewer did not execute builds or tests. Read/write transitions, polling,
EMEM advancement, packet validation, phase changes and final-state fields were
reported consistent with the pinned C simulator by source inspection.

Parent verified that all native source/harness/Make files remain unchanged from
the reviewed commit and matched these reviewed source hashes:

- Causal composition: `65e14024e2488140941917b2b89f20be64a7a4205f55086efd121c008114b2ba`.
- Recorded-read composition: `480b93638b2dfc30517527b52a75eeba2c07d1d3730c5bf965e77b93fbac8b2d`.
- Extracted six-function core: `7e156753aa407ff8c572e81e6d18396998fcb796c377fa92a0aa31db5c8930a8`.

`bootsim-review-rerun.json` records a separate parent execution of six frozen
cases (success, FMC error, inactive core, EMEM failure, bad packet header and
zero-low-PA behavior). Both executable identity and expected-output hashes are
checked against the published full-suite receipt before execution. These are
parent reruns, not independent reviewer runtime results. The complete146-case
suite and fresh-seed combined-gate receipts remain the broader author evidence.

Disposition: clear L618's **offline source-review gate**. Do not interpret this
as hardware authorization, production retirement, cryptographic validation,
complete simulator API coverage or a verified physical-device model. The
SIM-CHOICE assumptions and public boot-only bounds in BOOTSIM.md still apply.

## Remaining boot-path replacement

The next upstream dependency remains the C boot-image/arena builder in
`native/kernel/dev/gb10_gsp_boot.c`. It constructs WPR metadata, libos arguments,
radix tables, queues/RPCs, FMC parameters and the SEC2 COT packet consumed by the
boot core. The causal tests currently supply constructed packet fixtures; they
do not replace that builder. Production C callers, the Linux shim, NVIDIA
modules, firmware admission and hardware recovery remain under open L332.
