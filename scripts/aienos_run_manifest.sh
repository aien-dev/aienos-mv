#!/bin/sh
# aienos_run_manifest.sh -- write an aienos-run manifest (format: crates/aienos-run/src/manifest.rs).
#
#   scripts/aienos_run_manifest.sh <out> <channel> <record>...
#   record: image:<arch>:<kind>:<file>[:<url>]   or   runner:<platform>:<file>[:<url>]
#
# Size and SHA-256 are measured from <file>; <url> defaults to the file's base name
# (relative to the manifest). The manifest is unsigned; sign it with the release key
# separately (owner ceremony, not done here). Exit 0 written, 2 usage, 1 missing file.
set -eu
[ $# -ge 3 ] || { echo "usage: $0 <out> <channel> <record>..." >&2; exit 2; }
out="$1"; channel="$2"; shift 2
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
tmp="${out}.tmp"
{
    echo "aienos-run-manifest 1"
    echo "channel ${channel}"
    for r in "$@"; do
        kind="${r%%:*}"
        rest="${r#*:}"
        case "$kind" in
            image)
                arch="${rest%%:*}"; rest="${rest#*:}"
                ikind="${rest%%:*}"; rest="${rest#*:}"
                ;;
            runner)
                platform="${rest%%:*}"; rest="${rest#*:}"
                ;;
            *) echo "bad record ${r}" >&2; exit 2 ;;
        esac
        file="${rest%%:*}"
        url="${rest#*:}"
        [ "$url" != "$rest" ] || url="$(basename "$file")"
        [ -f "$file" ] || { echo "missing ${file}" >&2; exit 1; }
        size="$(wc -c <"$file" | tr -d ' ')"
        sum="$(sha256 "$file")"
        name="$(basename "$file")"
        if [ "$kind" = image ]; then
            echo "image ${arch} ${ikind} ${name} ${size} ${sum} ${url}"
        else
            echo "runner ${platform} ${name} ${size} ${sum} ${url}"
        fi
    done
} >"$tmp"
mv "$tmp" "$out"
