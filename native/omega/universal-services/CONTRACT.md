# Hosted Universal Services v1 (USV1)

Status: L716 application-scoped contract; hosted Linux adapter only. This is not an OS ABI or a promise of native Aienux support.

## Borrowed stream handles

- `1`: stdin, read only; maps to Linux descriptor 0.
- `2`: stdout, write only; maps to Linux descriptor 1.
- `3`: stderr, write only; maps to Linux descriptor 2.
- All other handles and wrong-direction access refuse with `USV_E_HANDLE = -1`.
- Invalid buffer ranges (including unsigned addition overflow) refuse with `USV_E_RANGE = -2`, before any syscall/effect. Valid zero-length operations return 0 without a syscall.
- A negative native syscall result returned to Omega maps to `USV_E_IO = -3`; no returned error is reported as EOF or success. Linux errno is intentionally not collapsed into success/EOF. Default Linux signal delivery remains active: a broken pipe can terminate the process with SIGPIPE before Omega observes `-EPIPE`; this profile does not suppress signals.
- `usread` returns a nonnegative byte count; 0 is EOF. `uswrite` returns the nonnegative count actually written. Short transfers are observable and the caller decides whether/how to continue. A later error does not roll back an already observed write prefix.
- Handles borrow the process existing descriptors. The facade never closes, duplicates, or retains OS descriptors or buffer addresses.

## Omega interface

`usread(h: ulong, b: edit uchar[4096], off: ulong, n: ulong) -> long`
`uswrite(h: ulong, b: view uchar[8192], off: ulong, n: ulong) -> long`

The pinned compiler ABI requires every `view`/`edit` array parameter to declare a fixed capacity (`T N`); unsized array parameters fail with diagnostic 16, and global arrays cannot be passed as these parameters (diagnostic 19). This ruling is source-backed by the pinned `omc` implementation at lines 2221 and 2295; the compiler itself is unchanged. USV1 therefore exposes fixed-capacity 4096-byte read and 8192-byte write views, not arbitrary capacities, and tests allocate local arrays for calls. Any future length-carried serialized bytes should live in module-owned buffers rather than passing globals as parameters. `write` on a borrowed `view` parameter is rejected by this compiler, so the hosted adapter copies the requested slice to one bounded 8192-byte local array before the native syscall; caller bytes are not modified.

Operations validate the handle and range on every call, then invoke Omega native `read`/`write` builtins exactly once. Caller owns buffer lifetime and must serialize calls. Fixed-buffer length is checked with `off <= len` and `n <= len-off` before offset arithmetic/syscall; no heap or hidden buffering is introduced.

## Application profile

The opt-in `math-stream.om` links the exact public `seed/math/core.om` source unchanged and implements the published line protocol using USV1, not a second arithmetic implementation. Requests are capped at 4095 payload bytes (4096-byte input array), output at the core 8192-byte result bound, and the session at 65536 request reads. LF frames requests; CR remains a math-core whitespace byte. A too-long math request is passed to the core bounded refusal path, then processing continues. EOF after a partial request evaluates that request; empty EOF emits nothing. One response is LF-terminated. A valid request after malformed/unsupported arithmetic refusal must still work.

No arguments, files, clocks, process creation, networking, async cancellation, or additional runtime are required by this example. Host compiler execution, Linux syscalls, shell build/test orchestration, and test-only external oracle scripts remain host dependencies and are disclosed. Native Aienux and Pi5 execution remain separate gates.
