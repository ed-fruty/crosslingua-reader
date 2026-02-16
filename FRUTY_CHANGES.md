# Fruty Custom Build — Change Log

Version: `1.0.0-fruty-dev`
Branch: `fruty-custom`
Base: `master` at commit `d6f38d4`

---

## 1. Custom Font: EdsLab

**What:** Added EdsLab as a built-in reader font (4 sizes: 12/14/16/18px).

**Why:** Personal preference for reading. EdsLab is a single-weight font (no separate bold/italic/bolditalic variants exist).

**Files added:**
- `custom_fonts/EdsLab.ttf` — source TTF
- `lib/EpdFont/builtinFonts/edslab_12_regular.h` — bitmap font data (12px)
- `lib/EpdFont/builtinFonts/edslab_14_regular.h` — bitmap font data (14px)
- `lib/EpdFont/builtinFonts/edslab_16_regular.h` — bitmap font data (16px)
- `lib/EpdFont/builtinFonts/edslab_18_regular.h` — bitmap font data (18px)

**Files modified:**
- `lib/EpdFont/builtinFonts/all.h` — include the 4 edslab regular headers
- `lib/EpdFont/scripts/build-font-ids.sh` — added EDSLAB font ID generation
- `lib/EpdFont/scripts/convert-builtin-fonts.sh` — added EdsLab conversion step
- `src/fontIds.h` — added `EDSLAB_12/14/16/18_FONT_ID` defines
- `src/main.cpp` — declared 4 `EpdFont` + 4 `EpdFontFamily` objects (single font pointer reused for all style slots since only one weight exists); registered with renderer
- `src/CrossPointSettings.h` — added `EDSLAB` to `FONT_FAMILY` enum
- `src/CrossPointSettings.cpp` — added `EDSLAB` case to `getReaderFontId()`
- `src/SettingsList.h` — added EdsLab to font family setting options

**Optimization:** Since EdsLab has only one weight, we reuse the same `EpdFont` pointer for regular/bold/italic/bolditalic slots in each `EpdFontFamily`. This avoids storing 4 identical copies per size (~209KB flash saved).

---

## 2. Removed NotoSans and OpenDyslexic Fonts

**What:** Removed NotoSans (4 sizes x 4 styles = 16 files) and OpenDyslexic (4 sizes x 4 styles = 16 files) from the build. Only Bookerly and EdsLab remain as reader fonts. NotoSans 8px regular is kept for the small UI font.

**Why:** The firmware binary exceeded the 0x640000 (6.5MB) app partition size. Removing these fonts brought it down to ~59% utilization.

**Files modified:**
- `lib/EpdFont/builtinFonts/all.h` — removed 32 NotoSans/OpenDyslexic includes (kept `notosans_8_regular.h` for small UI font)
- `src/main.cpp` — removed all NotoSans/OpenDyslexic `EpdFont` + `EpdFontFamily` objects and `renderer.insertFont()` calls
- `src/CrossPointSettings.h` — removed `NOTOSANS` and `OPENDYSLEXIC` from `FONT_FAMILY` enum; renumbered to `BOOKERLY=0, EDSLAB=1`
- `src/CrossPointSettings.cpp` — removed NotoSans/OpenDyslexic cases from `getReaderFontId()`
- `src/SettingsList.h` — removed NotoSans/OpenDyslexic from font family setting options

**Note:** Users with NotoSans or OpenDyslexic previously selected will fall through to the default (Bookerly) due to the `readAndValidate` bounds check.

---

## 3. CSS Color Support for EPUB Text

**What:** Added support for CSS `color` property in EPUB rendering. Text colors are quantized to 4 grayscale levels for the e-ink display: black (0), dark gray (1), light gray (2), white/hidden (3).

**Why:** Enables visual differentiation of translated text (styled with CSS color like `#5A5A5A`) from original text in bilingual EPUBs.

