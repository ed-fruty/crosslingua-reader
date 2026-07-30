#pragma once
#include <cstdint>

// The distinct PAGE LAYOUTS the chapter parser can produce for the Lingua feature.
//
// This is deliberately NOT the user's display-mode setting (CrossPointSettings::
// LINGUA_MODE). Several modes produce byte-identical pages and differ only in DRAWING
// (the Paragraph shade, which just picks a gray level) or in an OVERLAY composited on top at view
// time (Tooltip, Page Translation). The section.bin cache is keyed on the LAYOUT, so switching between two
// modes that share one reuses the cached pages instead of forcing a full chapter re-layout.
//
// The app owns the mode -> layout mapping (CrossPointSettings::linguaLayoutForDisplayMode); the layout
// engine only ever sees the layout, which is what keeps lib/Epub free of app-level mode semantics.
//
// VALUE STABILITY: serialized into the section.bin header as a uint8_t and part of the cache key.
// Renumbering or adding a value requires a SECTION_FILE_VERSION bump.
enum class LinguaLayout : uint8_t {
  // Every word survives, laid out in ONE full-width sequential flow. This is the layout of a plain
  // untranslated chapter AND of the two modes that show original and translation inline (Normal,
  // Interleaved). Interleaved differs from Normal only in the gray level the renderer draws
  // translated words at, which never moves a glyph, so their pages are byte-identical.
  Both = 0,
  // Translated words are dropped: only the source text is laid out. Shared by Original Only and by
  // the overlay modes (Page Translation, Tooltip), whose translations are composited at view time and would
  // double the text if they were also emitted inline.
  OriginalOnly = 1,
  // Untranslated words are dropped: only the translation is laid out.
  TranslationOnly = 2,
  // Original and translation paired into two half-width columns (renderSideBySide).
  SideBySide = 3,
  // The source flows COMPLETELY NORMALLY -- identical breaking, indent and justification to
  // OriginalOnly -- and every source line gets exactly one small annotation strip directly above it,
  // so the page reads strictly annotation, source, annotation, source all the way down. A sentence's
  // translation flows through the strips above its own source lines, one line of it per line of
  // source, starting at the x its source sentence starts at (renderInterlinear). Strips are ordinary
  // PageLines tagged LineFontRole::Annotation, so they consume real vertical space and the source
  // text tiles around them -- which is why this cannot be an overlay composited at view time
  // (Tooltip / Page Translation) and must be a layout of its own.
  //
  // Costs exactly (bodyLineHeight + annotationLineHeight) / bodyLineHeight, about +57% pages at the
  // 14pt/8pt portrait default: the page carries a second full rendition of the text at ~57% of the
  // body pitch and nothing else. It does NOT degrade with short-sentence prose, because no sentence
  // ends its line early.
  Interlinear = 4,
};
