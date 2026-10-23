# L743 parent disposition

The independent review found no actionable defect in the complete frozen `services.om` and `math-stream.om` at `f91754159061b387b85328a570fd8c1b1bd32043`. Its report names that commit's immediate parent as a review-base reference; this is not a review of the entire Aienux repository or integration diff. Preserve the original report unchanged.

Parent intake verified the reviewed production hashes still match the published `a00ddbf8c89ca637208e4353af27a8983c2ec69d` tree. The intervening correction removed unauthorized newly authored shell/Python harnesses and corrected documentation; native implementation/test bytes did not change. Parent also hash-checked and re-executed the independent offset/refusal probe: exit 0, exact stdout `YZ`.

## Receipt precision

The transport fixture is **65545 bytes total**, not a single 65536-byte input. Its first nine bytes are `eval 2+2\n`, followed by 65536 unterminated `0` bytes. Driver exit 1 and 15 output bytes include the successful first request and the subsequent bounded refusal. Ordinary replay: 40 input/30 output bytes. Exact-capacity refusal/recovery: 4106 total input/15 output bytes. This clarifies the report's abbreviated transport description; no product defect was demonstrated.

## Additional parent qualification: read error after progress

Using the same source-built application (SHA-256 `01d1f5be0c653f7f281d3fce7831ebc122f0caaf30db82c7e1baf9c3696871a0`), a real private FIFO held `eval 1+1` without LF while its writer remained open. Existing `socat -u /dev/null FD:0,nonblock` configured the shared read description. After consuming the partial request, Linux returned EAGAIN rather than EOF. Actual application exit **2**, stdout/stderr **empty**. No partial request was evaluated as successful EOF.

A successfully compiled AArch64 mutant changed only the read-error return to EOF. Under the same fixture it incorrectly exited **0** and emitted `OK 2\n`; the empty-output comparison exited **1**. This is a caught behavioral mutant, not a compile failure. The original product required no correction. Raw logs and mutant Omega source/binary are alongside this disposition.

This clears bounded independent source review and the specifically exercised read-after-progress fault. It does not qualify EINTR, successful partial-write retry, signal behavior, native Aienux, FFI, installation, GPU, latency, integrated judge or whole-issue acceptance. Those limits remain explicit. Native gate enrollment and actual application composition remain open; no automatic issue closure or merge authorization is implied.
