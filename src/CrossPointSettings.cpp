#include "CrossPointSettings.h"

#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "SettingsList.h"
#include "activities/translator/LanguagePickerActivity.h"
#include "fontIds.h"
#include "translator/InterlinearPairing.h"

namespace {

// Stack buffer for "<key>_obf" key construction — avoids a std::string
// allocation per obfuscated setting on every save and load.
constexpr size_t OBF_KEY_BUF = 64;

// Null-terminated copy into a fixed-size settings field.
void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

// The built-in reader face at `pt` in the requested family. `pt` MUST already be one of
// BUILTIN_READER_POINT_SIZES (snap first); anything else lands on the 14pt default.
//
// THE single (family, point size) -> font id lookup: getReaderFontId() and smallerReaderFontId()
// both route through here, so adding a built-in point size means editing one switch, not three.
// `sans` selects NOTOSANS; false means the EDSLAB slot (which is also family index 0, so a stored
// legacy Noto Serif value lands here — see LEGACY_NOTOSERIF).
int builtinReaderFontId(const uint8_t pt, const bool sans) {
  switch (pt) {
    case 12:
      return sans ? NOTOSANS_12_FONT_ID : EDSLAB_12_FONT_ID;
    case 16:
      return sans ? NOTOSANS_16_FONT_ID : EDSLAB_16_FONT_ID;
    case 18:
      return sans ? NOTOSANS_18_FONT_ID : EDSLAB_18_FONT_ID;
    case 14:
    default:
      return sans ? NOTOSANS_14_FONT_ID : EDSLAB_14_FONT_ID;
  }
}

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  const CrossPointSettings& s = *this;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        doc[obfKey] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  // Font family and size — both use dynamic getter/setters in SettingsList (the
  // option lists depend on the SD font registry), so the generic loop skips them.
  doc["fontFamily"] = fontFamily;
  doc["fontSize"] = fontPointSize;
  // SD card font family name — not in SettingsList, save manually
  if (sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = sdFontFamilyName;
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, save manually
  if (dictionaryName[0] != '\0') {
    doc["dictionaryName"] = dictionaryName;
  }

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (language < getLanguageCount()) ? LANGUAGE_CODES[language] : "EN";

  // Pre-Translation feature -- managed by LanguagePickerActivity / EngineSelectActivity,
  // not in SettingsList. 0xFF sentinels and a free-form API key don't fit the SettingInfo schema.
  doc["translationLanguage"] = translationLanguage;
  doc["sourceTranslationLanguage"] = sourceTranslationLanguage;
  doc["translationEngine"] = translationEngine;
  doc["translateApiKey"] = translateApiKey;
  doc["translationDisplayMode"] = translationDisplayMode;
  doc["translationShade"] = translationShade;
  // One key per mode. These are NEW names, not a rename of the single "translationSize" key they
  // replace: keys are append-only, and a file still carrying the old key must fall through to the
  // per-mode DEFAULTS (that is what preserves the overlays' historical Smaller behaviour) rather
  // than inherit one shared choice. toJson() rebuilds the document from scratch on every save, so
  // the retired key disappears from the file on the next write without a migration step.
  doc["interleavedTranslationSize"] = interleavedTranslationSize;
  doc["tooltipTranslationSize"] = tooltipTranslationSize;
  doc["pageTranslationSize"] = pageTranslationSize;
  doc["tooltipButtons"] = tooltipButtons;
  doc["tooltipBehavior"] = tooltipBehavior;
  doc["pageTranslationButtons"] = pageTranslationButtons;
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  CrossPointSettings& s = *this;
  bool needsResave = false;

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      // destPtr starts out holding the struct-initializer default; it stays that
      // way unless the document actually carries a value for this key.
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        continue;
      }

