# Allen's sky and sound: the science audit

Owner's rule (Drake, 2026-10-19): the whole idea of this system is grounded in
real astrophysics. The merger sound is there because it is real. The sky behind
Allen must be the real sky, not an invented one.

This note says, for every picture and sound in Allen's startup, what is real,
what is a simplification, and what is only an illustration. Every number comes
from a source that is named, or from a measurement that is in `evidence/real-sky/`.
Where a source says nothing, that is said too. The code is in
`native/kernel/ui/` (`night.c`, `sky_real.c`, `sky_data.c`, `chirp.c`).

Words: "design pixel" is one pixel of a 1920 wide screen; the picture scales
from there. V is the visual magnitude of a star (smaller is brighter, 6.5 is
about the faintest star a person sees on a dark night). B-V is a star's colour
number (smaller is bluer).

## 1. The verdict, one line each

| Part | Verdict |
|---|---|
| Stars (9096 in the catalogue, 8404 to V 6.5) | Real: position, brightness, colour. Limits in section 3. |
| Milky Way | Real: Gaia DR3 star counts and G-band flux per HEALPix cell, scaled so the brightest band is the published 20.7 V mag/arcsec2 (Leinert et al. 1998). Shown at that brightness, calibrated to the physical contrast over the sky (gain 1.2, section 3). |
| Dark sky glow | Real: V 22.0 mag/arcsec2, the published dark-site sky (Leinert et al. 1998). Not boosted. |
| Large and Small Magellanic Clouds | Real, at true place, size and tilt. |
| The view | Real: a pinhole-camera projection centred on GW150914's most probable sky position. |
| Starlight bent by the black holes | Real physics (point-mass lens), with one invented edge and one invented scale (section 4). |
| Black hole shadows and the thin ring | Real size rule (shadow radius sqrt(27)/2 Rs), drawn as a simple disc and ring. |
| Orbit and spiral | Real law (separation as t^(1/4), angle as t^(5/8)), with time compressed and the number of turns cut (section 4). |
| Merger and the new, larger hole | Right to within 5 percent (section 4). |
| The ripple across the sky | Illustration only. A real wave cannot be seen. |
| Ringing of the final hole | Illustration, wrong timescale (section 4). |
| The startup sound | The law for the pitch is real. The sweep, the cycle count and the ring are not the real signal (section 5). |
| Galaxies' sweeping light, twinkle, Allen's mind, the water | Art, not physics (section 6). |

## 2. Sources

| Input | What it gives | Licence and credit |
|---|---|---|
| Yale Bright Star Catalogue, 5th revised ed. (Hoffleit and Warren 1991), CDS V/50 | 9096 stars with J2000 position, proper motion, V, B-V | The CDS ReadMe carries no licence or copyright line. Credit: Hoffleit and Warren 1991, via CDS Strasbourg. |
| Gaia DR3 `gaia_source`, query 1 below (ESA/Gaia/DPAC) | Source count and summed G, BP, RP mean flux of every source with G > 7, per HEALPix level-7 cell (nside 128, 196608 cells) | CC BY-NC 3.0 IGO. Credit: ESA/Gaia/DPAC. Pinned sha256 in `scripts/ck_sky_gen.sh`. |
| Gaia DR3 `astrophysical_parameters`, query 2 below | Mean G-band extinction `ag_gspphot` per the same level-7 cells (the dust) | CC BY-NC 3.0 IGO. Credit: ESA/Gaia/DPAC. |
| Leinert et al. 1998 (A&AS 127, 1), as quoted in arXiv:2401.15518 (Bochum V 20.7 at the bright Milky Way) and arXiv:2604.09427 (dark-sky reference V 22) | The two physical brightnesses the display is tied to | Cited only: published numbers, no data copied. |
| OpenNGC (Mattia Verga), pinned to commit be3cd6c | Galaxy positions, sizes, tilt angles | CC BY-SA 4.0. Credit: Mattia Verga. Only two galaxies (their positions and sizes, which are facts) reach the tables. The ShareAlike term against this repository's licence (AGPL-3.0-or-later) is for the desk to clear. |
| GWTC-1 sky map of GW150914, LIGO-P1800381-v7 | Where on the sky the signal came from (HEALPix probability map) | Released by LIGO and Virgo through the LIGO Document Control Center. Credit: LIGO Scientific Collaboration and Virgo Collaboration. |
| Abbott et al., PRL 116, 061102 (2016) | The published numbers the audit measures against | Cited only. |

