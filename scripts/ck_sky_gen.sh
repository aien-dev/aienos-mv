#!/bin/sh
# ck_sky_gen.sh -- makes native/kernel/ui/sky_data.c, the real sky Allen
# opens on, from real astronomical data (see native/kernel/tools/ck_sky_gen.py
# and NOTICE for the sources, their licences and credits).
#
# Downloads each input once into $SKY_CACHE (default ~/.cache/aienos-sky),
# checks its sha256 (the generator refuses a file that does not match), and
# writes the C tables plus a plain-text report of what it found. The kernel
# itself never touches the network: the tables are compiled in.
#
# Settings (environment; the defaults are the committed sky):
#   SKY_RA, SKY_DEC  view centre, degrees (default: GW150914's most probable point)
#   SKY_FOV          degrees of sky across the screen's width (80)
#   SKY_ROLL         degrees the view is turned from north up (0)
#   SKY_MAG          faintest star, V magnitude (6.5, the naked-eye limit)
#   SKY_GRID         Milky Way samples across the screen's width (240)
#   SKY_EXPOSURE     linear light at the middle of a magnitude 6.5 star (see the tool)
#   SKY_BOOST        the Milky Way's light over its measured value, times (see the report)
#   SKY_SKYGLOW      the dark sky's own glow, V magnitudes per square arcsecond (22.0)
# Needs python3 with Pillow (to read the photograph), curl and sha256sum.
set -eu
cd "$(dirname "$0")/.."
C=${SKY_CACHE:-$HOME/.cache/aienos-sky}
mkdir -p "$C"
fetch() { # name url sha256
    if [ ! -f "$C/$1" ] || [ "$(sha256sum "$C/$1" | cut -d' ' -f1)" != "$3" ]; then
        echo "ck_sky_gen: downloading $2"
        curl -fsSL --retry 3 -m 900 -o "$C/$1.part" "$2"
        mv "$C/$1.part" "$C/$1"
    fi
    [ "$(sha256sum "$C/$1" | cut -d' ' -f1)" = "$3" ] || { echo "ck_sky_gen: $1 does not match its sha256"; exit 1; }
}
python3 native/kernel/tools/ck_sky_gen.py --list-sources | while read -r name url sum; do
    fetch "$name" "$url" "$sum"
done
set -- --cache "$C" --out native/kernel/ui/sky_data.c --report native/kernel/ui/sky_data.txt
[ -n "${SKY_RA:-}" ] && set -- "$@" --ra "$SKY_RA"
[ -n "${SKY_DEC:-}" ] && set -- "$@" --dec "$SKY_DEC"
[ -n "${SKY_FOV:-}" ] && set -- "$@" --fov "$SKY_FOV"
[ -n "${SKY_ROLL:-}" ] && set -- "$@" --roll "$SKY_ROLL"
[ -n "${SKY_MAG:-}" ] && set -- "$@" --mag "$SKY_MAG"
[ -n "${SKY_GRID:-}" ] && set -- "$@" --grid "$SKY_GRID"
[ -n "${SKY_EXPOSURE:-}" ] && set -- "$@" --exposure "$SKY_EXPOSURE"
[ -n "${SKY_BOOST:-}" ] && set -- "$@" --boost "$SKY_BOOST"
[ -n "${SKY_SKYGLOW:-}" ] && set -- "$@" --skyglow "$SKY_SKYGLOW"
python3 native/kernel/tools/ck_sky_gen.py "$@"
echo "ck_sky_gen: wrote native/kernel/ui/sky_data.c and sky_data.txt"
