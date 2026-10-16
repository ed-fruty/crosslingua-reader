#pragma once

// CrossLingua fork: gray ink for translated text, applied at the two glyph draw points of
// GfxRenderer.cpp (renderCharScaled / renderCharImpl) so the upstream draw code stays verbatim.

#include "BitmapHelpers.h"
#include "GfxRenderer.h"

namespace linguaInk {

// 0 = black (no Lingua ink); 1 = dark gray; 2 = light gray. A per-role ink set by the line being
// drawn (PageLine::render -> ForcedInkScope) OVERRIDES the per-word translated flag
// (TextBlock::render -> TranslatedWordScope), including with 0: a Lingua colour is a property of the
// whole line, and "black" has to be able to win over translated words. INK_INHERIT means no line
// asked.
inline uint8_t levelFor(const GfxRenderer& renderer) {
  const uint8_t forcedInk = renderer.getForcedInk();
  if (forcedInk != GfxRenderer::INK_INHERIT) return forcedInk;
  return renderer.isDrawingTranslatedWord() ? renderer.getTranslationGrayLevel() : 0;
}

// One ink pixel of a scaled (SUP/SUB) glyph; level 0 is the plain drawPixel.
//   BW pass: full coverage as ink, so the word stays visible without the gray pass.
//   Overlay gray planes: dark gray flags MSB + LSB, light gray MSB only.
//   Absolute gray planes each hold their own image: write that plane's value of the level.
inline void paintPixel(const GfxRenderer& renderer, const int x, const int y, const bool pixelState,
                       const uint8_t level) {
  const GfxRenderer::RenderMode mode = renderer.getRenderMode();
  if (level == 0 || mode == GfxRenderer::BW) {
    renderer.drawPixel(x, y, pixelState);
  } else if (renderer.grayPlanesAreAbsolute()) {
    renderer.drawPixel(x, y, grayPlanePixel(level, mode == GfxRenderer::GRAYSCALE_MSB, true).black);
  } else if (level == 1 || mode == GfxRenderer::GRAYSCALE_MSB) {
    renderer.drawPixel(x, y, false);
  }
}

// renderCharImpl's final draw. `renderMode` is the mode the glyph path resolved (absolute planes
// force BW for plain text); `passMode` is the pass actually being rendered. Same rules as
// paintPixel. On the BW plane, state=false sets the bit for every non-white value, which is exactly
// "flag the glyph's full coverage" on a gray plane.
inline void drawGlyph(const GfxRenderer& renderer, const uint8_t* bitmap, const int width, const int height,
                      const glyphBitmap::Frame& frame, const bool is2Bit, const GfxRenderer::RenderMode renderMode,
                      const bool pixelState, const uint8_t level, const GfxRenderer::RenderMode passMode) {
  if (level == 0) {
    renderer.drawGlyphBitmap(bitmap, width, height, frame, is2Bit, renderMode, pixelState);
  } else if (passMode == GfxRenderer::BW) {
    renderer.drawGlyphBitmap(bitmap, width, height, frame, is2Bit, GfxRenderer::BW, pixelState);
  } else if (renderer.grayPlanesAreAbsolute()) {
    const bool black = grayPlanePixel(level, passMode == GfxRenderer::GRAYSCALE_MSB, true).black;
    renderer.drawGlyphBitmap(bitmap, width, height, frame, is2Bit, GfxRenderer::BW, black);
  } else if (level == 1 || passMode == GfxRenderer::GRAYSCALE_MSB) {
    renderer.drawGlyphBitmap(bitmap, width, height, frame, is2Bit, GfxRenderer::BW, false);
  }
}

}  // namespace linguaInk
