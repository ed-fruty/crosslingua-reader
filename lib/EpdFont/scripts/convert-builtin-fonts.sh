#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit > $output_path
    echo "Generated $output_path"
  done
done

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="opendyslexic_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/OpenDyslexic/OpenDyslexic-${style}.otf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

EDSLAB_FONT_SIZES=(12 14 16 18)
EDSLAB_FONT_DIR="../../../custom_fonts/4.0"

for size in ${EDSLAB_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="edslab_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    output_path="../builtinFonts/${font_name}.h"
    # EdsLab now ships dedicated Regular/Italic/Bold/BoldItalic weights
    python fontconvert.py $font_name $size "${EDSLAB_FONT_DIR}/EdsLab-${style}.ttf" --2bit > $output_path
    echo "Generated $output_path"
  done
done

# EdsLab size 10 — regular only, used for UI titles (not reader body text)
python fontconvert.py edslab_10_regular 10 "${EDSLAB_FONT_DIR}/EdsLab-Regular.ttf" --2bit > ../builtinFonts/edslab_10_regular.h
echo "Generated ../builtinFonts/edslab_10_regular.h"

# Caecilia: regular weight only (borrows Bookerly bold/italic at runtime)
CAECILIA_FONT_SIZES=(12 14 16 18)
CAECILIA_FONT_PATH="../../../custom_fonts/CaeciliaLTPro55Roman.TTF"

for size in ${CAECILIA_FONT_SIZES[@]}; do
  font_name="caecilia_${size}_regular"
  output_path="../builtinFonts/${font_name}.h"
  python fontconvert.py $font_name $size "$CAECILIA_FONT_PATH" --2bit > $output_path
  echo "Generated $output_path"
done

python fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h
