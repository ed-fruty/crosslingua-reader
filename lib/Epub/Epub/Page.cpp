#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

namespace {

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const PageFontSet& fonts, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fonts, xOffset, yOffset);
    }
  }
}

bool isBorderBox(const PageElement& element) { return element.getTag() == TAG_PageBorderBox; }

void renderBoxBackgrounds(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                          const int xOffset, const int yOffset) {
  for (const auto& element : elements) {
    if (isBorderBox(*element)) {
      static_cast<const PageBorderBox&>(*element).renderBackground(renderer, xOffset, yOffset);
    }
  }
}

// Draws one border edge as a strip along a box side.
void drawBorderEdge(const GfxRenderer& renderer, const int x, const int y, const int length, const CssBorderSide& side,
                    const bool horizontal) {
  const int thickness = side.width;
  const auto strip = [&](const int offset, const int along, const int span, const int depth) {
    if (horizontal) {
      renderer.fillRect(x + along, y + offset, span, depth, true);
    } else {
      renderer.fillRect(x + offset, y + along, depth, span, true);
    }
  };
  switch (side.style) {
    case CssBorderStyle::Double:
      if (thickness >= 3) {
        const int line = std::max(1, thickness / 3);
        strip(0, 0, length, line);
        strip(thickness - line, 0, length, line);
        return;
      }
      break;
    case CssBorderStyle::Dotted:
    case CssBorderStyle::Dashed: {
      const int dash = side.style == CssBorderStyle::Dotted ? thickness : std::max(3, thickness * 3);
      const int gap = side.style == CssBorderStyle::Dotted ? std::max(2, thickness) : std::max(2, thickness * 2);
      for (int along = 0; along < length; along += dash + gap)
        strip(0, along, std::min(dash, length - along), thickness);
      return;
    }
    default:
      break;
  }
  strip(0, 0, length, thickness);
}

}  // namespace

// The two sentinels are declared in different libraries (lib/Epub must not depend on the concrete
// renderer type in a header), so pin them together where both are visible.
static_assert(PageFontSet::INK_INHERIT == GfxRenderer::INK_INHERIT,
              "PageFontSet ink sentinel must match the renderer's");

void PageLine::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) {
  if (fontRole == LineFontRole::Annotation && !fonts.annotationVisible) return;
  // A line is homogeneous, so the role resolves to ONE id here and TextBlock keeps its plain
  // int fontId — the mixed-font page is a property of the page, not of any single line.
  //
  // Same for the ink: colour is applied at the LINE boundary from the role, overriding the per-word
  // translated flag. Scope-guarded because a page is drawn three times (BW + LSB + MSB planes) and each
  // pass must set and clear it again; a latch would leak one line's colour into the next.
  const GfxRenderer::ForcedInkScope ink(renderer, fonts.inkForRole(fontRole));
  block->render(renderer, fonts.forRole(fontRole), xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file) && serializeLinguaFields(file);  // Lingua: + paragraphIdx, fontRole, translated
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto line = makeUniqueNoThrow<PageLine>(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  if (!line->deserializeLinguaFields(file)) return nullptr;  // Lingua
  return line;
}

bool PageLine::serializeLinguaFields(HalFile& file) const {
  serialization::writePod(file, paragraphIdx);
  // Per-line font role, one byte after paragraphIdx.
  serialization::writePod(file, static_cast<uint8_t>(fontRole));
  // Which words are translated text (see LinguaWordFlags.h for the encoding).
  return block->linguaTranslatedWords().serialize(file, block->wordCount());
}

bool PageLine::deserializeLinguaFields(HalFile& file) {
  // Paragraph index written after the TextBlock (see serialize()).
  serialization::readPod(file, paragraphIdx);
  // Font role byte. Always present -- the version check rejects every file written before the role
  // existed, so PageLine::deserialize only ever runs on pages that have it. An unknown value would
  // mean a corrupt file; clamp to Body rather than index a font set out of range.
  uint8_t roleByte = 0;
  serialization::readPod(file, roleByte);
  fontRole = (roleByte <= static_cast<uint8_t>(LineFontRole::Annotation)) ? static_cast<LineFontRole>(roleByte)
                                                                          : LineFontRole::Body;
  return block->linguaTranslatedWords().deserialize(file, block->wordCount());
}

void PageImage::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) {
  // Images don't use fonts or text rendering
  (void)fonts;
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: null ImageBlock");
    return nullptr;
  }
  auto image = makeUniqueNoThrow<PageImage>(std::move(ib), xPos, yPos);
  if (!image) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return image;
}

