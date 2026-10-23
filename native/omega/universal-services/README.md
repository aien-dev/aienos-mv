# L716 hosted services and math streaming

This directory is an opt-in USV1 hosted Linux reference plus a stream driver that reuses (does not edit or reimplement) the public exact math `core.om`. It is not wired into the `om`/`omg` public command, a native OS service, an installer, or an Aienux kernel. Native Aienux remains NOT_RUN pending its existing owners.

## Source composition

With the pinned public archive source and shared compiler available, build without generating foreign-language code:

```sh
cat "$OMEGA_MATH_CORE" native/omega/universal-services/services.om     native/omega/universal-services/math-stream.om | "$OMEGA_COMPILER" > math-stream
chmod 700 math-stream
```

The math source and compiler SHA-256 pins are listed in `TESTING.md` and the evidence manifests; verify them before compilation. Compiler stdin/stdout, Linux syscall execution and the shell are disclosed host tooling. This driver uses `read`, `write`, and `exit` compiled from Omega. USV1 errors are `-1` handle/rights, `-2` range, `-3` returned native I/O error. The compiler currently requires fixed array extents, so the qualified interface is 4096-byte read and 8192-byte write, with explicit runtime offset/count checks. A view is copied into one bounded local write buffer because the pinned compiler refuses a native write directly from a view parameter.

## Test gate

See [TESTING.md](TESTING.md) for command-by-command reconstruction using the pinned compiler, unchanged public math oracle, and native Omega assertions. Real short-write setup uses existing `socat` and a private FIFO; it needs no newly authored Python or shell harness. The removed worker-authored harnesses remain in Git history as historical evidence only, not the current test entrypoint.

The original six broad exact-math suites and immutable Spark/Pi5 replay receipts remain source-bound historical evidence. Parent cleanup changes documentation and orchestration, not the production Omega source. Linux, shell, GNU byte/file tools and socat are disclosed external test utilities; they are not part of application arithmetic. No global installation, services, model loads, GPU work or native boot is involved.

## Qualification limits

The interface borrows descriptors 0/1/2 and never closes them. Default Linux signal delivery applies: a broken pipe can deliver SIGPIPE before Omega observes a returned `-EPIPE`. No cancellation, EINTR retry, threads, network, clocks, files, process spawn, dynamic libraries, alternate runtime, Pi-independent ISA, or native Aienux service support is claimed. The bounded arithmetic/application behavior is inherited from the exact public core; its existing capacity/domain limits still apply.
