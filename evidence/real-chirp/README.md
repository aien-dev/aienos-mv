# Real chirp: the sound of GW150914 as published

Branch `feat/real-chirp`, on top of `feat/real-sky`. It changes only `ui/chirp.h`, `ui/chirp.c` and the two chirp
tests. It is a decision for Drake: the real sound is lower and quieter than the current invented one.

| | current kernel sound | this branch | GW150914 as published |
|---|---|---|---|
| pitch at the start | 200 Hz | 35 Hz | 35 Hz |
| pitch at the top | 1000 Hz | 250 Hz | about 250 Hz |
| length of the sweep | 592 ms | 164 ms | about 200 ms |
| cycles in the sweep | 179 | 9 | about 8 to 10 |
| ringing after the merger | 30 ms held at 1000 Hz | decays with a 4.0 ms time constant, held at 250 Hz | 4.0 ms at 250 Hz (final mass 62, spin 0.67) |

- `chirp-audit.txt`: the measured numbers and the physics side by side (the same audit as `evidence/real-sky/chirp-audit.txt`, run on this branch).
- `chirp-test.log`: the chirp host test, 26 checks, PASS.

Caveat: small speakers play almost nothing at 35 Hz, so the start of the real sweep is felt more than heard on the iMac.
The test's loudest-sample check was relaxed to 75 % of the peak level because the real ring is short (0.9 ms plateau, 4 ms decay).
Nothing was listened to on the real iMac speakers.
