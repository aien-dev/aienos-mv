# L722 correction of L719 stdio overflow

Original8049f5d executable received an Omega-generated8201-byte input:8193-byte ASK line, newline, valid ASK hi. Actual RED exit0 with473 stdout bytes proves silent loss and subsequent job execution. After correction the standalone input loop returns65 as soon as a byte exceeds capacity; subsequent commands are not dispatched. The public gate requires exit65 and both outputs empty. All879 existing native checks and5 compiled behavioral mutants still pass their original expectations, memory and stdio goldens unchanged. Review source and reproduction are retained, not replaced by a no-findings claim.

Independent corrective review pending. Whole portability/full repository/iMac acceptance remains open.