Why Gaia DR3 for the Milky Way: it is the only measured, whole-sky, optical
source-count and flux data at hand, and the owner's direction (2026-10-19) is
to compute the Milky Way from measured data. CC BY-NC 3.0 IGO allows
non-commercial use with credit (ESA/Gaia/DPAC); this project is non-commercial
(AGPL-3.0-or-later). Tables are compiled in, so a commercial use would need
ESA's agreement, noted for the desk.

**The two Gaia queries (run on 2026-10-19 against https://gea.esac.esa.int/tap-server/tap/async, ADQL).**
Level 7 is run in four HEALPix ranges of 49152 cells each (the TAP service returns at most about
50000 rows per job), the four results joined with one header. The script does this when it is
asked for level 7 (`SKY_HPX_LEVEL`, default 7) and fetches any chunk that is missing.

Query 1 (flux per cell), chunk c of 4 covers cells (c-1) x 49152 to c x 49152 - 1, output
`gaia_hpx7_flux_c.csv`; the joined table `gaia_hpx7.csv` has sha256
`4d1412d3df02f6ee34120b8fd9b96bde6766fcaf50c569a94b45b72d4ec151cd` (196608 rows):

```sql
SELECT GAIA_HEALPIX_INDEX(7, source_id) AS hpx7, COUNT(*) AS nsrc, SUM(phot_g_mean_flux) AS g_flux,
       SUM(phot_bp_mean_flux) AS bp_flux, SUM(phot_rp_mean_flux) AS rp_flux
FROM gaiadr3.gaia_source
WHERE phot_g_mean_flux IS NOT NULL AND phot_g_mean_mag > 7.0
  AND GAIA_HEALPIX_INDEX(7, source_id) BETWEEN LO AND HI
GROUP BY hpx7 ORDER BY hpx7
```

Query 2 (dust per cell), the same four chunks, output `gaia_dust7.csv`, sha256
`ce46146f06fa33791d81cde3f02f19a7ac259e5555b13c03cdb8092e75174d89`:

```sql
SELECT GAIA_HEALPIX_INDEX(7, source_id) AS hpx7, AVG(ag_gspphot) AS mean_ag, COUNT(*) AS n
FROM gaiadr3.astrophysical_parameters
WHERE ag_gspphot IS NOT NULL
  AND GAIA_HEALPIX_INDEX(7, source_id) BETWEEN LO AND HI
GROUP BY hpx7 ORDER BY hpx7
```

Level 6 (49152 cells, the earlier build) is still in the script (`SKY_HPX_LEVEL=6`, sha256
`334bde2b...` and `105f8f51...`). Each sha256 is of the CSV the TAP service returned (the joined
file for level 7); the script refuses any other file.

Each input is downloaded once by `scripts/ck_sky_gen.sh`, checked against a
sha256 written in the script (a file that does not match is refused), and
turned into tables by `native/kernel/tools/ck_sky_gen.c` (plain C, no Python).
The kernel never touches the network: the tables are compiled in.
`native/kernel/ui/sky_data.txt` is the generator's report from the last run.

## 3. The sky

**The view.** A gnomonic projection (what a pinhole camera sees), 80 degrees of
sky across the screen's width, north up, centred where the GW150914 sky map is
most probable. That point is the highest-probability pixel of the map (HEALPix
nside 512): right ascension 113.697, declination -72.757 degrees, in the
constellation Volans near Carina. In galactic coordinates it is l 284.54, b
-22.70. The map's 90 percent region is 181.6 square degrees, all of it on a
3840 x 2160 screen. The centre is the map's peak, not a point source: the real
signal could have come from anywhere in that region. The Large Magellanic Cloud
is inside it.

**Stars.** Position is the catalogue's J2000 position moved along its proper
motion to epoch 2026.8. Brightness: light from a star is proportional to
10^(-0.4 V), the definition of the magnitude scale. Colour: B-V goes to an
effective temperature by Ballesteros (2012, EPL 97, 34008), that temperature to
the colour of a black body through the CIE 1931 observer (Wyman, Sloan and
Shirley 2013, fit), then to sRGB. Checks the generator prints: B-V -0.30 is
16602 K and sRGB (177, 198, 255); B-V 0.65 is 5778 K (the Sun's) and (255, 241,
234); B-V 1.50 is 3794 K and (255, 207, 155). 310 catalogue stars have no B-V
and are drawn at 0.6, close to the Sun's. On a 3840 x 2160 screen the view holds
71 stars to V 4, 179 to V 5, 560 to V 6, 904 to V 6.5.

Limits, stated plainly:
- No star fainter than V 6.5 is drawn one by one. The faint ones are inside the
  Milky Way light (below).
- Stars are points drawn with a small core (1.2 design pixel). A real star is
  smaller than any screen can show; a bright one looks bigger only because of
  the eye or the camera. The drawn size is a display choice, not a measurement.
- A screen cannot show the real brightness range (a magnitude 0 star is about
  250 times brighter than V 6.0). The display level for V 6.5 is a chosen
  constant (0.06 linear, sRGB level 69) and everything else follows from the
  magnitude law. Bright stars therefore clip at the top.
- No atmosphere: no extinction near the horizon, no airglow colour, no moon.
  (The faint sky glow of an extremely dark site is added, section below.)

**Milky Way.** The light is Gaia DR3's own: for each of the 196608 HEALPix level-7 cells
(about 0.46 degree across), the summed G-band flux of the stars in it (stars with G > 7).
That is light, not a photograph. The cells are splatted onto a full-sphere 360 x 180 grid
(1 degree, north up) within 1.5 degrees of each cell's centre and averaged.

Colour and luminance are kept apart. A cell's light is split between the red, green and blue
channels by a tint from its BP-RP colour (reddened by the mean G-band extinction,
`ag_gspphot`, E(B-V) = A_G / 2.285, Cardelli et al. 1989), with the channel weights chosen so
that their luminance (0.2126 R + 0.7152 G + 0.0722 B) is exactly the cell's light. The first build
put almost all of the light into the red channel, so the band was about a third of its
true luminance. That is corrected and measured below.

Scale: the number is a count of light, so it has no brightness until it is tied to one. It is
tied to the brightest band, not to a photograph:

- The brightest band of the Milky Way (the Sagittarius and Scutum bulge) is V 20.7 mag/arcsec2
  (Bochum photometry, Leinert et al. 1998; the same value is quoted in arXiv:2401.15518, Table 3).
  That is 678 S10 (1 S10 is the light of a V 10 star per square degree, 27.78 V mag/arcsec2 by
  definition). The generator divides the grid by its brightest cell and multiplies by 678 S10.
- The dark sky's own glow is V 22.0 mag/arcsec2 (205 S10; Leinert et al. 1998, also the reference
  V 22 of arXiv:2604.09427). It is added everywhere, with no gain.
