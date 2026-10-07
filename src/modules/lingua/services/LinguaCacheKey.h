#pragma once

#include "modules/lingua/layout/LinguaLayout.h"

// Section-cache key normalization for the Lingua font ids (section.bin header, see Section.cpp).
namespace lingua {

// The translation font belongs in the cache key only where translated words are actually laid out IN
// IT, which is the Both layout and only Both -- the one layout that flows the two languages inline,
// so a second type size there re-breaks lines (see ChapterHtmlSlimParser::currentLineRole, which
// tags a line for that font under exactly this layout and no other). Under OriginalOnly the
// translated words are all dropped, and under TranslationOnly / SideBySide they are laid out in the
// BODY font by design, so in all three no line was measured in the translation font and no value of
// it can move a line break. Stamping the real id there would make a change to the Interleaved mode's
// Translation Size invalidate every cached chapter of the book, including chapters of a mode that
// lays out no differently-sized text at all. (The two modes that MAP to OriginalOnly, Tooltip and
// Page Translation, own SEPARATE size settings which are composited at view time and by design never
// reach a ReaderRenderSpec -- see CrossPointSettings::TRANSLATION_SIZE. This normalization covers the
// id that does reach one.)
//
// Keyed off the EFFECTIVE (post-fallback) layout, not the requested one, and applied to the header
// write, the lookup AND the id handed to the parser, so key and layout can never disagree: a chapter
// with no committed translation is laid out as Both even in Tooltip mode (Section::effectiveLayout),
// and keyedTranslationFontId keeps the real id for it exactly as it would for an actually-translated
// chapter. That is NOT a don't-care: ChapterHtmlSlimParser::currentBlockIsTranslated is a lang=
// mismatch against the book's primary language, not "this chapter has a committed Lingua" --
// so the chapter's OWN original HTML can carry lang-tagged blocks (a foreign epigraph, a quoted
// phrase) that currentLineRole() still tags LineFontRole::Translation under Both and lays out in this
// font. Zeroing the id here for an untranslated chapter would let a later change to it (e.g. the
// Interleaved mode's Smaller Translation Size) silently keep serving those blocks from a stale cache
// at the wrong size instead of invalidating it.
constexpr int keyedTranslationFontId(const int translationFontId, const LinguaLayout effectiveLayout) {
  return effectiveLayout == LinguaLayout::Both ? translationFontId : 0;
}

// Same rule, same reasoning, for the annotation font: it only reaches a page under the ONE layout
// that emits LineFontRole::Annotation rows (Interlinear), where it decides both the row's wrap and
// its pitch. Under every other layout no line is measured in it, so stamping the real id there would
// let a future annotation-size row invalidate every cached chapter of a mode that draws no
// annotations at all. Applied in all three places the translation one is -- the header write, the
// lookup compare and the id handed to the parser -- so the key and the layout can never disagree.
constexpr int keyedAnnotationFontId(const int annotationFontId, const LinguaLayout effectiveLayout) {
  return effectiveLayout == LinguaLayout::Interlinear ? annotationFontId : 0;
}

}  // namespace lingua
