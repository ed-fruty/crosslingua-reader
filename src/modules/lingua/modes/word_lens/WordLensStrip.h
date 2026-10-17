#pragma once
#include <cstddef>
#include <cstdint>

// Word Lens: placing the glosses of one source line on the annotation strip above it.
//
// Each gloss wants to sit centred over its word, clamped to the line. Where two would overlap the
// RARER word (higher rank) keeps its gloss; the other is shortened with an ellipsis to the gap left
// around its word, or dropped when fewer than MIN_TRUNCATED_CODEPOINTS would remain. Pure geometry
// over caller-measured widths, so it is host-tested without a renderer.
namespace lingua::wordlens {

inline constexpr size_t STRIP_TEXT_BYTES = 100;
inline constexpr size_t MIN_TRUNCATED_CODEPOINTS = 3;

struct StripGloss {
  // In: the word's extent on the line and its rank; the gloss text (NUL-terminated, may be
  // shortened in place).
  int16_t wordX = 0;
  int16_t wordWidth = 0;
  uint16_t rank = 0;
  char text[STRIP_TEXT_BYTES] = {};
  // Out.
  int16_t x = 0;
  int16_t width = 0;
  bool placed = false;
  bool truncated = false;
};

// Width of `text` in the annotation face.
using StripMeasureFn = int (*)(void* ctx, const char* text);

// Place `count` glosses (in reading order) on a strip `lineWidth` wide, keeping at least `minGap`
// between neighbours. Glosses that cannot be placed come back with placed == false.
void placeStripGlosses(StripGloss* glosses, size_t count, int lineWidth, int minGap, StripMeasureFn measure, void* ctx);

}  // namespace lingua::wordlens
