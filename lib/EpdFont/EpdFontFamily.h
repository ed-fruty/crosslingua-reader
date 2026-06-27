#pragma once
#include "EpdFont.h"

class EpdFontFamily {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, UNDERLINE = 4 };

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = REGULAR) const;
  bool hasPrintableChars(const char* string, Style style = REGULAR) const;
  const EpdFontData* getData(Style style = REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = REGULAR) const;

  // Glyph fallback: when this family lacks a codepoint, missing glyphs are sourced from
  // `fallback` (and its chain). Wire same-size families so e.g. EdsLab borrows Bookerly's
  // curly quotes / em-space. The pointer must outlive this family (use global font objects).
  void setFallback(const EpdFontFamily* fallback) { this->fallback = fallback; }

  // Resolve `cp` through this family then its fallback chain. On success returns the glyph and
  // sets *ownerData to the font data whose bitmap holds it (needed to actually render it).
  // Returns nullptr if no font in the chain has the glyph.
  const EpdGlyph* resolveGlyph(uint32_t cp, Style style, const EpdFontData** ownerData) const;

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* boldItalic;
  const EpdFontFamily* fallback = nullptr;

  const EpdFont* getFont(Style style) const;
};
