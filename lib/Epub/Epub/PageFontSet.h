#pragma once
#include <cstdint>

// What a laid-out line IS, so a page can mix type sizes without the layout library knowing which
// concrete fonts exist. The role is decided at layout time and baked into the cached page; the app
// resolves it to a font id at render time through PageFontSet.
//
// VALUE STABILITY: serialized as one byte per PageLine. Append only; renumbering needs a
// SECTION_FILE_VERSION bump.
enum class LineFontRole : uint8_t {
  Body = 0,         // the book's own text, in the reader font
  Translation = 1,  // translated text (Pre-Translation), optionally in a smaller font
  Annotation = 2,   // editorial furniture inserted by the reader, e.g. a "not translated" marker
};

// The font id per role, supplied by the app at render time.
//
// lib/Epub deliberately knows nothing about fontIds.h: it stores roles, the app resolves them.
// Build one at the app boundary (CrossPointSettings::readerPageFontSet()) so the ids the page is
// DRAWN with always come from the same place as the ids it was MEASURED with
// (ReaderRenderSpec::fontId / translationFontId).
//
// UNSET SENTINEL: 0, matching the "font not found" sentinel that fontIds.h reserves. A role left
// at 0 falls back to the body font. Note that -1 is NOT a sentinel here — font ids are signed
// hashes and negative ids are perfectly normal (NOTOSERIF_14_FONT_ID is negative), so only 0 can
// safely mean "unset".
struct PageFontSet {
  int body = 0;
  int translation = 0;
  int annotation = 0;

  constexpr PageFontSet() = default;
  // Uniform set: every role draws in the body font. This is today's behaviour for every mode.
  constexpr explicit PageFontSet(const int bodyId) : body(bodyId), translation(bodyId), annotation(bodyId) {}
  constexpr PageFontSet(const int bodyId, const int translationId, const int annotationId)
      : body(bodyId),
        translation(translationId != 0 ? translationId : bodyId),
        annotation(annotationId != 0 ? annotationId : bodyId) {}

  constexpr int forRole(const LineFontRole role) const {
    switch (role) {
      case LineFontRole::Translation:
        return translation;
      case LineFontRole::Annotation:
        return annotation;
      case LineFontRole::Body:
        break;
    }
    return body;
  }
};
