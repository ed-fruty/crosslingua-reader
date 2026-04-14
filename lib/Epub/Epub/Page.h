#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,  // New tag
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(FsFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  int16_t paragraphIdx = -1;  // Which paragraph this line belongs to (for modal translation)

  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos, int16_t paragraphIdx = -1)
      : PageElement(xPos, yPos), block(std::move(block)), paragraphIdx(paragraphIdx) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  const std::shared_ptr<TextBlock>& getTextBlock() const { return block; }
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  std::shared_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::shared_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;

  // Paragraph indices (from parser's sequential counter).
  // Used by modal translation to know which paragraphs are on this page.
  // -1 means unknown (old cache format).
  int16_t firstParagraphIdx = -1;
  int16_t lastParagraphIdx = -1;
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);

  // Extract plain text from all TextBlocks on this page.
  std::string extractText() const;

  // Extract only translated text (words with grayLevel > 0) from this page.
  std::string extractTranslatedText() const;

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(),
                       [](const std::shared_ptr<PageElement>& el) { return el->getTag() == TAG_PageImage; });
  }
};
