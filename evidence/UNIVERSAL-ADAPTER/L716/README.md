# L716 hosted Omega implementation receipts

Evidence for source commit `f91754159061b387b85328a570fd8c1b1bd32043` on `ua/services-L716`. Contains exact pinned math inputs, source manifest, fresh-source hosted runs on Spark and Raspberry Pi 5, compiled RED/mutant controls, original exact-math suites, raw syscall traces, I/O fixture/output, ELF and RSS receipts.

The implementation is an opt-in hosted Linux application path, not a native Aienux ABI, OS command change, boot/service/installer integration, or global installation. Native Aienux is `NOT_RUN` pending existing kernel owners. No latency timing was run. Host scripts only orchestrate and make fixtures; behavioral assertions are Omega. The exact-math core is reused unchanged with no host arithmetic fallback.

The pinned compiler's fixed-capacity view/edit ABI ruling (unsized arrays diagnostic 16; global array arguments diagnostic 19; source lines 2221 and 2295) is recorded in `../../../native/omega/universal-services/CONTRACT.md`; the compiler is unchanged. The extracted archive's 24-entry `provenance/sources.sha256` manifest verifies; the broader 183-entry `evidence/files.sha256` check had 3 mismatches outside L716 scope (README.md, tools/bootstrap.sh, tools/build.sh), preserved under `failures/`. `MANIFEST.sha256` hashes every other file in this evidence directory.
