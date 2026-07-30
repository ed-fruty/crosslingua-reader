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

// How the app resolves a role at render time: the font id per role, and the ink per role.
//
// lib/Epub deliberately knows nothing about fontIds.h: it stores roles, the app resolves them.
// Build one at the app boundary (CrossPointSettings::readerPageFontSet()) so the ids the page is
// DRAWN with always come from the same place as the ids it was MEASURED with
// (ReaderRenderSpec::fontId / translationFontId).
//
// UNSET SENTINEL: 0, matching the "font not found" sentinel that fontIds.h reserves. A role left
// at 0 falls back to the body font. Note that -1 is NOT a sentinel here — font ids are signed
// hashes and negative ids are perfectly normal (NOTOSANS_14_FONT_ID is negative), so only 0 can
// safely mean "unset".
//
// The INK is deliberately here rather than in a sibling struct: PageFontSet is already THE
// app->lib per-role render-time channel, built in one place and already threaded through every
// render() overload. A second parameter would have to be added to PageElement::render,
// PageLine::render, PageImage::render, PageHorizontalRule::render, Page::render, renderImages and
// renderWithImagePlaceholders for zero behavioural gain. Unlike the font ids, the ink is NOT a
// layout input and never reaches ReaderRenderSpec — changing it repaints, it never re-measures.
struct PageFontSet {
  // "This role has no app-supplied ink; keep the renderer's own per-word behaviour." Must equal
  // GfxRenderer::INK_INHERIT (static_assert in Page.cpp). Not 0, because 0 is a real ink (black).
  static constexpr uint8_t INK_INHERIT = 0xFF;

  int body = 0;
  int translation = 0;
  int annotation = 0;

  // Ink level (0=black, 1=dark gray, 2=light gray) the whole line draws in, or INK_INHERIT.
  // Only ever non-inherit for the mode that owns the role: the translation column in Side by Side,
  // the annotation rows in Interlinear. Body text is never inked from here.
  uint8_t translationInk = INK_INHERIT;
  uint8_t annotationInk = INK_INHERIT;
  // Drawing-only visibility gate. A hidden annotation line is still present in the laid-out Page,
  // so its vertical space remains reserved.
  bool annotationVisible = true;

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

  // Mirrors forRole() exactly, so a role added to LineFontRole fails the build in both places.
  constexpr uint8_t inkForRole(const LineFontRole role) const {
    switch (role) {
      case LineFontRole::Translation:
        return translationInk;
      case LineFontRole::Annotation:
        return annotationInk;
      case LineFontRole::Body:
        break;
    }
    return INK_INHERIT;
  }
};