- Physical contrast of the brightest band over the dark sky: 678 / 205 = 3.31 (luminance).

**The gain (`SKY_GAIN` / `--gain`, chosen 1.2).** One factor on the Milky Way's light only.
Rule: the luminance ratio the screen shows between the brightest band and the dark sky must equal
the physical 3.31. Measured on the Galactic-centre view (80 degrees across, 3840 x 2160, the peak of
the Milky Way grid at the centre of the frame, `evidence/real-sky/gain-measure.txt`), the ratio is
not 3.31 at gain 1: sRGB is 8-bit and gamma-coded, and the sky is only 4 to 5 levels, so the
rendered band is darker than the physical one.

| gain | on-screen added light over sky (luminance), Galactic-centre view | physical |
|---|---|---|
| 1.0 | 2.59 (L6 build; L7 gives the same to 2 places at gain 1.2) | 3.31 |
| 1.2 | 3.23 (L7, chosen) | 3.31 |
| 1.3 | 3.57 | 3.31 |
| 1.5 | 3.93 | 3.31 |
| 3.0 | 6.72 (L7) | 3.31 |
| 5.0 | 11.23 (L7) | 3.31 |

Gain 1.2 is the closest of the tested values (within 3 percent; the steps between 1.2 and 1.4 are
quantisation, not a trend). So the chosen gain is **1.2**. The gain is not a taste choice: it is
the value that makes the rendered ratio equal the physical one.