void PageHorizontalRule::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) {
  (void)fonts;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, thickness);

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto rule = makeUniqueNoThrow<PageHorizontalRule>(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return rule;
}

PageDropCap::PageDropCap(const int32_t fontId, const uint16_t scale256, const EpdFontFamily::Style style,
                         const char* utf8, const int16_t xPos, const int16_t yPos)
    : PageElement(xPos, yPos), fontId(fontId), scale256(scale256), style(style) {
  strncpy(text, utf8, MAX_TEXT_BYTES);
}

void PageDropCap::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) {
  // A sized variant that can no longer be loaded draws from the section font instead.
  const int drawFontId = renderer.ensureFontLoaded(fontId) ? fontId : fonts.body;
  int x = xPos + xOffset;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
    x += renderer.drawScaledCodepoint(drawFontId, cp, style, x, yPos + yOffset, scale256);
  }
}

bool PageDropCap::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, fontId);
  serialization::writePod(file, scale256);
  serialization::writePod(file, static_cast<uint8_t>(style));
  const auto length = static_cast<uint8_t>(strnlen(text, MAX_TEXT_BYTES));
  serialization::writePod(file, length);
  return file.write(text, length) == length;
}

std::unique_ptr<PageDropCap> PageDropCap::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  int32_t fontId = 0;
  uint16_t scale256 = 0;
  uint8_t style = 0;
  uint8_t length = 0;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, fontId);
  serialization::readPod(file, scale256);
  serialization::readPod(file, style);
  serialization::readPod(file, length);
  char text[MAX_TEXT_BYTES + 1] = {};
  if (length == 0 || length > MAX_TEXT_BYTES || scale256 == 0 || file.read(text, length) != length) {
    LOG_ERR("PGE", "Deserialization failed: invalid drop cap");
    return nullptr;
  }
  auto dropCap =
      makeUniqueNoThrow<PageDropCap>(fontId, scale256, static_cast<EpdFontFamily::Style>(style), text, xPos, yPos);
  if (!dropCap) LOG_ERR("PGE", "Deserialization failed: could not allocate PageDropCap");
  return dropCap;
}

void PageBorderBox::renderBackground(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  if (shaded) renderer.fillRectDither(xPos + xOffset, yPos + yOffset, width, height, Color::LightGray);
}

void PageBorderBox::render(GfxRenderer& renderer, const PageFontSet&, const int xOffset, const int yOffset) {
  const int x = xPos + xOffset;
  const int y = yPos + yOffset;
  const CssBorderSide& top = sides[0];
  const CssBorderSide& right = sides[1];
  const CssBorderSide& bottom = sides[2];
  const CssBorderSide& left = sides[3];
  if (top.visible()) drawBorderEdge(renderer, x, y, width, top, true);
  if (bottom.visible()) drawBorderEdge(renderer, x, y + height - bottom.width, width, bottom, true);
  if (left.visible()) drawBorderEdge(renderer, x, y, height, left, false);
  if (right.visible()) drawBorderEdge(renderer, x + width - right.width, y, height, right, false);
}

bool PageBorderBox::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  for (const CssBorderSide& side : sides) {
    serialization::writePod(file, side.width);
    serialization::writePod(file, static_cast<uint8_t>(side.style));
  }
  serialization::writePod(file, shaded);
  return true;
}

