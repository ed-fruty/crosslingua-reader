#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <new>

namespace {

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::shared_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const PageFontSet& fonts, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fonts, xOffset, yOffset);
    }
  }
}

}  // namespace

// The two sentinels are declared in different libraries (lib/Epub must not depend on the concrete
// renderer type in a header), so pin them together where both are visible.
static_assert(PageFontSet::INK_INHERIT == GfxRenderer::INK_INHERIT,
              "PageFontSet ink sentinel must match the renderer's");

void PageLine::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) {
  // A line is homogeneous, so the role resolves to ONE id here and TextBlock keeps its plain
  // int fontId — the mixed-font page is a property of the page, not of any single line.
  //
  // Same for the ink: colour is applied at the LINE boundary from the role, not per word from a
  // style bit. Scope-guarded because a page is drawn three times (BW + LSB + MSB planes) and each
  // pass must set and clear it again; a latch would leak one line's colour into the next.
  const GfxRenderer::ForcedInkScope ink(renderer, fonts.inkForRole(fontRole));
  block->render(renderer, fonts.forRole(fontRole), xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  if (!block->serialize(file)) return false;
  // Pre-Translation: paragraph index (section-cache version bump forces a full re-read).
  serialization::writePod(file, paragraphIdx);
  // Per-line font role, one byte after paragraphIdx.
  serialization::writePod(file, static_cast<uint8_t>(fontRole));
  return true;
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

  // Pre-Translation: paragraph index written after the TextBlock (see serialize()).
  int16_t paragraphIdx = -1;
  serialization::readPod(file, paragraphIdx);

  // Font role byte. Always present -- the version check rejects every file written before the role
  // existed, so PageLine::deserialize only ever runs on pages that have it. An unknown value would
  // mean a corrupt file; clamp to Body rather than index a font set out of range.
  uint8_t roleByte = 0;
  serialization::readPod(file, roleByte);
  const LineFontRole fontRole = (roleByte <= static_cast<uint8_t>(LineFontRole::Annotation))
                                    ? static_cast<LineFontRole>(roleByte)
                                    : LineFontRole::Body;

  auto* line = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  line->paragraphIdx = paragraphIdx;
  line->fontRole = fontRole;
  return std::unique_ptr<PageLine>(line);
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
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
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

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

void Page::render(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fonts, xOffset, yOffset, [](const PageElement&) { return true; });
}

void Page::renderImages(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fonts, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const PageFontSet& fonts, const int xOffset,
                                       const int yOffset) const {
  for (const auto& element : elements) {
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

  // Pre-Translation: paragraph range for this page (appended after footnotes for back-compat).
  serialization::writePod(file, firstParagraphIdx);
  serialization::writePod(file, lastParagraphIdx);

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

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

  // Pre-Translation: paragraph range (always present: the section version bump invalidates
  // any pre-v32 file at the header check, so Page::deserialize only runs on v32+ pages).
  serialization::readPod(file, page->firstParagraphIdx);
  serialization::readPod(file, page->lastParagraphIdx);

  return page;
}
