# Preserved development failures

These are debugging history, not accepted gates or TDD RED counts.

* runtime-green first compile: extra put operand, error5; shell attempt returned126
  on empty non-executable output. Corrected. No invalid ELF ran or earned a pass.
* courier1: register budget error12. Split/reduced locals. courier2 exit17 and
  courier3 exit10 exposed header offsets and a test clobbering the packet output
  through its own check printer. courier4 and fresh replays pass.
* intent1/2, view1, paint1: error12; simplified functions/condition temporaries.
* scene1 native exit1: mood token indexing; corrected. scene2 passes.
* allen1: error7, function `want` conflicted with local name; renamed jobwant.
  allen2: error7 (write fd type); allen3: error3 (text declaration order).
* replay1: error10 (scene parameter `start` conflicted with runtime function).
* replay2 printed PASS with session mutant exit0: INVALID acceptance receipt.
  Found a weak test corrupting header syntax instead of session digits and an
  external shell AND-list missing explicit failure. Corrected both. replay3+
  reject the session mutant with nonzero native behavioral assertion failures.

Behavioral RED records (valid compiled static AArch64 ELFs): runtime stubs exit15;
intent-edge-red exit1; load-edge-red exit2. Their exact combined Omega sources
and native assertion output are committed here. Remaining tests were incremental
regression development; no claim every module had a pre-implementation RED.

Original quiet-build reports its ~/.local/state log cannot be written in the
sandbox. It still runs the compiler while holding its shared lock; errors are
preserved in bootstrap/replay/strict/clippy logs. Compiler/builder exit statuses
are enforced in gate.sh. Empty/nonstatic/wrong-machine outputs cannot proceed.

Publication attempts: git add/commit at original metadata fail with read-only
index.lock under aienos-imac-allen/.git/worktrees/aienos-allen-omega-L694.
`git push origin feat/allen-omega-L694` fails: cannot resolve github.com.
No approval bypass or service/network changes attempted. Separate publication
metadata under the permitted handoff root records this same worktree's commits;
it is not another worktree or implementation worker.
