#pragma once
#include <cstdint>

#include "InterlinearAnnotation.h"
#include "PtLayout.h"

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every VALUE field: a section file built
// with a different spec is discarded and rebuilt. (interlinearPairFn is the one
// exception and is documented as such below.)
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
  // Pre-Translation: font the TRANSLATED text is laid out in. 0 -- and ONLY 0 -- means "same as
  // fontId": font ids are signed hashes, so a negative id is a perfectly normal font (see the
  // UNSET SENTINEL note in PageFontSet.h, which uses the same single-sentinel rule).
  // A distinct font changes word measurement and therefore line breaking, so unlike the drawing-
  // only shade this IS part of the cache key -- but only under PtLayout::Both, the one layout that
  // lays translated words out in it (OriginalOnly drops them; TranslationOnly and SideBySide lay
  // them out in the body font by design). Section normalizes it to 0 for every other layout, for the
  // key and for the layout engine alike; callers may set the real id unconditionally.
  int translationFontId = 0;
  // Pre-Translation (PtLayout::Interlinear): font the small ANNOTATION rows are laid out in. 0 --
  // and only 0 -- means "same as fontId" (see the sentinel note above); that is also the graceful
  // answer when the annotation face cannot cover the target script, in which case the rows still
  // appear above their sentences, just at body size.
  //
  // A genuine layout input, keyed exactly like translationFontId and for the same reason: it decides
  // both how an annotation row wraps and how tall it is. Section normalizes it to 0 for every layout
  // but Interlinear (keyedAnnotationFontId), for the key and the layout engine alike, so a future
  // annotation-size row cannot invalidate the cache of a mode that draws no annotations.
  int annotationFontId = 0;
  // Pre-Translation (PtLayout::Interlinear): the app's sentence aligner. NOT a cache key -- it is a
  // pure function of the two texts, identical in every build, so it cannot change what a cached page
  // contains; it is carried here only because this struct is already the one channel from the app to
  // the layout engine. nullptr disables annotation emission (the source paragraph then lays out
  // exactly as under Original Only). See lib/Epub/Epub/InterlinearAnnotation.h.
  InterlinearPairFn interlinearPairFn = nullptr;

  // True when a chapter laid out under `other` can be served under this spec without a
  // re-layout, i.e. both specs key the SAME section.bin. Compares every VALUE field and
  // deliberately omits interlinearPairFn, the one field documented above as not a cache
  // key. Lives here, next to the fields, so a new keyed field has exactly one place to be
  // added rather than a second list in a caller.
  //
  // Slightly STRICTER than Section's on-disk header check, which first normalizes the two
  // translation font ids for the effective layout (keyedTranslationFontId /
  // keyedAnnotationFontId): this can therefore report a difference where the on-disk key
  // is identical. That direction is safe -- it costs at most one redundant re-resolve that
  // hits the cache with unchanged pagination -- while the reverse would silently serve
  // pages measured with the wrong font.
  bool layoutEquals(const ReaderRenderSpec& other) const {
    return fontId == other.fontId && lineCompression == other.lineCompression &&
           extraParagraphSpacing == other.extraParagraphSpacing && paragraphAlignment == other.paragraphAlignment &&
           viewportWidth == other.viewportWidth && viewportHeight == other.viewportHeight &&
           hyphenationEnabled == other.hyphenationEnabled && embeddedStyle == other.embeddedStyle &&
           imageRendering == other.imageRendering && focusReadingEnabled == other.focusReadingEnabled &&
           ptLayout == other.ptLayout && translationFontId == other.translationFontId &&
           annotationFontId == other.annotationFontId;
  }
};
