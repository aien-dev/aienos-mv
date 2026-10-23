# L716 hosted services and math streaming

This directory is an opt-in USV1 hosted Linux reference plus a stream driver that reuses (does not edit or reimplement) the public exact math `core.om`. It is not wired into the `om`/`omg` public command, a native OS service, an installer, or an Aienux kernel. Native Aienux remains NOT_RUN pending its existing owners.

## Source composition

With the pinned public archive source and shared compiler available, build without generating foreign-language code:

```sh
cat "$OMEGA_MATH_CORE" native/omega/universal-services/services.om     native/omega/universal-services/math-stream.om | "$OMEGA_COMPILER" > math-stream
chmod 700 math-stream
```

The math source is pinned by SHA-256 in `CONTRACT.md` and the test runner; the compiler hash is checked before compilation. Compiler stdin/stdout, Linux syscall execution and the shell are disclosed host tooling. This driver uses `read`, `write`, and `exit` compiled from Omega. USV1 errors are `-1` handle/rights, `-2` range, `-3` returned native I/O error. The compiler currently requires fixed array extents, so the qualified interface is 4096-byte read and 8192-byte write, with explicit runtime offset/count checks. A view is copied into one bounded local write buffer because the pinned compiler refuses a native write directly from a view parameter.

## Test gate

`tests/run-hosted.sh` composes only committed Omega implementation/test sources plus the exact public core/CLI oracle. Omega test programs make behavior assertions; shell and a small Python descriptor harness only compile, arrange real descriptors, compare bytes, validate ELF metadata and execute. It covers wrong rights/handles, range and overflow refusal, zero length, short reads, NUL bytes, stderr mapping, closed-fd and `/dev/full` errors, a nonblocking short write, request/session limits, CRLF/partial EOF, refusal recovery, exact-output differential against the pinned original CLI, and two successfully compiled behavioral mutants.

Set `OMEGA_COMPILER`, `OMEGA_MATH_CORE`, `OMEGA_MATH_CLI`, and `TMPDIR` explicitly on another host. `TMPDIR` must point to a user-owned cache/handoff location. The runner checks the compiler/core/CLI SHA-256 before use. No package install, service, port, GPU, model, boot, or account change is needed.

## Qualification limits

The interface borrows descriptors 0/1/2 and never closes them. Default Linux signal delivery applies: a broken pipe can deliver SIGPIPE before Omega observes a returned `-EPIPE`. No cancellation, EINTR retry, threads, network, clocks, files, process spawn, dynamic libraries, alternate runtime, Pi-independent ISA, or native Aienux service support is claimed. The bounded arithmetic/application behavior is inherited from the exact public core; its existing capacity/domain limits still apply.
