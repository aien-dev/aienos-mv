# Omega now gates the existing boot simulation

`make -C native/linux/aien-gpu sim-test` builds `regcore.om` + `regtrace.om` through the pinned seed compiler and supplies the resulting native AArch64 executable to both the plain and sanitizer boot-simulation runs. `window1_sim_test.sh` no longer uses its sed/grep/awk/sort/comm allowlist comparison block. It sends each actual `regs.txt`, followed by NUL, to Omega. No optional native flag or legacy fallback remains for this check. Other shell checks and the external C boot simulator remain.

The native gate parses `R|W SPACE hex-offset SPACE hex-value SPACE t=decimal LF`, accepting6..8 lowercase offset digits, exactly8 value digits, and1..20 timestamp digits. Timestamps are ignored diagnostic strings, not converted to u64. NUL is accepted only between complete lines and resets the per-startup SEC2 doorbell latch. The aggregate stream must terminate with NUL and fit65536 bytes. Exit0 means all accesses were permitted by the shared policy and all six write-list entries appeared somewhere in the suite. Exit1 means a permission or write-coverage failure; exit2 means framing, capacity or I/O refusal. All exits are silent. This is a verifier of observed accesses, not a replay or hardware operation.

Like the old check, it does not require every read-list address to appear in real boot traces. The separate RGP1 qualification exercises all13 read entries. The native gate additionally enforces the one-shot SEC2 rule independently for every trace, not only the successful init trace. Existing init/seqregs doorbell assertions remain in shell.

Both the builder and direct shell entry pin the C shim to SHA25610990a78ff93af70f8a7c2f36fb92fa05d8a53f787f70df77dfd73f28db6d5d6. Policy source reuse does not silently accept drift in the actual shim. `OMEGA_CC` selects the compiler path, not its identity: SHA25601b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6 is required. Missing or changed compiler/shim fails closed. The default path is the established Spark reconstruction; other hosts must set OMEGA_CC explicitly. Python performs external compilation/tests; shell/Linux/host SHA remain disclosed dependencies.

23 direct fixtures pass after11 stub failures. Four successfully compiled mutants are caught: bypass-policy4, skip-coverage1, missing trace reset1, empty timestamp1. Five real-caller controls show the old script ignores a failing native checker, the current script fails with that checker, shim drift refuses, the old script fails with sed/sort/comm blocked, and the new script passes without calling them. Six guard controls cover closed stdin, read-after-progress EAGAIN, output symlink/hardlink/input-overlap preservation and stale-output invalidation on a missing compiler. Builder checks are single-user preflight, not hostile filesystem race protection.

An initial source attempt used unsupported `break` and received compiler error9; explicit active-loop flags replaced it. Compiler refusal was not counted as a behavioral RED or a caught mutant.

## Replay

`bash evidence/omega-sequencer/trace-replay.sh` exports frozen sources, reconstructs the compiler from the existing qualified seed pin, runs direct native tests and the real Make target in fresh output space. Receipts under `trace-*` retain negative controls, source/binary identities and real caller output. Historical GSQ3/RGP1 receipts remain unchanged.

## Review and limits

Independent L604 review of the prior GSQ3/shared-policy source found no blockers. Parent verified both reviewed composed hashes before closing it; reviewer did not execute tests. This does not review the subsequently added trace parser/build integration.

**A real legacy test path is replaced, not the driver.** No NVIDIA module was unloaded or replaced, no firmware or live service changed, and no MMIO occurred. The kernel shim, C boot builder/sequencer/simulator, native hardware ABI, firmware behavior and hardware rollback qualification remain outside this completed slice.
