#!/bin/bash
# Regenerate the EdsLab fonts from custom_fonts/4.0, refresh generated font IDs, and build.
# EdsLab ships dedicated Regular/Italic/Bold/BoldItalic weights; Caecilia/GPro borrow EdsLab's
# bold/italic and fall back to it, so their IDs are refreshed here too (build-font-ids.sh).
#
# Usage:
#   scripts/update-edslab.sh            # regenerate + build (no flash)
#   scripts/update-edslab.sh --upload   # regenerate + build + flash the connected device
set -e

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
FONTDIR="$ROOT/custom_fonts/4.0"
OUTDIR="$ROOT/lib/EpdFont/builtinFonts"
CONVERT="$ROOT/lib/EpdFont/scripts/fontconvert.py"
STYLES=("Regular" "Italic" "Bold" "BoldItalic")

echo ">>> Regenerating EdsLab headers from $FONTDIR"
for size in 12 14 16 18; do
  for style in "${STYLES[@]}"; do
    lstyle=$(echo "$style" | tr '[:upper:]' '[:lower:]')
    python3 "$CONVERT" "edslab_${size}_${lstyle}" "$size" "$FONTDIR/EdsLab-${style}.ttf" --2bit \
      > "$OUTDIR/edslab_${size}_${lstyle}.h" 2>/dev/null
  done
done
# Size 10 — regular only, used for UI titles
python3 "$CONVERT" edslab_10_regular 10 "$FONTDIR/EdsLab-Regular.ttf" --2bit \
  > "$OUTDIR/edslab_10_regular.h" 2>/dev/null

echo ">>> Regenerating src/fontIds.h (font content changed -> IDs change -> page cache invalidates)"
bash "$ROOT/lib/EpdFont/scripts/build-font-ids.sh" > "$ROOT/src/fontIds.h" 2>/dev/null

if [ "$1" = "--upload" ]; then
  echo ">>> Building and flashing"
  pio run -e default --target upload
else
  echo ">>> Building (pass --upload to also flash)"
  pio run -e default
fi
