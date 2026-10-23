# Allen portable compiler build qualification

Base: aienos/aienux `f52f17a9e9520c1d16ba4d30aa8ee3bda8ee12a5`.
Compiler source: omega-bridge `0fc7d8662cb799e7c206b17c18ea4c3ec2b8fea9`.
Platform: Linux AArch64 on Spark. No live services or hardware configuration changed.

## Executed

- RED: the original build followed a dangling output link and published through
  it. `red.log` exits 1 after the existing-output check. The relocated-compiler
  probe alone did not establish selection: the original ignored the setting.
- One test setup attempt could not append to a read-only copied compiler.
  `fixture-setup-failure.log` records it. The test now makes its private copy
  writable before corrupting it; this was not a production failure.
- GREEN: the complete component gate includes the relocated-compiler probe,
  existing-file preservation, dangling-link refusal, corrupt compiler refusal,
  failed compilation leaving no output, absent compiler/argument refusal and
  temporary-file cleanup. `host-gate.log`, exit 0.
- Rebuilt the original Omega compiler from its exact Git source archive using
  `quiet-build sh tools/bootstrap.sh --offline`. `bootstrap.log`, exit 0.
- Replayed the complete Allen gate in a clean exported source directory using
  the repository-local default compiler location, with no `OMEGA_COMPILER`
  override and no private handoff compiler. Native modules, golden bytes,
  provider stream/fragment/refusal checks and all 24 compiled mutants pass.
- After Drake explicitly authorized publication, Allen and omega-bridge became
  public on 2026-10-23. Anonymous GitHub API reads verified both. Downloaded the
  pinned compiler tarball without credentials and independently rebuilt it;
  `public-bootstrap.log`, exit 0. Its digest equals the original compiler pin.
- `git diff --check` and `sh -n` for the three changed build scripts pass.

Source and artifact digests are in `source.sha256` and `artifacts.sha256`.
The broader repository verification is separate from these component results;
its eventual result must be recorded before claiming repository-wide qualification.

## Scope limits

The bootstrap proves source reconstruction and self-rebuild equality, not
independent diverse compiler ancestry. These checks do not qualify a hosted
window/input loop, live inference, durable device storage, x86/macOS/EFI output,
native boot or a physical iMac trial. Public source availability is PASS;
the interactive Alpha 1.0 release is not complete. No release tag was created.

Author review: compiler digest remains fixed; compilation and ELF inspection
must succeed before atomic no-replace publication; existing output links and
files are refused; transient files are removed on failure. Independent source
review remains pending with the publication desk.