**CSS parsing (`lib/Epub/Epub/css/`):**
- `CssStyle.h` — added `textGrayLevel` field (uint8_t) and `color` defined flag
- `CssParser.h` — added `interpretColor()` static method
- `CssParser.cpp` — implemented `interpretColor()` (handles named colors, #hex, rgb()); maps to 4 gray levels via luminance; added color to CSS cache serialization

**HTML parser (`lib/Epub/Epub/parsers/`):**
- `ChapterHtmlSlimParser.h` — added `effectiveColor` field and `hasColor`/`color` to `StyleStackEntry`
- `ChapterHtmlSlimParser.cpp` — color propagates through inline style stack; passed to `addWord()` via `effectiveColor`

**Text layout (`lib/Epub/Epub/`):**
- `ParsedText.h` — `addWord()` accepts `color` parameter
- `ParsedText.cpp` — color is packed into bits 5-6 of `wordStyles` (2-bit gray level); eliminated separate `wordColors` list

**Rendering (`lib/GfxRenderer/`):**
- `GfxRenderer.h` — added `colorTextGrayLevel` field and `setColorTextGrayLevel()` setter
- `GfxRenderer.cpp` — `renderChar()` remaps non-zero gray levels through the configurable `colorTextGrayLevel`; gray text now also renders in BW pass as fallback (ensures visibility even without working grayscale overlay)

**Serialization:**
- `TextBlock.h` / `TextBlock.cpp` — removed `wordColors` list (color is embedded in `wordStyles` bits 5-6); updated serialize/deserialize
- `Section.cpp` — bumped `SECTION_FILE_VERSION` from 13 → 15 (forces cache rebuild)

---

## 4. Color Text Style Setting (Configurable Gray Level)

**What:** Added a "Color Mode" setting (Settings → Reader) that controls how CSS-colored text renders on the e-ink display.

| Option | Value | Effect |
|--------|-------|--------|
| Normal | 0 | Colored text renders as black (no differentiation) |
| Dark | 1 | Colored text renders as dark gray (default) |
| Light | 2 | Colored text renders as light gray |
| None | 3 | Colored text is hidden/invisible |

**Why:** E-ink grayscale rendering varies by device. This lets the user control the visibility/contrast of CSS-colored text.

**Files modified:**
- `src/CrossPointSettings.h` — added `COLOR_TEXT_STYLE` enum and `colorTextStyle` field (default: `CT_DARK`)
- `src/CrossPointSettings.cpp` — added to serialization (read/write) at end for backward compat
- `src/SettingsList.h` — added `SettingInfo::Enum` for color text style (uses existing `STR_COLOR_MODE` label with `STR_NORMAL`/`STR_DARK`/`STR_LIGHT`/`STR_NONE_OPT` options)
- `src/main.cpp` — wires `SETTINGS.colorTextStyle` to `renderer.setColorTextGrayLevel()` in main loop
- `lib/GfxRenderer/GfxRenderer.cpp` — `renderChar()` applies the override: any non-zero CSS gray level is replaced with the setting value

---

## 5. Configurable Line Spacing

**What:** Changed line spacing from a 3-option enum (Tight/Normal/Wide) to a numeric value setting (range 5-30, step 1, representing multiplier × 10).

**Files modified:**
- `src/CrossPointSettings.h` — changed `lineSpacing` default and type semantics
- `src/CrossPointSettings.cpp` — updated `getReaderLineCompression()` from switch-case to arithmetic
- `src/SettingsList.h` — changed from `SettingInfo::Enum` to `SettingInfo::Value`

---

## 6. I18n: EdsLab Font Name

**Files modified:**
- `lib/I18n/I18nKeys.h` — added `STR_EDSLAB` string ID
- `lib/I18n/translations/*.yaml` (all 8 languages) — added `STR_EDSLAB: "EdsLab"`

---

## 7. Version Branding

**File modified:**
- `platformio.ini` — changed version from `1.0.0` to `1.0.0-fruty` (builds as `1.0.0-fruty-dev` in default env)

---

## Memory Impact Summary

| Metric | Before | After |
|--------|--------|-------|
| Flash | Exceeded 6.5MB partition | 59.3% (3.9MB / 6.5MB) |
| RAM | — | 31.0% (101KB / 328KB) |
| Peak RAM (parsing) | ~15-24KB extra from wordColors lists | Eliminated (color packed into existing wordStyles) |

---

## Merge Instructions: Keeping This Branch Updated with Upstream

This branch (`fruty-custom`) is based on `master`. When upstream `origin/master` gets new commits, follow these steps to incorporate them while keeping our custom changes:

### Option A: Rebase (recommended for clean history)

```bash
# 1. Fetch latest upstream changes
git fetch origin

# 2. Switch to our branch
git checkout fruty-custom

# 3. Rebase our changes on top of the latest master
git rebase origin/master
```

If merge conflicts occur during rebase:
```bash
# See which files conflict
git status

# For each conflicted file, edit to resolve conflicts, then:
git add <resolved-file>

# Continue the rebase
git rebase --continue

# If a conflict is too messy and you want to abort:
git rebase --abort
```

### Option B: Merge (preserves branch history)

```bash
git fetch origin
git checkout fruty-custom
git merge origin/master
```

### Conflict Resolution Strategy

Our changes are concentrated in specific areas. Here's how to resolve conflicts:

| File(s) | Strategy |
|---------|----------|
| `platformio.ini` | Keep our version (`1.0.0-fruty`). Accept any other upstream changes (new build flags, deps, etc.) |
| `src/main.cpp` (font section) | Keep our version (only Bookerly + EdsLab). If upstream adds new fonts, ignore them or add selectively |
| `src/CrossPointSettings.h` | Keep our `FONT_FAMILY` enum (`BOOKERLY=0, EDSLAB=1`). Accept new settings upstream adds |
| `src/CrossPointSettings.cpp` | Keep our `getReaderFontId()` (only Bookerly + EdsLab). For serialization, new upstream fields go at the end — accept them. Our `colorTextStyle` should stay at its position |
| `src/SettingsList.h` | Keep our font family list (`Bookerly, EdsLab`). Accept new settings upstream adds |
| `lib/EpdFont/builtinFonts/all.h` | Keep our version (Bookerly + NotoSans8 + EdsLab + Ubuntu). If upstream adds fonts, only add if we want them |
| `lib/GfxRenderer/GfxRenderer.*` | Keep our `colorTextGrayLevel` and `effectiveGrayLevel` logic. Accept other rendering changes |
| `lib/Epub/Epub/**` | Keep our changes (color packed in wordStyles, no wordColors list). If upstream modifies these files, carefully merge — our structural changes (removing wordColors) must be preserved |
| `lib/Epub/Epub/Section.cpp` | Keep whichever `SECTION_FILE_VERSION` is higher (ours or upstream). If upstream bumps it too, use the higher number |
| `lib/I18n/translations/*.yaml` | Accept upstream string additions. Keep our `STR_EDSLAB` entry |
| `src/fontIds.h` | Keep our version. Accept new font IDs if upstream adds them |

### General conflict resolution rules:

1. **"Accept both"** — when upstream adds something new (new setting, new string) alongside our additions
2. **"Keep ours"** — for font removal (NotoSans, OpenDyslexic), version branding, font enum values
3. **"Keep theirs"** — for bug fixes, new features unrelated to our changes
4. **"Manual merge"** — when both sides modify the same function (e.g., if upstream changes `renderChar` logic)

### After resolving conflicts:

```bash
# Rebuild to verify everything compiles
pio run

# Flash and test:
# - Open an EPUB with CSS colors → verify color text renders with chosen style
# - Switch font to EdsLab → verify all sizes work
# - Check settings → Color Mode and font options should show correctly
```

### Quick sanity checks after merge:

1. `pio run` compiles without errors
2. Binary fits in partition (< 6.5MB)
3. EdsLab font renders (bold/italic tags show same glyphs, no crash)
4. CSS-colored text visible with "Embedded Style" on
5. "Color Mode" setting works (Normal/Dark/Light/None)
