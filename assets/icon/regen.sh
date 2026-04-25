#!/usr/bin/env bash
# Rasterize icon.svg into the Apple iconset PNGs that iconutil consumes.
# Run manually whenever icon.svg changes; commit the regenerated PNGs.
# The build itself never invokes this — librsvg is not a build-time dep.

set -euo pipefail

cd "$(dirname "$0")"

if ! command -v rsvg-convert >/dev/null 2>&1; then
    echo "regen.sh: rsvg-convert not found (brew install librsvg)" >&2
    exit 1
fi

SRC=icon.svg
OUT=mb.iconset

if [[ ! -f "$SRC" ]]; then
    echo "regen.sh: $SRC not found in $(pwd)" >&2
    exit 1
fi

mkdir -p "$OUT"

# Apple's required iconset entries. Each base size needs @1x and @2x.
sizes=(16 32 128 256 512)

for s in "${sizes[@]}"; do
    one="$OUT/icon_${s}x${s}.png"
    two="$OUT/icon_${s}x${s}@2x.png"
    rsvg-convert -w "$s"          -h "$s"          "$SRC" -o "$one"
    rsvg-convert -w "$((s * 2))"  -h "$((s * 2))"  "$SRC" -o "$two"
    echo "wrote $one ($s x $s)"
    echo "wrote $two ($((s * 2)) x $((s * 2)))"
done

echo "done — $OUT/ ready for: iconutil -c icns $OUT -o mb.icns"
