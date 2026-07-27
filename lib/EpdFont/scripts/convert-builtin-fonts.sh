#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSANS_FONT_SIZES=(12 14 16 18)

# EdsLab, the built-in default reading family. Edit THIS line to add or drop a
# reader point size, then re-run:
#   bash lib/EpdFont/scripts/convert-builtin-fonts.sh
#   bash lib/EpdFont/scripts/build-font-ids.sh   # see the note in that script
# Adding/removing a size also needs: a matching #include in builtinFonts/all.h,
# the EpdFont/EpdFontFamily globals + insertFont() call in src/main.cpp, the
# EDSLAB_<size>_FONT_ID entry in build-font-ids.sh, the switch in
# CrossPointSettings::getReaderFontId(), and BUILTIN_READER_POINT_SIZES in
# src/ReaderFontSizes.h. Changing font *content* changes the ids, which
# invalidates every cached page layout on the SD card (by design).
EDSLAB_FONT_SIZES=(12 14 16 18)
# Extra EdsLab faces outside the reader matrix, "<size>:<Style>" pairs. These are
# NOT user-selectable body sizes (BUILTIN_READER_POINT_SIZES stays 12/14/16/18) —
# 8pt regular is the small annotation face (the counterpart to notosans_8_regular),
# kept in the family so a slab-serif body page never mixes in a grotesque.
# Add e.g. 10:Regular here to ship a 10pt face, then re-run the two commands above.
EDSLAB_EXTRA_FACES=(8:Regular)

for size in ${EDSLAB_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="edslab_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/EdsLab/EdsLab-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

for face in ${EDSLAB_EXTRA_FACES[@]}; do
  size="${face%%:*}"
  style="${face##*:}"
  font_name="edslab_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
  font_path="../builtinFonts/source/EdsLab/EdsLab-${style}.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
  echo "Generated $output_path"
done

# Noto Serif is no longer a built-in family: EdsLab took the serif slot, because
# the app partition has room for two reader families, not three. The TTFs stay in
# builtinFonts/source/NotoSerif — to bring it back, copy the loop below with
# NOTOSERIF_FONT_SIZES / source/NotoSerif/NotoSerif-${style}.ttf and re-add its
# includes, globals, ids and settings enum entry (same checklist as EdsLab above).
for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

# Arabic glyphs for UI text (menus, file browser titles). The built-in fonts
# must cover the *output* of MiniBidi's do_shape() — contextual presentation
# forms — not base letters, or shaped UI text silently drops glyphs.
# Curated for firmware-size budget: core Arabic (Presentation Forms-B,
# incl. the Lam-Alef ligature forms) plus the Farsi/Urdu extra letters'
# Presentation Forms-A blocks, the few characters shaping leaves at their
# base codepoint, Arabic punctuation, and both digit sets. No harakat and
# no Sindhi/Pashto/Kurdish forms — book text gets those from SD-card fonts.
ARABIC_INTERVALS=(
  --additional-intervals 0x060C,0x060C  # Arabic comma
  --additional-intervals 0x061B,0x061B  # Arabic semicolon
  --additional-intervals 0x061F,0x061F  # Arabic question mark
  --additional-intervals 0x0621,0x0621  # hamza (non-joining, never shaped)
  --additional-intervals 0x0640,0x0640  # tatweel
  --additional-intervals 0x0660,0x0669  # Arabic-Indic digits
  --additional-intervals 0x06BA,0x06BA  # noon ghunna base (initial/medial keep base cp)
  --additional-intervals 0x06D4,0x06D4  # Urdu full stop
  --additional-intervals 0x06F0,0x06F9  # extended Arabic-Indic digits (Farsi/Urdu)
  --additional-intervals 0xFB56,0xFB59  # peh (Farsi)
  --additional-intervals 0xFB66,0xFB69  # tteh (Urdu)
  --additional-intervals 0xFB7A,0xFB7D  # tcheh (Farsi)
  --additional-intervals 0xFB88,0xFB95  # ddal, jeh, rreh (Urdu), keheh, gaf (Farsi/Urdu)
  --additional-intervals 0xFB9E,0xFB9F  # noon ghunna isolated/final (Urdu)
  --additional-intervals 0xFBA6,0xFBB1  # heh goal, heh doachashmee, yeh barree(+hamza) (Urdu)
  --additional-intervals 0xFBFC,0xFBFF  # farsi yeh (Farsi/Urdu)
  --additional-intervals 0xFE80,0xFEFC  # Presentation Forms-B: core Arabic + Lam-Alef
)

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    # EdsLab is the CrossLingua UI face. Script-specific fonts remain later in
    # the stack as fallbacks for glyphs EdsLab does not ship.
    font_name="edslab_ui_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/EdsLab/EdsLab-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    arabic_path="../builtinFonts/source/NotoSansArabic/NotoSansArabic-${style}.ttf"
    # Append the Vietnamese Ubuntu cut for any Latin Extended Additional glyph
    # EdsLab does not ship. The font stack keeps EdsLab at highest priority.
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path $arabic_path $viet_path \
      --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py edslab_ui_8_regular 8 \
  ../builtinFonts/source/EdsLab/EdsLab-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  ../builtinFonts/source/NotoSansArabic/NotoSansArabic-Regular.ttf \
  ../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > ../builtinFonts/edslab_ui_8_regular.h

# Keep the legacy UI assets reproducible so CROSSLINGUA_UI_EDSLAB can be
# switched back to 0 without restoring files from Git.
for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    arabic_path="../builtinFonts/source/NotoSansArabic/NotoSansArabic-${style}.ttf"
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path $arabic_path $viet_path \
      --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > $output_path
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  ../builtinFonts/source/NotoSansArabic/NotoSansArabic-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
