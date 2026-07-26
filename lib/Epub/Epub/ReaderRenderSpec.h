#pragma once
#include <cstdint>

#include "PtLayout.h"

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every field: a section file built
// with a different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which
// fills every field: the settings-derived ones from the store, the viewport
// from the caller. Taking the viewport as arguments is what keeps a spec from
// existing in a half-filled state — the 0 defaults below are a last-resort
// backstop (a 0x0 viewport lays out nothing), not an invitation to omit it.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  // Pre-Translation: the page LAYOUT the display mode implies, NOT the raw mode. Different modes
  // that produce identical pages share one layout, so switching between them is a cache HIT
  // instead of a full chapter re-layout. See PtLayout.h and
  // CrossPointSettings::ptLayoutForDisplayMode().
  PtLayout ptLayout = PtLayout::Both;
  // Pre-Translation: font the TRANSLATED text is laid out in. 0 (and -1) mean "same as fontId".
  // A distinct font changes word measurement and therefore line breaking, so unlike the drawing-
  // only shade this IS part of the cache key.
  int translationFontId = 0;
};
