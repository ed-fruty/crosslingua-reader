#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  // Bitmask of text style flags carried per-word through layout and serialized in page cache.
  // Bits 0-1 select the font variant (BOLD/ITALIC); bits 2-7 are decoration/positioning/translation/
  // ruby-group overlays applied at render time without changing the underlying font. getFont() ignores
  // all bits above bit 1 so decorations compose freely with bold/italic (e.g. BOLD | UNDERLINE | SUP).
  enum Style : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,      // drawn as a line below baseline by TextBlock::render()
    STRIKETHROUGH = 8,  // drawn as a line through midline by TextBlock::render()
    SUP = 16,           // superscript: glyph scaled 50%, raised ~40% of ascender
    SUB = 32,           // subscript: glyph scaled 50%, lowered ~25% of ascender
    TRANSLATED =
        64,  // bit 6: word came from a translated block (lang= attribute set); used by Pre-Translation Dark/Light modes
    RUBY_CONTINUE = 128,  // bit 7: group ruby follower marker for native <ruby>/<rt> support; set at layout
                          // (ParsedText), persisted in the section.bin word style byte, read by TextBlock and
                          // ChapterHtmlSlimParser to keep ruby groups together. Upstream introduced this as 64,
                          // which collides with TRANSLATED on this line; renumbered to bit 7 in the section.bin
                          // upstream merge (which rejects every cache written by either line, so no stored style
                          // byte is ever reinterpreted under the new numbering). Bit 7 had been reserved for the
                          // Tooltip display mode (PT_TOOLTIP), but that flag was never set nor persisted
                          // anywhere -- the tooltip renders the page original-only and surfaces translations
                          // through an at-view popup -- so the reservation is retired in favor of ruby. The
                          // style byte is now FULL: any future flag needs a wider persisted word style
                          // (TextBlock arena styles[] is uint8_t) plus a section.bin version bump. getFont()
                          // ignores all bits above bit 1, so ruby composes with bold/italic/decorations.
  };
  static constexpr uint8_t TEXT_DECORATION_MASK = static_cast<uint8_t>(UNDERLINE | STRIKETHROUGH);

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;
  /// Returns true if the resolved style's font can render `cp` directly
  /// (interval coverage only — see EpdFont::hasCodepoint).
  bool hasCodepoint(uint32_t cp, Style style = REGULAR) const;
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp, Style style = REGULAR) const;
  uint32_t applyLigatures(uint32_t cp, const char*& text, Style style = REGULAR) const;
  static constexpr bool hasTextDecoration(const Style style) {
    return (static_cast<uint8_t>(style) & TEXT_DECORATION_MASK) != 0;
  }

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;

  const EpdFont* getFont(Style style) const;
};