std::unique_ptr<PageBorderBox> PageBorderBox::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  CssBorderSide sides[4];
  bool shaded = false;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, height);
  for (CssBorderSide& side : sides) {
    uint8_t style = 0;
    serialization::readPod(file, side.width);
    serialization::readPod(file, style);
    if (style > static_cast<uint8_t>(CssBorderStyle::Dashed)) {
      LOG_ERR("PGE", "Deserialization failed: invalid border style %u", style);
      return nullptr;
    }
    side.style = static_cast<CssBorderStyle>(style);
  }
  serialization::readPod(file, shaded);
  if (width == 0 || height == 0) {
    LOG_ERR("PGE", "Deserialization failed: empty border box");
    return nullptr;
  }
  auto box = makeUniqueNoThrow<PageBorderBox>(width, height, sides, shaded, xPos, yPos);
  if (!box) LOG_ERR("PGE", "Deserialization failed: could not allocate PageBorderBox");
  return box;
}

// Backgrounds precede every border, including borders inside shaded containers.
void Page::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) const {
  renderBoxBackgrounds(elements, renderer, xOffset, yOffset);
  renderFilteredPageElements(elements, renderer, fonts, xOffset, yOffset, isBorderBox);
  renderFilteredPageElements(elements, renderer, fonts, xOffset, yOffset,
                             [](const PageElement& element) { return !isBorderBox(element); });
}

void Page::renderImages(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fonts, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset,
                                       const int yOffset) const {
  renderBoxBackgrounds(elements, renderer, xOffset, yOffset);
  renderFilteredPageElements(elements, renderer, fonts, xOffset, yOffset, isBorderBox);
  for (const auto& element : elements) {
    if (isBorderBox(*element)) continue;
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else {
      element->render(renderer, fonts, xOffset, yOffset);
    }
  }
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  // Lingua: paragraph range for this page (appended after footnotes for back-compat).
  serialization::writePod(file, firstParagraphIdx);
  serialization::writePod(file, lastParagraphIdx);

  const uint16_t linkCount = std::min<uint16_t>(links.size(), MAX_LINKS_PER_PAGE);
  serialization::writePod(file, linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    const auto& link = links[i];
    if (file.write(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to write link %u", i);
      return false;
    }
    serialization::writePod(file, link.x);
    serialization::writePod(file, link.y);
    serialization::writePod(file, link.width);
    serialization::writePod(file, link.height);
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = makeUniqueNoThrow<Page>();
  if (!page) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }

  uint16_t count;
  serialization::readPod(file, count);

  // Reserve up front so a page load costs one allocation for the element vector
  // instead of a grow-copy-free cycle every doubling. `count` is untrusted (it
  // comes straight off the SD cache), so clamp it: a real page holds a few dozen
  // elements, while a corrupt header could ask for 65535 * sizeof(unique_ptr) and
  // abort() on the failed allocation (vector's operator new is throwing, and this
  // firmware builds with -fno-exceptions). Under-reserving is harmless -- the
  // push_back path below still grows normally.
  static constexpr uint16_t RESERVE_CAP = 256;
  page->elements.reserve(std::min(count, RESERVE_CAP));

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else if (tag == TAG_PageDropCap) {
      auto dropCap = PageDropCap::deserialize(file);
      if (!dropCap) {
        return nullptr;
      }
      page->elements.push_back(std::move(dropCap));
    } else if (tag == TAG_PageBorderBox) {
      auto box = PageBorderBox::deserialize(file);
      if (!box) {
        return nullptr;
      }
      page->elements.push_back(std::move(box));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  // Lingua: paragraph range (always present: the section version bump invalidates
  // any pre-v32 file at the header check, so Page::deserialize only runs on v32+ pages).
  serialization::readPod(file, page->firstParagraphIdx);
  serialization::readPod(file, page->lastParagraphIdx);

  uint16_t linkCount;
  serialization::readPod(file, linkCount);
  if (linkCount > MAX_LINKS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid link count %u", linkCount);
    return nullptr;
  }
  page->links.resize(linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    auto& link = page->links[i];
    if (file.read(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to read link %u", i);
      return nullptr;
    }
    link.href[sizeof(link.href) - 1] = '\0';
    serialization::readPod(file, link.x);
    serialization::readPod(file, link.y);
    serialization::readPod(file, link.width);
    serialization::readPod(file, link.height);
    if (link.href[0] == '\0' || link.width <= 0 || link.height <= 0) {
      LOG_ERR("PGE", "Invalid link geometry %u", i);
      return nullptr;
    }
  }

  return page;
}