      bool loaded = false;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        bool ok = false;
        const std::string decoded = obfuscation::deobfuscateFromBase64(doc[obfKey] | "", &ok);
        if (ok && !decoded.empty()) {
          copyToField(destPtr, decoded.c_str(), info.stringMaxLen);
          loaded = true;
        }
      }
      if (!loaded) {
        // Read as const char*, never `| std::string(...)`: ArduinoJson's
        // std::string converter drags a per-TU copy of the serializer into
        // flash. See the note in PersistableStore.h.
        const char* raw = doc[info.key].is<const char*>() ? doc[info.key].as<const char*>() : nullptr;
        if (raw) {
          // Obfuscated field recovered from a legacy plaintext value -> resave.
          if (info.obfuscated && strcmp(raw, destPtr) != 0) needsResave = true;
          copyToField(destPtr, raw, info.stringMaxLen);
        }
      }
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)SLEEP_10_MIN, SLEEP_TIMEOUT_COUNT, (uint8_t)SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  frontButtonBack = clamp(doc["frontButtonBack"] | (uint8_t)FRONT_HW_BACK, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm =
      clamp(doc["frontButtonConfirm"] | (uint8_t)FRONT_HW_CONFIRM, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft = clamp(doc["frontButtonLeft"] | (uint8_t)FRONT_HW_LEFT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)FRONT_HW_RIGHT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  validateFrontButtonMapping(s);

  // Reader font size — an actual point size since 1.5. Files written by 1.4 and
  // earlier hold the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot in 0..3; no font is
  // renderable at those sizes, so the range is unambiguous and folds to the
  // point sizes those slots used to mean. Drop this once 1.4 upgrades are done.
  uint8_t storedFontSize = doc["fontSize"] | DEFAULT_FONT_POINT_SIZE;
  if (storedFontSize <= LEGACY_FONT_SIZE_MAX) {
    storedFontSize = 12 + storedFontSize * 2;  // 0,1,2,3 -> 12,14,16,18
    needsResave = true;
  }
  fontPointSize = storedFontSize;

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  // An ABSENT key adopts the struct-initializer default (EdsLab); a present key is
  // honoured, so an existing user's choice survives the upgrade untouched.
  const uint8_t storedFontFamily = doc["fontFamily"] | fontFamily;
  fontFamily = clamp(storedFontFamily, BUILTIN_FONT_COUNT, EDSLAB);
  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sfn, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
  if (storedFontFamily == LEGACY_OPENDYSLEXIC && sdFontFamilyName[0] == '\0') {
    fontFamily = EDSLAB;
    strncpy(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName) - 1);
    sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
    needsResave = true;
  } else if (storedFontFamily >= BUILTIN_FONT_COUNT) {
    needsResave = true;
  } else if (storedFontFamily == LEGACY_NOTOSERIF) {
    // Noto Serif was dropped from the firmware and EdsLab took its slot, so the
    // stored index already resolves to the right family and the file needs no
    // rewrite. Logged rather than silently reinterpreted, because the reader font
    // visibly changes for these users and their section cache is invalidated (the
    // font id is part of the cache key — see getReaderFontId / Section.cpp:195).
    LOG_DBG("CPS", "Font family 0 (was Noto Serif) now resolves to EdsLab");
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, load manually
  copyToField(dictionaryName, doc["dictionaryName"] | "", sizeof(dictionaryName));

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  // Pre-Translation feature -- absent keys keep struct-initializer defaults (backward compatible
  // with settings.json files written before this feature shipped).
  translationLanguage = doc["translationLanguage"] | translationLanguage;
  sourceTranslationLanguage = doc["sourceTranslationLanguage"] | sourceTranslationLanguage;
  translationEngine = clamp(doc["translationEngine"] | (uint8_t)ENGINE_GOOGLE_V2, (uint8_t)TRANSLATION_ENGINE_COUNT,
                            (uint8_t)ENGINE_GOOGLE_V2);
  copyToField(translateApiKey, doc["translateApiKey"] | "", sizeof(translateApiKey));
  translationShade =
      clamp(doc["translationShade"] | (uint8_t)SHADE_DIMMED, (uint8_t)TRANSLATION_SHADE_COUNT, (uint8_t)SHADE_DIMMED);
  // Per-mode translated-text sizes. ArduinoJson's `|` yields its right operand when the key is
  // ABSENT (or holds a value that will not convert), so a settings.json written before these keys
  // existed — every existing install, since all three names are new — adopts the per-mode DEFAULT
  // below, and only a key that is present and in range can override it. The defaults mirror the
  // struct initializers in CrossPointSettings.h: Interleaved matched the body text before this row
  // existed, and both overlays were always drawn one step smaller, so this load path reproduces the
  // pre-split behaviour byte for byte instead of resetting every user to Same.
  interleavedTranslationSize = clamp(doc["interleavedTranslationSize"] | (uint8_t)SIZE_SAME,
                                     (uint8_t)TRANSLATION_SIZE_COUNT, (uint8_t)SIZE_SAME);
  tooltipTranslationSize = clamp(doc["tooltipTranslationSize"] | (uint8_t)SIZE_SMALLER, (uint8_t)TRANSLATION_SIZE_COUNT,
                                 (uint8_t)SIZE_SMALLER);
  pageTranslationSize =
      clamp(doc["pageTranslationSize"] | (uint8_t)SIZE_SMALLER, (uint8_t)TRANSLATION_SIZE_COUNT, (uint8_t)SIZE_SMALLER);
  // Display mode, with the retired-hole migration. Values 1 and 2 were the separate "Dimmed" and
  // "Dimmed Light" modes; they are now ONE mode (PT_INTERLEAVED) plus the translationShade colour
  // sub-setting, so a stored 1/2 folds into that pair and requests a resave — same needsResave
  // mechanism as the font-size rescale and the OpenDyslexic family remap above. Anything else out
  // of range clamps to PT_NORMAL. These two branches are exhaustive over the stored value, and
  // the migration branch runs FIRST, so translationDisplayMode can never come out of a load
  // holding a retired hole.
  const uint8_t storedDisplayMode = doc["translationDisplayMode"] | (uint8_t)PT_NORMAL;
  if (storedDisplayMode == PT_LEGACY_DIMMED || storedDisplayMode == PT_LEGACY_DIMMED_LIGHT) {
    translationDisplayMode = PT_INTERLEAVED;
    translationShade = (storedDisplayMode == PT_LEGACY_DIMMED_LIGHT) ? SHADE_DIMMED_LIGHT : SHADE_DIMMED;
    needsResave = true;
  } else {
    translationDisplayMode = clamp(storedDisplayMode, (uint8_t)PT_MODE_COUNT, (uint8_t)PT_NORMAL);
  }
  // Tooltip / Page Translation overlay controls: 0/1 selectors clamped to their enum counts. An
  // ABSENT key falls back to the feature default (SIDE buttons, TURN_PAGE nav) rather than 0 — a
  // settings.json written before these defaults changed had no key, so it should adopt the new
  // default too.
  tooltipButtons = clamp(doc["tooltipButtons"] | (uint8_t)OVERLAY_BUTTONS_SIDE, (uint8_t)OVERLAY_BUTTONS_COUNT,
                         (uint8_t)OVERLAY_BUTTONS_SIDE);
  tooltipBehavior = clamp(doc["tooltipBehavior"] | (uint8_t)TOOLTIP_NAV_TURN_PAGE, (uint8_t)TOOLTIP_NAVIGATION_COUNT,
                          (uint8_t)TOOLTIP_NAV_TURN_PAGE);
  // The key was "modalButtons" until the mode was renamed to Page Translation. Fall back to the
  // legacy key so an existing button choice is not silently reset to the default, and request a
  // resave so the file is rewritten under the new name — same needsResave mechanism as the
  // sleepTimeout and font-size migrations above. The legacy value is only consulted when the new
  // key is absent (the `|` default chain), so a file carrying both prefers the new one.
  const uint8_t legacyPageTranslationButtons = doc["modalButtons"] | (uint8_t)OVERLAY_BUTTONS_SIDE;
  if (doc["pageTranslationButtons"].isNull() && !doc["modalButtons"].isNull()) {
    needsResave = true;
  }
  pageTranslationButtons = clamp(doc["pageTranslationButtons"] | legacyPageTranslationButtons,
                                 (uint8_t)OVERLAY_BUTTONS_COUNT, (uint8_t)OVERLAY_BUTTONS_SIDE);

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

CrossPointSettings::StatusBarSpec CrossPointSettings::statusBarSpec() const {
  StatusBarSpec spec;
  spec.showChapterPageCount = statusBarChapterPageCount != 0;
  spec.showBookProgressPercent = statusBarBookProgressPercentage != 0;
  spec.titleMode = statusBarTitle;
  spec.showBattery = statusBarBattery != 0;
  spec.showBatteryPercent = hideBatteryPercentage == HIDE_NEVER;
  spec.clockMode = statusBarClock;
  spec.clock12h = clockFormat == 1;
  spec.clockUtcOffsetQ = clockUtcOffsetQ;
  spec.progressBarMode = statusBarProgressBar;
  spec.progressBarHeightPx =
      statusBarProgressBar != HIDE_PROGRESS ? static_cast<uint8_t>((statusBarProgressBarThickness + 1) * 2) : 0;
  spec.xtcMode = xtcStatusBarMode;
  return spec;
}

PtLayout CrossPointSettings::ptLayoutForDisplayMode(const uint8_t mode) {
  switch (static_cast<PRE_TRANSLATION_MODE>(mode)) {
    // Overlay modes surface their translations in a popup composited at view time, so their main
    // flow is original-only — the same pages Original Only produces, byte for byte.
    case PT_ORIGINAL_ONLY:
    case PT_PAGE_TRANSLATION:
    case PT_TOOLTIP:
      return PtLayout::OriginalOnly;
    case PT_TRANSLATION_ONLY:
      return PtLayout::TranslationOnly;
    case PT_SIDE_BY_SIDE:
      return PtLayout::SideBySide;
    // Its own layout: the translation is emitted as a small annotation row ABOVE the source line each
    // sentence starts on, so the pages genuinely differ from every other mode's and cannot share a
    // cache entry with them.
    case PT_INTERLINEAR:
      return PtLayout::Interlinear;
    // Everything inline-bilingual. Interleaved differs from Normal only in the gray level translated
    // words are DRAWN at (translationShade), which never moves a glyph, so the pages are identical.
    // The retired holes can never reach this function (fromJson migrates them), but are mapped so the
    // switch stays exhaustive.
    case PT_NORMAL:
    case PT_INTERLEAVED:
    case PT_LEGACY_DIMMED:
    case PT_LEGACY_DIMMED_LIGHT:
      return PtLayout::Both;
  }
  return PtLayout::Both;  // unreachable: every enumerator returns above
}

int CrossPointSettings::translationFontIdForSize(const uint8_t sizeSetting) const {
  // 0 == "same as the body font", which is both the SIZE_SAME answer and the graceful answer when
  // the active family ships no smaller face: smallerReaderFontId() returns 0 there, so a stored
  // SIZE_SMALLER degrades to Same WITHOUT rewriting the setting (switch back to a family that has a
  // smaller face and the choice is still there — and no SPIFFS write happened to preserve it).
  if (sizeSetting != SIZE_SMALLER) return 0;
  return smallerReaderFontId();
}

int CrossPointSettings::getInterleavedTranslationFontId() const {
  // The one size that is a LAYOUT input. Both the cache key (ReaderRenderSpec::translationFontId)
  // and the render-time font set (readerPageFontSet) read it through here, so changing it
  // invalidates exactly the sections whose line breaking it can move, and a page is always drawn in
  // the fonts it was measured with. The overlay sizes deliberately have no path to either.
  //
  // Gated on the mode being Interleaved, and this is the ONLY place that gate can live: Normal and
  // Interleaved collapse onto the same PtLayout::Both (their pages differ only in how translated
  // words are drawn), so the layout engine cannot tell them apart -- yet Normal must keep the
  // translation at body size, because presenting the two languages as one undifferentiated flow is
  // the entire point of that mode. A stored Smaller therefore sits dormant while the mode is anything
  // but Interleaved and returns the instant it is selected again, with no SPIFFS write either way.
  // (Interlinear has its OWN layout and its own font slot -- see getInterlinearAnnotationFontId --
  // so it neither reads nor is affected by this size.)
  if (translationDisplayMode != PT_INTERLEAVED) return 0;
  return translationFontIdForSize(interleavedTranslationSize);
}

int CrossPointSettings::getInterlinearAnnotationFontId() const {
  // Same shape as getInterleavedTranslationFontId: mode-gated here, because the layout engine only
  // ever sees a PtLayout and must not carry mode semantics. Feeds BOTH the section cache key
  // (ReaderRenderSpec::annotationFontId) and the render-time font set (readerPageFontSet), so an
  // annotation row is always drawn in the face it was measured and advanced with.
  //
  // THE single place the annotation face is chosen. A future user-facing Annotation Size row, and a
  // switch to the now-merged EdsLab 8pt face (EDSLAB_8_FONT_ID), plug in here and are picked up by
  // the layout engine, the renderer and the cache key at once -- nothing downstream names a font id.
  // Still SMALL_FONT_ID for now, deliberately: notosans_8 is what interlinearAnnotationScriptSupported()
  // below reasons about, EdsLab's ~590-codepoint repertoire covers far less of it, EDSLAB_8_FONT_ID is
  // registered only under #ifndef OMIT_FONTS unlike SMALL_FONT_ID, and the swap would invalidate every
  // cached interlinear section. Changing the annotation face is a decision, not a merge fixup.
  if (translationDisplayMode != PT_INTERLINEAR) return 0;
  if (!interlinearAnnotationScriptSupported()) return 0;
  // v1: the 8pt UI face. Registered unconditionally at startup (src/main.cpp), so no new font
  // loading and the slim build has it; SD-card font families register their own 8pt face under the
  // same id (SdCardFontSystem), so this follows the active family there too.
  return SMALL_FONT_ID;
}

bool CrossPointSettings::interlinearAnnotationScriptSupported() const {
  // notosans_8_regular covers Latin + Latin Extended (Vietnamese included), combining marks,
  // Cyrillic U+0400-04FF and Hebrew, but its interval table jumps straight from U+036F to U+0400 --
  // no Greek -- and it has no Devanagari, Thai, kana, Hangul, CJK or Arabic letters (only a handful
  // of Arabic punctuation marks and digits). For an uncovered target every glyph would render as
  // U+FFFD, and a CJK target is worse than missing: drawText reroutes any CJK-bearing string to a
  // fallback face whose advanceY differs from the height the row was measured and advanced with, so
  // the row would overflow its box and collide with the source line.
  //
  // Returning false makes getInterlinearAnnotationFontId() hand back 0, i.e. the BODY font: the rows
  // still sit above their sentences and the layout is still correct, they are simply not small. That
  // is the intended v1 degradation, and it is the ONLY thing this predicate does -- it is a face
  // decision, not a suppression.
  //
  // Hebrew/Arabic/Persian are listed for a second reason, but note what that does and does not buy:
  // the rows are still emitted for an RTL target, they are simply not ANCHORED. Anchoring an RTL row
  // over an LTR source sentence needs an end-side first-line inset, which BlockStyle cannot express
  // (CSS text-indent insets from the start edge, i.e. the right, under RTL), so v1 lays those rows out
  // un-anchored at the full measure on their own natural margin (flush right). See buildAnnotationRows
  // in lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp and the Version 40 section of
  // docs/file-formats.md.
  //
  // Listed by BCP-47 code rather than by table index so the list survives LANGUAGES[] being extended;
  // a language added there is treated as supported, which is right for the Latin/Cyrillic ones that
  // dominate the list and shows replacement glyphs (not a crash) for a new unsupported script.
  static constexpr const char* UNSUPPORTED_TARGETS[] = {"ar", "el", "fa", "he",    "hi",
                                                        "ja", "ko", "th", "zh-CN", "zh-TW"};
  if (translationLanguage >= LanguagePickerActivity::NUM_LANGUAGES) return false;  // unset target
  const char* code = LanguagePickerActivity::LANGUAGES[translationLanguage].code;
  for (const char* unsupported : UNSUPPORTED_TARGETS) {
    if (std::strcmp(code, unsupported) == 0) return false;
  }
  return true;
}

int CrossPointSettings::getTooltipTranslationFontId() const { return translationFontIdForSize(tooltipTranslationSize); }

int CrossPointSettings::getPageTranslationOverlayFontId() const {
  return translationFontIdForSize(pageTranslationSize);
}

PageFontSet CrossPointSettings::readerPageFontSet() const {
  // The same three ids readerRenderSpec() keys the cache on, read from the same accessors: a page is
  // always drawn in the fonts it was measured with. Only the two LAYOUT sizes belong here — the
  // overlays draw outside the Page. Each accessor is mode-gated and returns 0 (-> body font, via the
  // PageFontSet constructor) for every mode but its own.
  return PageFontSet(getReaderFontId(), getInterleavedTranslationFontId(), getInterlinearAnnotationFontId());
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  // Pre-Translation: the cache key carries the LAYOUT the mode implies, never the mode itself.
  // Drawing-only differences (translationShade) and the overlay modes therefore do not invalidate
  // a cached section. The INTERLEAVED translated-text font does change line breaking, so it IS keyed
  // — and it is the only one of the three sizes that is, which is why this reads the Interleaved
  // accessor by name rather than a shared one: the tooltip and Page Translation sizes are composited
  // over a finished page and must never reach a spec.
  spec.ptLayout = ptLayoutForDisplayMode(translationDisplayMode);
  spec.translationFontId = getInterleavedTranslationFontId();
  // The Interlinear annotation face is the second layout-affecting size, keyed for the same reason
  // (it decides both the annotation row's wrap and its pitch) and normalized to 0 by Section for
  // every layout but Interlinear.
  spec.annotationFontId = getInterlinearAnnotationFontId();
  // Not a cache key: the app's sentence aligner, injected so lib/Epub owns the geometry and the app
  // owns the alignment heuristic it shares with the Tooltip mode.
  spec.interlinearPairFn = &interlinearPairSentences;
  return spec;
}

float CrossPointSettings::getReaderLineCompression() const {
  // SD card fonts use same compression as Bookerly (the most neutral values)
  if (sdFontFamilyName[0] != '\0') {
    switch (lineSpacing) {
      case TIGHT:
        return 0.95f;
      case NORMAL:
      default:
        return 1.0f;
      case WIDE:
        return 1.1f;
    }
  }

  switch (fontFamily) {
    // EdsLab keeps the neutral spacing Noto Serif used in this slot; the values are
    // unchanged from that family, not retuned for EdsLab's metrics.
    case EDSLAB:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  fontPointSize =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  saveToFile();
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  // A built-in family only exists at BUILTIN_READER_POINT_SIZES, so a size
  // carried over from an SD family may not be one of them. ensureLoaded()
  // normally persists the snap; snap again here (without allocating — this runs
  // in the page render loop) so rendering is correct even before it has run.
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  return builtinReaderFontId(pt, fontFamily == NOTOSANS);
}

int CrossPointSettings::smallerReaderFontId() const {
  // SD card family: the manager keeps exactly ONE reader-size face loaded and
  // SdCardFontSystem::resolveFontId() ignores its pointSize argument by design
  // (src/SdCardFontSystem.cpp:173-178), so there is no smaller SD face to drop to — SD families
  // simply have no Smaller option.
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    if (sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize) != 0) return 0;
    // The named family has no loaded face; fall through to the built-in ladder, exactly as
    // getReaderFontId() does, so both agree on which family is actually rendering.
  }

  // Built-in families ship exactly BUILTIN_READER_POINT_SIZES. Snap first (fontPointSize may still
  // carry a size only an SD family had), then step one entry down.
  constexpr size_t kCount = std::size(BUILTIN_READER_POINT_SIZES);
  const uint8_t body = snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, kCount, fontPointSize);
  size_t idx = 0;
  while (idx < kCount && BUILTIN_READER_POINT_SIZES[idx] != body) idx++;
  if (idx == 0 || idx >= kCount) return 0;  // already the smallest size this family ships
  return builtinReaderFontId(BUILTIN_READER_POINT_SIZES[idx - 1], fontFamily == NOTOSANS);
}
