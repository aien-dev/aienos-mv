# Real sky: measurements and pictures

Branch `feat/real-sky`. The method, sources, limits and the sound and picture audit are in
[docs/ALLEN_SKY_SCIENCE.md](../../docs/ALLEN_SKY_SCIENCE.md). This folder holds what was measured.
Nothing here was measured on the iMac.

## Pictures (chosen build: Gaia DR3 level 7, Milky Way gain 1.2; 3840x2160 frame model)

| file | what it shows |
|---|---|
| `sky-whole.png` | the whole chosen view, scaled to 1920x1080: stars, the Milky Way band, dark sky |
| `sky-centre-1to1.png` | 1280x720 at full size, centred on the GW150914 position |
| `sky-galaxy0-1to1.png` | the Large Magellanic Cloud's box at full size. The cloud is barely visible (see the science document, section 3) |
| `sky-galaxy1-1to1.png` | the Small Magellanic Cloud's box at full size. Barely visible, same reason |
| `sky-brightest-1to1.png` | the brightest star at full size, with its glow |
| `gain-3-whole.png`, `gain-5-whole.png` | the same view with the Milky Way at gain 3 and 5, for comparison (not chosen) |

Made by `scripts/ck_sky_test.sh` (the chosen build) and by the test binaries built from the
generated tables at each gain (see `gain-measure.txt`).

## Files

- `gain-measure.txt`: the numbers behind the gain. The chosen gain is the one that makes the rendered
  luminance ratio between the brightest Milky Way band and the dark sky equal the physical 678 / 205 = 3.31
  (Galactic-centre view). Gain 1.2 gives 3.23.
- `generator-report.txt`: the report of the chosen generator run (inputs with sha256, the sky's peak and
  90 % region, star counts, photometry, table sizes).
- `sky-test.log`: the host sky test, 13 checks, 0 failed.
- `allen-ui-test.log`: the Allen window test and its picture checks; the place test (26 checks) is
  run by the same script.
- `chirp-audit.txt`: the sound and picture numbers set against GW150914 (see section 5 of the science document).

## Sizes

| table | bytes |
|---|---|
| stars in view (1035 of 8404 to V 6.5) | 8280 |
| Milky Way luma | 64980 |
| Milky Way colour | 32580 |
| `sky_data.c` as source text (level 7, and level 6) | 452276 |
| `IMACKERN.EFI` (built from `962ee43` plus this change; see below) | 405504 |

## Timings (Spark, one core, shared machine: not a quiet-lock timing, not the iMac)

- The whole sky at 3840x2160 (`ck_night_init`, best of 3 per run): level 6 135 to 136 ms; level 7 109 to 140 ms.
  The ranges overlap: no difference between the levels can be claimed.
- Generator, level 7 (joins four Gaia chunks, 196608 cells): 72 to 94 s. Level 6: about 2 s.

## What was not done

- The Magellanic Clouds are not visible: their light was in the removed photograph and nothing measured has replaced it.
- The Gaia grid at the LMC's position is no brighter than the dark sky (sRGB luma 4 to 5), which is not what the LMC is.
- The grid is 1 degree; a 0.5 degree grid is not built.
- Nothing was run on the iMac's screen; the gain is calibrated on the host's sRGB path.
- The full `make -C native/kernel test` suite and the QEMU tests were not run here.
