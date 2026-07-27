# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

## EdsLab (built in — nothing to install)

**EdsLab**, a slab serif with genuine Ukrainian Cyrillic (і, ї, є, ґ), is
compiled into this firmware and is the **default reading font**. It needs no SD
card and no download: flash the firmware and it is already selected under
**Settings > Reader > Font Family**, at 12/14/16/18 pt.

It replaced Noto Serif as the built-in serif family, so a device that was using
Noto Serif comes up on EdsLab after the upgrade; Noto Sans is unchanged and
still selectable. Because the reader font id is part of the page-cache key, the
first open of each book after upgrading re-lays it out.

An SD-card build of the same family is still published with each release as
`EdsLab-sdcard-font.zip`, and `lib/EpdFont/scripts/sd-fonts.yaml` still carries
its recipe. That build is **optional** — it exists so an updated cut of the font
can be shipped without reflashing, and because the SD build carries wider
coverage than the firmware one (see below). Install it exactly like any other SD
family (Option 3 below); it then appears as a second "EdsLab" entry in the font
list alongside the built-in one.

Coverage differs between the two builds, which is the one thing worth knowing:

| | built-in | SD `.cpfont` |
|---|---|---|
| interval set | the shared built-in set (`lib/EpdFont/scripts/fontconvert.py`) | `reading,(0x2100-0x214F)` (`sd-fonts.yaml`) |
| Latin, Russian/Ukrainian Cyrillic | yes | yes |
| numero sign № | no — outside the built-in interval set | yes |
| Vietnamese (U+1EA0–U+1EF9), combining marks, most of Latin Ext-B | **no — the TTFs have no such glyphs** | no |

The font's own repertoire is ~590 codepoints, so neither build can render
Vietnamese, polytonic Greek or the full Cyrillic range. Text in those scripts
renders as *nothing at all* on a built-in family — EdsLab has no U+FFFD
replacement glyph either, so `EpdFont::getGlyph()` returns `nullptr` and the
character is skipped with zero advance rather than drawn as a box. Switch to
Noto Sans (built in, full coverage) or an SD family for those books.

To regenerate either build after changing the TTFs:

    # built-in headers (sizes live in EDSLAB_FONT_SIZES at the top of the script)
    bash lib/EpdFont/scripts/convert-builtin-fonts.sh
    bash lib/EpdFont/scripts/build-font-ids.sh > src/fontIds.h

    # SD card .cpfont build
    python3 lib/EpdFont/scripts/build-sd-fonts.py --only EdsLab

EdsLab is *not* listed under **Manage Fonts** — that browser downloads from
the shared [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts),
which carries the upstream catalogue only.

**Licence.** EdsLab is © 2026 Eds Lab, released by its author as "free for
personal and commercial use" (the terms are embedded in the font's own name
table; see `lib/EpdFont/builtinFonts/source/EdsLab/LICENSE.txt`). Unlike the
other families in this repository it is not under the SIL Open Font Licence.

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to Wi-Fi
2. Go to **Settings > System > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Start **File Transfer** and connect through **Join Network** or **Create Hotspot**
2. Open the web interface URL shown on the reader
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts)
2. Copy font family folders to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot and the results are merged: a
   family installed in `/fonts/` shows up even when `/.fonts/` also
   exists, and vice versa. The two roots only collide if the same family
   name appears in both — in that case the copy in `/.fonts/` wins and
   the duplicate in `/fonts/` is ignored.

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and power on your CrossPoint reader

## CJK in the User Interface

The built-in UI fonts are Latin-only, so by default the interface (book titles
in the library, file names in the browser, list rows, headers) shows
replacement boxes for Chinese/Japanese/Korean text even when book *content*
renders correctly with a selected SD-card font.

To avoid shipping a large CJK glyph set in flash, CrossPoint instead reuses the
SD-card font you already selected: when a UI string contains a CJK character
the built-in font cannot draw, that whole string is rendered with your selected
SD-card font instead.

The fallback is **size-matched**. The built-in UI fonts render at 8 pt
(small/author lines), 10 pt (list rows) and 12 pt (book-cover titles, headers),
so CrossPoint loads your SD family at those sizes too and maps each UI font to
its same-size SD font. CJK book names therefore appear at the same size as the
Latin text around them. For this to work the family must contain `.cpfont`
files at sizes **8, 10 and 12** (in addition to the reader sizes 12–18); any UI
size missing from the family simply keeps showing boxes for CJK at that size.

Note that **Settings > Reader > Font Size** lists every size the family ships,
so a family built at 8,10,12,14,16,18 offers all six as reading sizes — the UI
sizes are not hidden from the list. Reading at 8 pt is your call; if you would
rather not see the small sizes there, convert two families (one with the UI
sizes for fallback, one with only the reading sizes you want).

When converting your own font, include the UI sizes:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

What this means in practice:

- Select a CJK-capable SD font under **Settings > Reader > Font Family**
  (see [Installing Fonts](#installing-fonts) and the `cjk` / `hangul` presets
  under [Converting Custom Fonts](#converting-custom-fonts)). That single
  selection drives both book content *and* size-matched CJK fallback in the UI.
- Pure-Latin UI strings keep the crisp built-in font; only strings that
  actually contain CJK are routed to the SD font.
- The fallback is per *string*, not per glyph: a mixed title such as
  `三体 Vol.1` renders entirely in the SD font (including the Latin part). If
  that SD font is a `Mono` family, the Latin portion will appear half/full
  width.
- If no SD font is selected (a built-in reading font is active), there is no
  CJK fallback and the UI again shows boxes for CJK — pick a CJK SD font to
  restore it.

## Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020–U+007E (Basic Latin) |
| `latin1` | U+0080–U+00FF (Latin-1 Supplement) |
| `latin-ext` | European languages (Latin + Extended-A/B + punctuation + ligatures) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `hebrew` | Hebrew + Alphabetic Presentation Forms |
| `georgian` | Georgian + Georgian Supplement |
| `armenian` | Armenian |
| `ethiopic` | Ethiopic + Extended |
| `vietnamese` | Vietnamese subset (ơ/ư and combining marks) |
| `punctuation` | General punctuation (U+2000–U+206F) |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables + Jamo + Compatibility Jamo |
| `cherokee` | Cherokee (historic + supplement block) |
| `tifinagh` | Tifinagh |
| `symbols` | Math, currency, arrows, box-drawing, misc symbols, dingbats |
| `reading` | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin` | Matches the firmware's built-in font conversion intervals |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Additional options

`--force-autohint` — force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.
