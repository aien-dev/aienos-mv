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

## What was not done

- Nothing was run on the real iMac screen.
- The sky shown is fixed on the GW150914 position; the sidereal time, zenith and precession code is in
  `sky_real.c` and tested, but not wired to a place and time.
