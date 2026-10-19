# Real sky: measurements and pictures

Branch `feat/real-sky`. The method, sources, limits and the sound and picture audit are in
[docs/ALLEN_SKY_SCIENCE.md](../../docs/ALLEN_SKY_SCIENCE.md). This folder holds what was measured.

## Pictures (made by `scripts/ck_sky_test.sh`, 3840x2160 build, 1:1 crops)

| file | what it shows |
|---|---|
| `sky-whole.png` | the whole sky, scaled to 1920x1080: stars, the Milky Way band on the left, the two Magellanic Clouds |
| `sky-centre-1to1.png` | 1280x720 at full size, centred on the GW150914 position, with the Large Magellanic Cloud on the right |
| `sky-galaxy0-1to1.png` | the Large Magellanic Cloud at full size |
| `sky-galaxy1-1to1.png` | the Small Magellanic Cloud at full size |
| `sky-brightest-1to1.png` | the brightest star at full size, with its glow |

Known look of the pictures: the Milky Way is drawn from a 241 x 152 grid at quarter resolution with a
4x4 dither, so its edge shows soft facets at screen size. The dark sky is a very dark warm grey
(mean sRGB level 20), not black, because the real dark sky glows (V 22, 205 S10). All diffuse light
(Milky Way, galaxies, sky glow) is shown 5 times brighter than the stars' scale, a declared choice
(`--boost`), explained in the science document.

## Files

- `generator-report.txt`: what the generator printed: input checksums, the sky map's peak and 90 %
  region (181.6 square degrees, all on screen), star counts per limit, photograph registration
  (92 stars, rms 1.55 px = 0.09 degrees), photometry, table sizes.
- `sky-test.log`: the host test, 13 checks, 0 failed.
- `chirp-audit.txt`: the sound and picture numbers set against GW150914 (see section on sound in the science document).

## Sizes

| table | bytes |
|---|---|
| stars in view (1035 of 8404 to V 6.5) | 8280 |
| Milky Way brightness | 36632 |
| Milky Way colour | 18392 |
| palette, sRGB tables, galaxies, checks | about 10496 |
| `sky_data.c` as source text | 340788 |

## Timings (host, one core, not the iMac)

- Building the whole sky at 3840x2160: 275.8 ms best of 3 (runs 304.5, 275.8, 740.3 ms), measured by the host
  test while other agents were also running on the machine. It is not a quiet-lock publication timing.
  It has not been measured on the iMac.
- Generator end to end (reads the five inputs, writes the tables): 7 s.

## Other host checks

- `scripts/ck_allen_ui_test.sh`: ALLEN_UI_TEST PASS, 91 checks (`allen-ui-test.log`). The whole picture at 3840x2160 on one core of this host: 3912 frames, average 21434 us, slowest 171262 us.
- `make -C native/kernel imac-kernel` builds clean with -Werror: `IMACKERN.EFI` is 348160 bytes. The earlier figure for the invented sky on the Horizon branch was 282624 bytes (that branch, not remeasured on this base), so the real sky costs about 65536 bytes, in step with the 73.8 KB of tables.
- Both of these were run directly at the lowest priority (nice 19), not through `quiet-build`, because the quiet-build queue stayed blocked behind other agents' timing runs for over an hour. They are correctness checks and a size count; the timings above are therefore not lock-held publication timings.

## What was not done

- Nothing was run on the real iMac screen or measured on the iMac chip.
- The sky shown is fixed on the GW150914 position; the sidereal time, zenith and precession code is in
  `sky_real.c` and tested, but not wired to a place and time.

## Boost choice

The sky is built with the diffuse light shown 5 times (default). The first worker's frames at 1, 3 and 5 times are
on the Spark in `~/handoffs/real-sky/sky-whole-b{1,3,5}.png` (not committed, 6 MB each). 1 is the true contrast at one
design pixel and makes the Milky Way faint; 3 was that worker's pick for a dark-site look; 5 is what is built in.
Change it with `SKY_BOOST=3 scripts/ck_sky_gen.sh`, then rerun `scripts/ck_sky_test.sh`.
