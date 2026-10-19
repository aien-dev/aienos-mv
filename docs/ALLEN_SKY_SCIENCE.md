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
| Milky Way | Real photograph of the real sky, stars removed, tied to a published brightness measurement, shown brighter than true (declared, section 3). |
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
| "The Milky Way panorama", ESO/S. Brunier, eso0932a | A 6000 x 3000 photograph of the whole sky, galactic longitude and latitude | CC BY 4.0. Credit: ESO/S. Brunier. |
| OpenNGC (Mattia Verga), pinned to commit be3cd6c | Galaxy positions, sizes, tilt angles | CC BY-SA 4.0. Credit: Mattia Verga. Only two galaxies (their positions and sizes, which are facts) reach the tables. The ShareAlike term against this repository's licence (AGPL-3.0-or-later) is for the desk to clear. |
| GWTC-1 sky map of GW150914, LIGO-P1800381-v7 | Where on the sky the signal came from (HEALPix probability map) | Released by LIGO and Virgo through the LIGO Document Control Center. Credit: LIGO Scientific Collaboration and Virgo Collaboration. |
| Abbott et al., PRL 116, 061102 (2016) | The published numbers the audit measures against | Cited only. |

Why the bright-star catalogue and not Hipparcos, Tycho or Gaia: those carry
CC BY-NC 3.0 IGO (non-commercial; ESA asks for a request before commercial use).
This project's licence has no such limit. The Yale catalogue is complete to
about V 6.5, which is the naked-eye limit, so for a picture meant to look like
what a person sees, nothing visible is missing (the section 3 limits say what
is).

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

**Milky Way.** The ESO photograph is registered against the catalogue: of 92
stars brighter than V 2.5 found in the photograph, 81 sit within 2.5 pixels of
the catalogue position after a median shift of (0.12, 0.35) px, rms 1.55 px
(1 pixel is 0.06 degrees). So the photograph's geometry is right to about 0.1
degree. The photograph's stars are then removed (grey opening over 7 pixels,
then a 2.5 pixel blur) because the catalogue draws those, sharp. The rest is
the light of the stars too faint and many to separate, the glowing gas and the
dark dust lanes.

The photograph is not photometric. Measured on 2920 catalogue stars of V 3.5 to
6.0 away from the Milky Way's middle, the photograph's light of a star rises as
(its true light) to the power 0.03 where a photometric image would give 1.0
(scatter 1.2 magnitude). It is a stretched picture. So its brightness is not
read from it. It is tied to a measurement instead: the Bochum V photometry of
the southern Milky Way (Leinert et al. 1998, A&AS 127, 1, section 10, fig. 65;
scale tops out at 900 S10(V), one S10 being one 10th magnitude star per square
degree), the view's brightest Milky Way being set to that value and the
photograph's darkest sky to nothing. The dark sky's own glow (airglow plus
zodiacal light at a dark site) is added at V 22.0 magnitudes per square arcsecond
(205 S10; Leinert et al. 1998).

The declared untruth: all that diffuse light is multiplied by **5** (`--boost`)
against the stars. At true contrast (boost 1) the Milky Way is nearly invisible
on a screen at 2.5 arcminute per design pixel; the eye sees it on a dark night
because it is large and the eye integrates over area and has adapted for half an
hour. A screen in a lit room does not. Boost 5 was chosen by looking at boost
1, 3 and 5 frames (on the Spark in `~/handoffs/real-sky/`, not committed); it is one number in
`scripts/ck_sky_gen.sh` (`SKY_BOOST`) and printed in the report.

The Milky Way is stored as a 241 x 152 grid (0.40 degree apart) and drawn at a
quarter of the screen's resolution with a 4 x 4 ordered dither. It is smooth in
the sky anyway. Its colour is stored at half that again.

**Galaxies.** Brighter than V 6.5 and in the view, OpenNGC has two: the Large
Magellanic Cloud (ESO056-115, V 0.29, 646 x 550 arcminutes, position angle 170)
and the Small Magellanic Cloud (NGC0292, V 2.30, 300 x 180 arcminutes, position
angle 45). They are placed at their true centre, size and tilt; their light
comes from the photograph. On a 3840 wide screen the LMC's half axes are 219 x
191 px, the SMC's 112 x 74.

**The sky above you now (not wired).** `sky_real.c` also holds Greenwich mean
sidereal time (IAU 1982), the zenith for a latitude and longitude, and the 1976
IAU precession back to J2000, so a later change can show the sky over the
machine. It is off unless the build sets a location. It is tested against
published sidereal times in `ui/tests/sky_test.c`.

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

Sizes, timings and pictures are in `evidence/real-sky/README.md`. In short, on a host with one core: the whole sky at 3840x2160 is built in 275.8 ms (best of 3); the data tables are 73.8 KB; the iMac kernel image is 348160 bytes (about 65536 more than the invented sky); the 13 sky checks and 91 picture checks pass. Nothing here was measured on the iMac itself.

## 9. Open

- The real chirp (section 5): a decision for Drake.
- The OpenNGC ShareAlike term against the repository licence.
- The ripple and the ringing should carry an "illustration" label wherever
  shown to the public.
- The sky was checked on a host, not on the iMac's real screen.