Earlier build (49ab287): `--boost 5` multiplied the sky glow by 5 (to V 20.3, far brighter than any
dark site) and left the Milky Way unchanged. The Milky Way was barely above its own sky, which is
the faintness this change fixes. The first version of this generator also scaled the Milky Way to
its 95th-percentile cell, which put the Galactic centre at 18.6 V mag/arcsec2, about 2 magnitudes
brighter than the published band (20.7). Both are corrected.

**What the screen shows, measured (3840 x 2160, the frames in `evidence/real-sky/`):**

| build | dark sky (dimmest cell), sRGB R | brightest cell in the chosen view, sRGB R | added light over sky, chosen view (luminance) | added light over sky, Galactic-centre view (luminance) |
|---|---|---|---|---|
| 49ab287 (before, boost 5 on the sky) | 21.5 | 38.6 | 0.61 | 1.46 |
| L7, gain 1.2 (chosen) | 4.5 | 11.0 | 1.49 | 3.23 |
| L7, gain 3 | 5.5 | 18.6 | 3.05 | 6.72 |
| L7, gain 5 | 5.5 | 25.8 | 5.49 | 11.23 |

"Added light over sky" is (band luminance minus sky luminance) over sky luminance, the same
quantity as the physical 678 / 205 = 3.31. The Galactic-centre column is the measured check of the
rule, and the one the gain was chosen by. Physical values for comparison are in the table above.

The chosen view is centred on GW150914 (galactic latitude -23 degrees), so its brightest cell is
dimmer than the Galactic centre.
A magnitude 6.5 star's centre is sRGB 69. A screen cannot show the whole range (section 3,
limits), so the stars are the top of the picture and the Milky Way sits below them at its physical
level. Frames: `evidence/real-sky/sky-whole.png` and `sky-centre-1to1.png` (chosen), with
`gain-3-whole.png` and `gain-5-whole.png` for comparison.



**Galaxies.** Brighter than V 6.5 and in the view, OpenNGC has two: the Large
Magellanic Cloud (ESO056-115, V 0.29, 646 x 550 arcminutes, position angle 170)
and the Small Magellanic Cloud (NGC0292, V 2.30, 300 x 180 arcminutes, position
angle 45). They are placed at their true centre, size and tilt. On a 3840 wide
screen the LMC's half axes are 219 x 191 px, the SMC's 112 x 74.

**Not done, stated plainly: the Magellanic Clouds are not visible.** Their light used
to come from the ESO photograph, which is removed. Nothing measured replaces it yet: the
galaxy is drawn as an elliptical profile with a tint and no brightness from data. Measured on
the frame at gain 1.2, the LMC's box averages sRGB R 6.6 against a sky of 4.5 (G 4.4, B 3.8 for
the SMC box at 5.0), so both are barely above the sky. The Gaia cells should already hold the
clouds' light (their stars are in the catalogue), but at the LMC's centre the Milky Way grid is
no brighter than the dark sky (sRGB luma 4 to 5). The LMC should not be that dark. Its
integrated V 0.29 over its ellipse (area pi/4 x 646 x 550 arcmin2 = 1.0e9 arcsec2) gives a mean
surface brightness of 0.29 + 2.5 log10(1.0e9) = V 22.8 mag/arcsec2, about twice the dark sky's
light: a physical LMC is a faint glow of about 1.1 times the sky's light added, similar to the
Milky Way band in the chosen view (1.5). So the grid, or its use, is a fault to chase before the
clouds can be drawn from data. Open.

