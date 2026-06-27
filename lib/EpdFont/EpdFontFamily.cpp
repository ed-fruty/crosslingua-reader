#include "EpdFontFamily.h"

#include <Utf8.h>

#include <algorithm>

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

const EpdGlyph* EpdFontFamily::resolveGlyph(const uint32_t cp, const Style style, const EpdFontData** ownerData) const {
  const EpdFont* primary = getFont(style);
  if (const EpdGlyph* g = primary->getGlyph(cp)) {
    *ownerData = primary->data;
    return g;
  }
  for (const EpdFontFamily* fam = fallback; fam != nullptr; fam = fam->fallback) {
    const EpdFont* f = fam->getFont(style);
    if (const EpdGlyph* g = f->getGlyph(cp)) {
      *ownerData = f->data;
      return g;
    }
  }
  *ownerData = primary->data;
  return nullptr;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  // Without a fallback, defer to the single-font path (unchanged behaviour).
  if (fallback == nullptr) {
    getFont(style)->getTextDimensions(string, w, h);
    return;
  }
  // Fallback-aware bounds: mirror EpdFont::getTextBounds but resolve each glyph through the
  // chain so measurement matches what renderChar() will actually draw (layout consistency).
  int minX = 0, maxX = 0, minY = 0, maxY = 0, cursorX = 0;
  const auto* p = reinterpret_cast<const uint8_t*>(string);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    const EpdFontData* ownerData = nullptr;
    const EpdGlyph* glyph = resolveGlyph(cp, style, &ownerData);
    if (!glyph) glyph = resolveGlyph(REPLACEMENT_GLYPH, style, &ownerData);
    if (!glyph) continue;
    minX = std::min(minX, cursorX + glyph->left);
    maxX = std::max(maxX, cursorX + glyph->left + glyph->width);
    minY = std::min(minY, static_cast<int>(glyph->top - glyph->height));
    maxY = std::max(maxY, static_cast<int>(glyph->top));
    cursorX += glyph->advanceX;
  }
  *w = maxX - minX;
  *h = maxY - minY;
}

bool EpdFontFamily::hasPrintableChars(const char* string, const Style style) const {
  int w = 0, h = 0;
  getTextDimensions(string, &w, &h, style);
  return w > 0 || h > 0;
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
  return getFont(style)->getGlyph(cp);
};
