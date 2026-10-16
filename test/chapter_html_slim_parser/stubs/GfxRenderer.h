#pragma once

#include <EpdFontFamily.h>
#include <Utf8.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  // Lingua per-line ink (Page.cpp scopes it per PageLine); the fixture draws nothing.
  static constexpr uint8_t INK_INHERIT = 0xFF;
  class ForcedInkScope {
   public:
    ForcedInkScope(GfxRenderer&, uint8_t) {}
  };
  // Lingua per-word translated flag (TextBlock::render scopes it per word); drawText records it.
  mutable bool drawingTranslatedWord = false;
  struct TranslatedWordScope {
    const GfxRenderer& renderer;
    bool previous;
    TranslatedWordScope(const GfxRenderer& r, const bool translated) : renderer(r), previous(r.drawingTranslatedWord) {
      r.drawingTranslatedWord = translated;
    }
    ~TranslatedWordScope() { renderer.drawingTranslatedWord = previous; }
  };
  // Optional draw log: every drawText appends (text, drawn as a translated word).
  mutable std::vector<std::pair<std::string, bool>>* drawLog = nullptr;
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    return left == 0 || left == ' ' || right == ' ' ? 0 : tracking;
  }
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  void drawText(int, int, int, const char* text, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {
    if (drawLog) drawLog->emplace_back(text, drawingTranslatedWord);
  }
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    int width = 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      width += 8 + trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t) const {}
};