**The sky above you (wired).** `sky_real.c` holds Greenwich mean sidereal time (IAU 1982),
the zenith for a latitude and longitude, and the 1976 IAU precession back to J2000
(`ui/tests/sky_test.c` checks them against published values). Allen takes a place in two ways:

- a town name from a table of 20 (`memphis`, `chicago`, ...), as before;
- coordinates typed as: `35 north 90 west`, `35.1 n, 90.0 w`, `35.1, -90.0`, `lat 35.1 lon -90`,
  `-33.87 151.21`, `north 35 west 90`. The parser reads the raw line (the folded one has lost the
  points and minus signs), in integer arithmetic, in millionths of a degree. A bare pair of numbers,
  arithmetic, a name, or a number with an operator is not a place. Tested in
  `native/kernel/svc/tests/allen_place_test.c` (26 checks, run by `scripts/ck_allen_ui_test.sh`).

"sky above me" then shows the sky over that place, turning at Earth's rate (the screen
refreshes every 10 seconds, `allen_ui.c`). Coordinates set the place only; the mode is unchanged.

## 4. The black holes and the picture of the merger

Measured by `ui/tests/chirp_audit.c`, output in `evidence/real-sky/chirp-audit.txt`.
Published values used: source-frame masses 36 and 29 solar masses, final mass 62,
final spin 0.67, redshift 0.09, luminosity distance 410 Mpc (Abbott et al. 2016,
PRL 116, 061102, table I; abstract).

**Bent light (real physics).** A point mass moves the image of a star seen at
angle theta from it, to where the star really is, beta = theta - E^2/theta. The
code does exactly that (`warp()` in `night.c`), with the pull of both holes
added. Two weak points, both ours:
- The pull is eased to zero between 2.2 E and 4.4 E so the bent region has no
  edge. The real pull has no edge: it falls as 1/theta and never stops. What the
  picture leaves out is 27.3 design px at 2.2 E and 13.6 px at 4.4 E (E is 60 px).
  Far from the holes the sky is not bent here and would be, slightly.
- The scale. A hole's shadow has radius sqrt(27)/2 = 2.598 Rs (Rs is the
  Schwarzschild radius, 2GM/c^2). The Einstein radius for a far-away source is
  sqrt(2 Rs / D) in angle against the shadow's 2.598 Rs / D, for a viewer at
  distance D. The picture's ratio of Einstein radius to shadow radius is 3.00
  (60 over 20 px). That is exactly what a viewer 30.4 Rs from the hole sees:
  3229 km from a 36 solar mass hole. It is a consistent picture, but of a viewer
  who could not survive there. It is a chosen viewpoint, close enough to make
  the arcs big.
- Superposing two single-hole pulls is accurate only when the pull is weak. Near
  a hole the picture draws the shadow and a thin bright ring at 1.04 of its
  radius, as a simple disc and a ring. Real strong lensing has a photon ring and
  an infinite stack of thinner copies of the sky. Not drawn.

**Orbit and spiral (real law, compressed).** Separation as t^(1/4), angle as
t^(5/8), t the time left (leading-order inspiral, Peters 1964; Abbott et al.
2016). The picture's wide orbit is 270 design px, which with 1 Rs = 7.70 px is
35.1 Rs of a single hole, or 35.1 GM/c^2 of the pair. Physics there: orbital
period 0.46 s, gravitational-wave pitch 4.4 Hz, 42 s to the merger, about 150
turns. The picture: one orbit per 5.2 s (11.4 times slower at the start), 6 s
to the merger (7 times faster in total), 1.85 turns. So the law is real, the
clock is not. The orbit is drawn leaning 60 degrees away from face on (its
height is half its width).

