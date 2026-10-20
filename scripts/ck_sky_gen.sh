#!/bin/sh
# ck_sky_gen.sh -- makes native/kernel/ui/sky_data.c, the real sky Allen
# opens on, from real astronomical data (see native/kernel/tools/ck_sky_gen.c
# and docs/ALLEN_SKY_SCIENCE.md for the sources, their licences and credits).
#
# Downloads each input once into $SKY_CACHE (default ~/.cache/aienos-sky),
# checks its sha256 (a file that does not match is refused), unpacks the two
# .gz files, builds the generator (plain C, needs only cc, libm, curl, gzip and
# sha256sum) and writes the C tables plus a plain-text report of what it found.
# The kernel itself never touches the network: the tables are compiled in.
#
# Settings (environment; the defaults are the committed sky):
#   SKY_RA, SKY_DEC  view centre, degrees (default: GW150914's most probable point)
#   SKY_FOV          degrees of sky across the screen's width (80)
#   SKY_ROLL         degrees the view is turned from north up (0)
#   SKY_MAG          faintest star, V magnitude (6.5, the naked-eye limit)
#   SKY_GRID         Milky Way samples across the screen's width (240)
#   SKY_EXPOSURE     linear light at the middle of a magnitude 6.5 star (see the tool)
#   SKY_BOOST        the Milky Way's light over its measured value, times (5; see the report)
#   SKY_SKYGLOW      the dark sky's own glow, V magnitudes per square arcsecond (22.0)
#   SKY_OUT          where the tables go (default native/kernel/ui/sky_data.c)
set -eu
cd "$(dirname "$0")/.."
C=${SKY_CACHE:-$HOME/.cache/aienos-sky}
OUT=${SKY_OUT:-native/kernel/ui/sky_data.c}
REPORT=${OUT%.c}.txt
mkdir -p "$C"
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_sky_gen.XXXXXX")
trap 'rm -rf "$W"' EXIT
OPEN=be3cd6c5498806a2fbb2f7e0452ef802dbb02be0 # OpenNGC commit the inputs are pinned to
: > "$W/inputs"
fetch() { # key file url sha256
    if [ ! -f "$C/$2" ] || [ "$(sha256sum "$C/$2" | cut -d' ' -f1)" != "$4" ]; then
        echo "ck_sky_gen: downloading $3"
        curl -fsSL --retry 3 -m 900 -o "$C/$2.part" "$3"
        mv "$C/$2.part" "$C/$2"
    fi
    [ "$(sha256sum "$C/$2" | cut -d' ' -f1)" = "$4" ] || { echo "ck_sky_gen: $2 does not match its sha256"; exit 1; }
    echo "input $1: $3 sha256 $4" >> "$W/inputs"
}
fetch bsc5 catalog.gz https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz 3dc44b1e90be8fbe5bcc7656032560f51275f985c7e3f783c9028e1838ec7bed
# Gaia DR3 Milky Way: full-sky HEALPix level-6 aggregate (49152 cells)
# Query: source counts + summed G/BP/RP flux (stars V>7), grouped by HPX6
# ESA/Gaia/DPAC, CC BY-NC 3.0 IGO.  Credit: ESA/Gaia/DPAC.
GAIA_TAP=https://gea.esac.esa.int/tap-server/tap/async
GAIA_Q1="SELECT GAIA_HEALPIX_INDEX(6, source_id) AS hpx6, COUNT(*) AS nsrc, SUM(phot_g_mean_flux) AS g_flux, SUM(phot_bp_mean_flux) AS bp_flux, SUM(phot_rp_mean_flux) AS rp_flux FROM gaiadr3.gaia_source WHERE phot_g_mean_flux IS NOT NULL AND phot_g_mean_mag > 7.0 GROUP BY hpx6 ORDER BY hpx6"
GAIA_Q2="SELECT GAIA_HEALPIX_INDEX(6, source_id) AS hpx6, AVG(ag_gspphot) AS mean_ag, COUNT(*) AS n FROM gaiadr3.astrophysical_parameters WHERE ag_gspphot IS NOT NULL GROUP BY hpx6 ORDER BY hpx6"
gaia_fetch() { # filename query
    if [ ! -f "$C/$1" ]; then
        echo "ck_sky_gen: querying Gaia DR3 ($1)..."
        JOB=$(curl -fsS --max-time 30 -X POST "$GAIA_TAP" \
            --data-urlencode "REQUEST=doQuery" --data-urlencode "LANG=ADQL" \
            --data-urlencode "FORMAT=csv" --data-urlencode "PHASE=RUN" \
            --data-urlencode "QUERY=$2" -w "%{redirect_url}" 2>/dev/null | tail -1)
        [ -n "$JOB" ] || { echo "ck_sky_gen: Gaia TAP job submission failed"; exit 1; }
        for i in $(seq 1 120); do
            PHASE=$(curl -fsS --max-time 10 "$JOB/phase" 2>/dev/null)
            case "$PHASE" in COMPLETED) break ;; ERROR|ABORTED) echo "ck_sky_gen: Gaia job $PHASE"; exit 1 ;; esac
            sleep 3
        done
        curl -fsSL --max-time 300 -L "$JOB/results/result" -o "$C/$1.part"
        mv "$C/$1.part" "$C/$1"
        echo "ck_sky_gen: downloaded $C/$1 ($(wc -l < "$C/$1") rows)"
    fi
    echo "input $1: $GAIA_TAP ADQL (query in docs/ALLEN_SKY_SCIENCE.md)" >> "$W/inputs"
}
gaia_fetch gaia_hpx6.csv "$GAIA_Q1"
gaia_fetch gaia_dust.csv "$GAIA_Q2"
fetch ngc NGC.csv https://raw.githubusercontent.com/mattiaverga/OpenNGC/$OPEN/database_files/NGC.csv 54abea0b57b0a103831bfc7a8e3a99a23acfbf4bb581fcf3d8af539cbb826c84
fetch ngc_addendum addendum.csv https://raw.githubusercontent.com/mattiaverga/OpenNGC/$OPEN/database_files/addendum.csv c93f983dda6780b76b7b2e9271a856a892627710148a982ee98101a54c642fdd
fetch gw150914 GW150914_skymap.fits.gz https://dcc.ligo.org/public/0157/P1800381/007/GW150914_skymap.fits.gz a8a4bdeb46aade1389e594e466ee6372a401157188df636eae2bbb3eab2805f2
gzip -dc "$C/catalog.gz" > "$W/bsc5.dat"
gzip -dc "$C/GW150914_skymap.fits.gz" > "$W/GW150914_skymap.fits"
ln -s "$C/NGC.csv" "$C/addendum.csv" "$W/" && cp "$C/gaia_hpx6.csv" "$W/gaia_hpx6.csv" && cp "$C/gaia_dust.csv" "$W/gaia_dust.csv"
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -o "$W/ck_sky_gen" native/kernel/tools/ck_sky_gen.c -lm
set -- --cache "$W" --out "$OUT" --report "$REPORT" --inputs "$W/inputs"
[ -n "${SKY_RA:-}" ] && set -- "$@" --ra "$SKY_RA"
[ -n "${SKY_DEC:-}" ] && set -- "$@" --dec "$SKY_DEC"
[ -n "${SKY_FOV:-}" ] && set -- "$@" --fov "$SKY_FOV"
[ -n "${SKY_ROLL:-}" ] && set -- "$@" --roll "$SKY_ROLL"
[ -n "${SKY_MAG:-}" ] && set -- "$@" --mag "$SKY_MAG"
[ -n "${SKY_GRID:-}" ] && set -- "$@" --grid "$SKY_GRID"
[ -n "${SKY_EXPOSURE:-}" ] && set -- "$@" --exposure "$SKY_EXPOSURE"
[ -n "${SKY_BOOST:-}" ] && set -- "$@" --boost "$SKY_BOOST"
[ -n "${SKY_SKYGLOW:-}" ] && set -- "$@" --skyglow "$SKY_SKYGLOW"
"$W/ck_sky_gen" "$@"
echo "ck_sky_gen: wrote $OUT and $REPORT"