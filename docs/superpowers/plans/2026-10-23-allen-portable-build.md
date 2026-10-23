# Allen portable seeded build

This continues the existing Alpha delivery contract in `native/omega/allen/ALPHA.md`.
It removes one acquisition blocker, not the unfinished hosted/iMac qualification.

1. Reproduce the private compiler-path dependency with the real pinned compiler
   copied to a fresh directory. Add build-orchestration checks for a changed
   compiler, invalid source, existing output and dangling output links.
2. Let `build-one.sh` select `OMEGA_COMPILER`, defaulting to the repository-local
   `build/allen-toolchain/build/bootstrap/omc`. Retain the fixed compiler digest.
   Publish an executable only after successful compilation and ELF inspection;
   never replace an existing output, including a dangling link.
3. Document source-only reconstruction of omega-bridge commit
   `0fc7d8662cb799e7c206b17c18ea4c3ec2b8fea9`. Rebuild from its Git archive and
   verify the existing compiler digest before executing the complete Allen gate
   from a clean source export outside the developer checkout.
4. Record actual results and remaining launch gates. Push the tested candidate
   through the existing publication desk; do not label this a public alpha.

No compiler implementation, product runtime, identity, memory or service changes.
Shell remains external build/test orchestration; all compiled probes are Omega.