**Merger.** Equal masses are drawn; the real ones are 36 and 29 (ratio 0.8, close
enough for a picture, but a real unequal pair is lopsided). The final hole is
drawn with a shadow 2.00 times each initial one; the published masses give
62 over 32.5 = 1.91, so the picture is 4.8 percent big. Its Einstein radius grows
1.414 times (square root of 2) where the square root of 1.91 is 1.381, the same
gap. The real remnant spins at 0.67, which squashes and shifts its shadow;
the picture draws a round, unspun one.

**The ripple across the sky (illustration).** The wave from GW150914 stretches
space by about 10^-21 (peak strain 1.0e-21), a shift that no eye or telescope
could see, and it does not move the stars across the sky. The picture draws a
ring of stretch and squeeze with a cos 2 theta pattern, which is the true
shape of a gravitational wave's effect on a ring of free particles, at a size
about 20 orders of magnitude too large, running out from the merger over 3.6 s
as if the wave were visible light crossing the screen. It stays because it makes
the idea visible. It should be called an illustration wherever it is shown.

**Ringing (illustration, wrong timescale).** The real final hole rings in its
least damped mode at 250 Hz (Berti, Cardoso and Will 2006 fit with mass 62
(detector frame 67.6), spin 0.67; matches the published 250 Hz top of the sweep)
with a quality factor of 3.17, which is a damping time of 4.0 ms: gone in about
two cycles. The picture's shadow wobbles with a 95 ms period and a 260 ms decay time: 2.7
cycles per decay time where the real one has 1. A real ringdown is too quick to
see at all; the picture slows the period 24 times and the decay 65 times. Shown as a wobble for the same
reason as the ripple.

## 5. The startup sound

`ui/chirp.c`, a "blirp" under a second, built from a formula and no recording.

Real: the pitch of an inspiral rises as (time left)^(-3/8) and the strength of
the wave as pitch^(2/3). The code uses exactly those, and the peak is at the
merger.

Measured against the real signal (`evidence/real-sky/chirp-audit.txt`):

| | Published and physics | The kernel's sound |
|---|---|---|
| Pitch at the start | 35 Hz | 200 Hz (5.7 times higher) |
| Pitch at the top | about 250 Hz | 1000 Hz (4.0 times higher) |
| Sweep length | about 0.2 s (0.16 s by the leading-order law) | 0.59 s (3 times longer) |
| Cycles | about 8 from 35 to 150 Hz (8.4 by the law) | 179 |
| Ring after the merger | 250 Hz, gone in 4.0 ms | 1000 Hz held, 30.0 ms time constant |

The sound is therefore the right kind of curve with invented numbers: higher
(raised into hearing, but by 5.7 at the start and 4.0 at the top, not one
factor), longer, with about twenty times the cycles, and a ring that is held at
the peak pitch for far longer than the real one lasts. A single factor
would be a real thing: a gravitational-wave signal scales exactly with
the total mass, so a lighter pair would give higher pitch and a shorter
chirp. The kernel's sound is not any one pair's.

