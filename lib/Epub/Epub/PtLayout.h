#pragma once
#include <cstdint>

// The distinct PAGE LAYOUTS the chapter parser can produce for the Pre-Translation feature.
//
// This is deliberately NOT the user's display-mode setting (CrossPointSettings::
// PRE_TRANSLATION_MODE). Several modes produce byte-identical pages and differ only in DRAWING
// (the Paragraph shade, which just picks a gray level) or in an OVERLAY composited on top at view
// time (Tooltip, Modal). The section.bin cache is keyed on the LAYOUT, so switching between two
// modes that share one reuses the cached pages instead of forcing a full chapter re-layout.
//
// The app owns the mode -> layout mapping (CrossPointSettings::ptLayoutForDisplayMode); the layout
// engine only ever sees the layout, which is what keeps lib/Epub free of app-level mode semantics.
//
// VALUE STABILITY: serialized into the section.bin header as a uint8_t and part of the cache key.
// Renumbering or adding a value requires a SECTION_FILE_VERSION bump.
enum class PtLayout : uint8_t {
  // Every word survives, laid out in ONE full-width sequential flow. This is the layout of a plain
  // untranslated chapter AND of every mode that shows original and translation inline (Normal,
  // Paragraph, Interlinear). Paragraph differs from Normal only in the gray level the renderer
  // draws translated words at; Interlinear will grow its own layout in a later change.
  Both = 0,
  // Translated words are dropped: only the source text is laid out. Shared by Original Only and by
  // the overlay modes (Modal, Tooltip), whose translations are composited at view time and would
  // double the text if they were also emitted inline.
  OriginalOnly = 1,
  // Untranslated words are dropped: only the translation is laid out.
  TranslationOnly = 2,
  // Original and translation paired into two half-width columns (renderSideBySide).
  SideBySide = 3,
};
