#include "TooltipOverlay.h"

#include <CrossPointSettings.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "fontIds.h"

bool TooltipOverlay::handleInput(MappedInputManager& input) {
  const bool useFrontButtons = (SETTINGS.tooltipButtons == 0);
  const auto nextBtn =
      useFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
  const auto backBtn =
      useFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;

  if (input.wasReleased(nextBtn)) {
    if (currentSentenceIndex < 0) {
      currentSentenceIndex = 0;
      wrapAround = false;
      return true;
    }
    if (currentSentenceIndex < splits.count - 1) {
      currentSentenceIndex++;
      return true;
    }
    if (!wrapAround) {
      currentSentenceIndex = -1;
      wrapAround = true;
      return true;
    }
    currentSentenceIndex = 0;
    wrapAround = false;
    return true;
  }

  if (input.wasReleased(backBtn)) {
    if (currentSentenceIndex >= 0) {
      currentSentenceIndex = -1;
      return true;
    }
    return false;
  }

  return false;
}

void TooltipOverlay::onPageChanged() {
  currentSentenceIndex = -1;
  wrapAround = false;
  pagePrepared = false;
  origWordCount = 0;
  transWordCount = 0;
  splits.count = 0;
}

void TooltipOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  transWordCount = 0;

  // Walk all PageLine elements. Original words have grayLevel==0, translated have grayLevel>0.
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    for (; wIt != words.end() && sIt != styles.end(); ++wIt, ++sIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel == 0) {
        if (origWordCount < MAX_WORDS) {
          origWordPtrs[origWordCount++] = wIt->c_str();
        }
      } else {
        if (transWordCount < MAX_WORDS) {
          transWordPtrs[transWordCount++] = wIt->c_str();
        }
      }
    }
  }

  splits = splitSentences(origWordPtrs, origWordCount);
  LOG_DBG("TIP", "Page: %d orig words, %d trans words, %d sentences", origWordCount, transWordCount, splits.count);
}

TooltipOverlay::SentenceBounds TooltipOverlay::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                   int fontId, int xOffset, int yOffset) const {
  SentenceBounds bounds = {0, 0, 0};
  int origWordIdx = 0;
  bool foundFirst = false;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();
    const auto& xpositions = block->getWordXpos();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    auto xIt = xpositions.begin();
    for (; wIt != words.end() && sIt != styles.end() && xIt != xpositions.end(); ++wIt, ++sIt, ++xIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel > 0) continue;

      if (origWordIdx >= span.startWord && origWordIdx < span.endWord) {
        const int wordX = *xIt + line->xPos + xOffset;
        const int wordY = line->yPos + yOffset;

        if (!foundFirst) {
          bounds.firstLineY = wordY;
          bounds.startX = wordX;
          bounds.endX = wordX;
          foundFirst = true;
        }

        if (wordY == bounds.firstLineY) {
          if (wordX < bounds.startX) bounds.startX = wordX;
          if (wordX > bounds.endX) bounds.endX = wordX;
        }
      }
      origWordIdx++;
    }
  }

  return bounds;
}

void TooltipOverlay::drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span,
                                           int fontId, int xOffset, int yOffset) const {
  int origWordIdx = 0;
  int currentLineY = -1;
  int lineStartX = 0;
  int lineEndX = 0;
  const int underlineY_offset = renderer.getFontAscenderSize(fontId) + 2;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();
    const auto& xpositions = block->getWordXpos();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    auto xIt = xpositions.begin();
    for (; wIt != words.end() && sIt != styles.end() && xIt != xpositions.end(); ++wIt, ++sIt, ++xIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel > 0) continue;

      if (origWordIdx >= span.startWord && origWordIdx < span.endWord) {
        const int wordX = *xIt + line->xPos + xOffset;
        const int wordY = line->yPos + yOffset;
        const int wordWidth =
            renderer.getTextWidth(fontId, wIt->c_str(),
                                  static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(*sIt) & 0x1F));

        if (wordY != currentLineY) {
          if (currentLineY >= 0) {
            renderer.drawLine(lineStartX, currentLineY + underlineY_offset, lineEndX,
                              currentLineY + underlineY_offset, true);
          }
          currentLineY = wordY;
          lineStartX = wordX;
          lineEndX = wordX + wordWidth;
        } else {
          lineEndX = wordX + wordWidth;
        }
      }
      origWordIdx++;
    }
  }

  if (currentLineY >= 0) {
    renderer.drawLine(lineStartX, currentLineY + underlineY_offset, lineEndX, currentLineY + underlineY_offset, true);
  }
}

static int findSentenceLastLineY(const Page& page, const SentenceSpan& span, int yOffset) {
  int origWordIdx = 0;
  int lastLineY = 0;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    for (; wIt != words.end() && sIt != styles.end(); ++wIt, ++sIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel > 0) continue;

      if (origWordIdx >= span.startWord && origWordIdx < span.endWord) {
        lastLineY = line->yPos + yOffset;
      }
      origWordIdx++;
    }
  }
  return lastLineY;
}

void TooltipOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset,
                            int yOffset, int viewportWidth, int viewportHeight) {
  if (currentSentenceIndex < 0) return;

  preparePage(page);

  if (currentSentenceIndex >= splits.count) {
    currentSentenceIndex = -1;
    return;
  }

  const SentenceSpan& span = splits.spans[currentSentenceIndex];

  char translationBuffer[512];
  MappedSentenceResult mapped = mapSentenceTranslations(origWordPtrs, origWordCount, transWordPtrs, transWordCount,
                                                        splits, translationBuffer, sizeof(translationBuffer));

  const char* tooltipText = "";
  if (currentSentenceIndex < mapped.count) {
    tooltipText = mapped.sentences[currentSentenceIndex].translatedText;
  }

  if (tooltipText[0] == '\0') return;

  const int lineHeight = renderer.getLineHeight(fontId);
  SentenceBounds bounds = findSentenceBounds(page, span, fontId, xOffset, yOffset);
  if (bounds.firstLineY == 0 && bounds.startX == 0) return;

  const int lastLineY = findSentenceLastLineY(page, span, yOffset);

  constexpr int PADDING = 6;
  constexpr int CORNER_RADIUS = 3;
  constexpr int GAP = 4;
  const int maxTooltipWidth = viewportWidth - 2 * PADDING;

  const int textWidth = renderer.getTextWidth(tooltipFontId, tooltipText);
  const int tooltipLineHeight = renderer.getLineHeight(tooltipFontId);

  int tooltipWidth;
  int tooltipHeight;
  int numLines = 1;

  if (textWidth <= maxTooltipWidth - 2 * PADDING) {
    tooltipWidth = textWidth + 2 * PADDING;
    tooltipHeight = tooltipLineHeight + 2 * PADDING;
  } else {
    tooltipWidth = maxTooltipWidth;
    const int availableTextWidth = tooltipWidth - 2 * PADDING;
    numLines = (textWidth + availableTextWidth - 1) / availableTextWidth;
    const int maxLines = (viewportHeight * 4 / 10) / tooltipLineHeight;
    if (numLines > maxLines) numLines = maxLines;
    tooltipHeight = numLines * tooltipLineHeight + 2 * PADDING;
  }

  const int spaceAbove = bounds.firstLineY - yOffset;
  int tooltipX = xOffset + PADDING;
  int tooltipY;

  if (tooltipHeight + GAP <= spaceAbove) {
    tooltipY = bounds.firstLineY - GAP - tooltipHeight;
  } else {
    tooltipY = lastLineY + lineHeight + GAP;
  }

  if (tooltipY < yOffset + PADDING) tooltipY = yOffset + PADDING;
  if (tooltipY + tooltipHeight > yOffset + viewportHeight - PADDING) {
    tooltipY = yOffset + viewportHeight - PADDING - tooltipHeight;
  }

  renderer.fillRect(tooltipX - 1, tooltipY - 1, tooltipWidth + 2, tooltipHeight + 2, false);
  renderer.drawRoundedRect(tooltipX, tooltipY, tooltipWidth, tooltipHeight, 1, CORNER_RADIUS, true);

  const int textX = tooltipX + PADDING;
  int textY = tooltipY + PADDING;
  const int availWidth = tooltipWidth - 2 * PADDING;
  const int spaceW = renderer.getSpaceWidth(tooltipFontId);

  const char* p = tooltipText;
  int lineCount = 0;
  while (*p && lineCount < numLines) {
    int lineWidth = 0;
    const char* lineStart = p;
    const char* lastWordEnd = p;

    while (*p) {
      const char* wordStart = p;
      while (*p && *p != ' ') p++;
      const int wLen = static_cast<int>(p - wordStart);

      char wordBuf[128];
      const int copyLen = (wLen < 127) ? wLen : 127;
      memcpy(wordBuf, wordStart, copyLen);
      wordBuf[copyLen] = '\0';
      const int wordWidth = renderer.getTextWidth(tooltipFontId, wordBuf);

      if (lineWidth > 0 && lineWidth + spaceW + wordWidth > availWidth) {
        p = wordStart;
        break;
      }

      lineWidth += (lineWidth > 0 ? spaceW : 0) + wordWidth;
      lastWordEnd = p;

      while (*p == ' ') p++;
    }

    const int drawLen = static_cast<int>(lastWordEnd - lineStart);
    if (drawLen > 0) {
      char lineBuf[256];
      const int cLen = (drawLen < 255) ? drawLen : 255;
      memcpy(lineBuf, lineStart, cLen);
      lineBuf[cLen] = '\0';
      renderer.drawText(tooltipFontId, textX, textY, lineBuf);
      textY += tooltipLineHeight;
      lineCount++;
    }

    if (p == lineStart) break;
  }

  drawSentenceUnderline(renderer, page, span, fontId, xOffset, yOffset);
}

int getTooltipFontId() {
  if (SETTINGS.fontSize <= CrossPointSettings::SMALL) {
    return SETTINGS.getReaderFontId();
  }
  const uint8_t smallerSize = SETTINGS.fontSize - 1;
  switch (SETTINGS.fontFamily) {
    case CrossPointSettings::BOOKERLY:
    default:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return BOOKERLY_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default: return BOOKERLY_14_FONT_ID;
        case CrossPointSettings::LARGE: return BOOKERLY_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return BOOKERLY_18_FONT_ID;
      }
    case CrossPointSettings::EDSLAB:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return EDSLAB_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default: return EDSLAB_14_FONT_ID;
        case CrossPointSettings::LARGE: return EDSLAB_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return EDSLAB_18_FONT_ID;
      }
    case CrossPointSettings::ALEGREYA:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return ALEGREYA_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default: return ALEGREYA_14_FONT_ID;
        case CrossPointSettings::LARGE: return ALEGREYA_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return ALEGREYA_18_FONT_ID;
      }
    case CrossPointSettings::GPRO:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return GPRO_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default: return GPRO_14_FONT_ID;
        case CrossPointSettings::LARGE: return GPRO_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return GPRO_18_FONT_ID;
      }
  }
}