What a real version would be: the signal as it is, 35 up to 250 Hz over about
0.2 s with its eight cycles, then a 250 Hz ring dying in 4 ms. It is audible
(it is what LIGO's own audio of the event plays), but it is low and short,
and small speakers do not reproduce 35 Hz. That choice, real and quiet against
louder and invented, is Drake's. Work for it is a separate branch, `feat/real-chirp`.

## 6. Everything else on the screen

- **Twinkle.** The brightest stars (V 2.0 and brighter) twinkle. Twinkling
  (scintillation) is the atmosphere, so this is correct for an observer on the
  ground and wrong for one in space. It is the observer the sky already implies.
- **The sweeping band of light on the galaxies** while Allen answers: art.
  The galaxies themselves do not change.
- **Allen's mind** (the sphere of points and threads), **the water**, **the
  orb**: art. They make no claim about the real world and say nothing about it.
- **The fade in** (the eyes adjusting to the dark, bright stars first): real in
  kind (the eye does adapt over minutes, rods take about half an hour), shown in
  seven seconds.

## 7. For the black-hole dusk (goodbye)

Notes for the branch `feat/goodbye-event-horizon`, so its picture can follow the
same rules (the physics, all standard results for a non-spinning hole):
- Shadow radius sqrt(27)/2 Rs = 2.598 Rs. The photon ring is the shadow's edge,
  at the light's critical orbit of 1.5 Rs.
- The innermost stable circular orbit is 3 Rs (6 GM/c^2), the inner edge of a
  thin disc.
- The disc's temperature falls with radius as r^(-3/4) (Shakura and Sunyaev
  1973), times (1 - sqrt(r_in/r))^(1/4) near the inner edge. A hot inner disc is
  blue-white, the outer redder. That temperature maps to colour with the same
  black body chain used for stars here (`blackbody_rgb`).
- Doppler beaming: the side of the disc turning towards the viewer is brighter.
  Intensity scales as delta^3 at one frequency and delta^4 over all, delta being
  the Doppler factor (Rybicki and Lightman).
- Light from the far side of the disc is bent over the top of the hole and
  seen above the shadow. Real pictures show this.
Only the first two are in the picture here; the rest are for the dusk.

## 8. What this change measured

Sizes, timings and pictures are in `evidence/real-sky/README.md`. On one core of the Spark (not the iMac):

- The generated table `sky_data.c` is 452276 bytes at level 7 and at level 6 (the grid is the same
  1-degree grid; only the values differ). The tables in the image are 64980 bytes of luma and 32580
  of colour, as before.
- The whole sky at 3840 x 2160 is made in 122 to 140 ms (`ck_night_init`, best of 3 per run; four
  runs at level 6 gave 135 to 136 ms, four at level 7 gave 109 to 140 ms). The ranges overlap: no
  timing difference between the levels can be claimed. The machine was shared with other jobs.
- Level 6 against level 7: same table size and the same timing within noise, so the level is chosen
  by the input. Level 7 uses all 196608 Gaia cells (the level-6 build used 49152). Its table differs
  from level 6 in 945 of its 5610 lines. The Galactic-centre luminance ratio is the same to two
  decimals (3.23 at gain 1.2).
- `IMACKERN.EFI` is 405504 bytes, built from `962ee43` plus this change (it is rebuilt after the
  commit; see the README for the final figure). The earlier real-sky figure was 348160.
- Host checks: the sky test 13 of 13 pass (`ck_sky_test.sh`); the Allen window test and its picture
  checks, the Allen place test (26 checks) and the Allen memory test are in
  `evidence/real-sky/README.md`. Not run: the full `make -C native/kernel test` suite and the QEMU
  tests; those were not run in this change.
- Nothing here was measured on the iMac.

## 9. Open

- The Magellanic Clouds have no light model since the photograph was removed, and the Gaia grid does
  not show the LMC (section 3). Fix before the clouds are called real.
- The level-7 grid is still 1 degree. A 0.5-degree grid would use the cells in full; it doubles the table
  (about 260 KB more) and has not been measured.
- The Milky Way gain 1.2 is calibrated on the host's sRGB path (8 bits, gamma). The iMac's panel may differ;
  the calibration is repeated on the iMac when a frame can be read there.
- The flight through nearby stars is a plan (`docs/SKY_FLIGHT_PLAN.md`), not code.
- Gaia DR3 is CC BY-NC 3.0 IGO; the repository is AGPL-3.0-or-later. The desk to confirm the use.
- The real chirp (section 5): a decision for Drake.
- The OpenNGC ShareAlike term against the repository licence.
- The ripple and the ringing should carry an "illustration" label wherever shown to the public.
- The sky was checked on a host, not on the iMac's real screen.
