#include "TooltipOverlay.h"

#include <CrossPointSettings.h>
#include <HalStorage.h>
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
  translatedParagraphCount = 0;
  originalParagraphCount = 0;
  splits.count = 0;
}

// Strip HTML tags from a string, returning plain text.
static std::string stripHtmlTags(const char* html, int len) {
  std::string result;
  result.reserve(len);
  bool inTag = false;
  for (int i = 0; i < len; i++) {
    if (html[i] == '<') {
      inTag = true;
    } else if (html[i] == '>') {
      inTag = false;
    } else if (!inTag) {
      // Decode common HTML entities
      if (html[i] == '&' && i + 2 < len) {
        if (strncmp(html + i, "&amp;", 5) == 0) {
          result += '&';
          i += 4;
          continue;
        }
        if (strncmp(html + i, "&lt;", 4) == 0) {
          result += '<';
          i += 3;
          continue;
        }
        if (strncmp(html + i, "&gt;", 4) == 0) {
          result += '>';
          i += 3;
          continue;
        }
        if (strncmp(html + i, "&quot;", 6) == 0) {
          result += '"';
          i += 5;
          continue;
        }
        if (strncmp(html + i, "&#39;", 5) == 0 || strncmp(html + i, "&apos;", 6) == 0) {
          result += '\'';
          i += (html[i + 1] == '#') ? 4 : 5;
          continue;
        }
      }
      result += html[i];
    }
  }
  return result;
}

void TooltipOverlay::loadTranslationsFromHtml() {
  translatedParagraphCount = 0;

  if (translatedHtmlPath.empty()) return;

  FsFile f = Storage.open(translatedHtmlPath.c_str(), O_RDONLY);
  if (!f) {
    LOG_ERR("TIP", "Cannot open translated HTML: %s", translatedHtmlPath.c_str());
    return;
  }

  // Read the file in chunks, scanning for data-translation="true" markers.
  // Format: <tag ... data-translation="true" ...>translated text</tag>
  // We extract the text between > and </
  static constexpr int BUF_SIZE = 1024;
  char buf[BUF_SIZE];
  std::string accum;  // accumulates across chunk boundaries

  while (f.available()) {
    int bytesRead = f.read(reinterpret_cast<uint8_t*>(buf), BUF_SIZE - 1);
    if (bytesRead <= 0) break;
    buf[bytesRead] = '\0';
    accum.append(buf, bytesRead);

    // Process complete tags in accum
    size_t searchFrom = 0;
    while (searchFrom < accum.size()) {
      // Look for data-translation="true"
      size_t markerPos = accum.find("data-translation=\"true\"", searchFrom);
      if (markerPos == std::string::npos) break;

      // Find the closing > of this opening tag
      size_t tagEnd = accum.find('>', markerPos);
      if (tagEnd == std::string::npos) break;  // incomplete, wait for more data

      // Find the closing </
      size_t contentStart = tagEnd + 1;
      size_t closingTag = accum.find("</", contentStart);
      if (closingTag == std::string::npos) break;  // incomplete

      // Extract text content and strip inner HTML tags
      int contentLen = static_cast<int>(closingTag - contentStart);
      if (contentLen > 0 && translatedParagraphCount < MAX_PARAGRAPHS) {
        translatedParagraphs[translatedParagraphCount] =
            stripHtmlTags(accum.c_str() + contentStart, contentLen);
        translatedParagraphCount++;
      }

      searchFrom = closingTag + 2;
    }

    // Keep unprocessed tail (last incomplete tag)
    if (searchFrom > 0) {
      accum.erase(0, searchFrom);
    } else if (accum.size() > BUF_SIZE * 4) {
      // Safety: if we've accumulated too much without finding markers, discard
      accum.clear();
    }
  }

  f.close();
  LOG_DBG("TIP", "Loaded %d translated paragraphs from HTML", translatedParagraphCount);
}

void TooltipOverlay::matchPageTranslations(const Page& page) {
  // Extract original paragraph texts from the page's TextBlocks.
  // Paragraphs are detected by Y-position gaps > 30px (same heuristic as Page::extractText()).
  originalParagraphCount = 0;
  int16_t prevY = -1;
  std::string currentParagraph;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    if (words.empty()) continue;

    int16_t gap = (prevY >= 0) ? (el->yPos - prevY) : 0;
    if (prevY >= 0 && gap > 30) {
      // New paragraph — flush current
      if (!currentParagraph.empty() && originalParagraphCount < MAX_PARAGRAPHS) {
        originalParagraphs[originalParagraphCount++] = std::move(currentParagraph);
        currentParagraph.clear();
      }
    }
    prevY = el->yPos;

    for (const auto& w : words) {
      if (!currentParagraph.empty()) currentParagraph += ' ';
      currentParagraph += w;
    }
  }
  if (!currentParagraph.empty() && originalParagraphCount < MAX_PARAGRAPHS) {
    originalParagraphs[originalParagraphCount++] = std::move(currentParagraph);
  }

  // Match page paragraphs to translated paragraphs by substring search.
  // Build concatenated translated text for all matched paragraphs.
  transWordCount = 0;
  std::string matchedTranslation;

  for (int p = 0; p < originalParagraphCount; p++) {
    const auto& origPara = originalParagraphs[p];
    // Find first 30 chars of original in the translated HTML's original paragraphs context.
    // The translated paragraphs array is in the same order as the chapter's paragraphs.
    // We search by matching original text to find the paragraph index.
    // For simplicity: walk through translated paragraphs in order, find matching by index.
    // The translation for paragraph N in the chapter = translatedParagraphs[N].
    // We need to know which chapter paragraph index this page paragraph corresponds to.

    // Simple approach: search for a prefix of the original paragraph text in the chapter.
    // Since we process the .translated.html which has original then translated paragraphs,
    // and we extracted only translated paragraphs, the index in translatedParagraphs corresponds
    // to the chapter paragraph index. We match by finding which chapter paragraph starts with
    // the same text as the page paragraph.

    // For now: iterate through a "chapter original paragraphs" list.
    // But we don't have that. We only have translated paragraphs.
    // Alternative: just use the translated paragraphs in order, matched to page paragraphs in order.
    // This works if the page paragraphs are a contiguous subset of chapter paragraphs.
    // We use a static cursor that advances through the chapter.
  }

  // Simpler approach: build a single translated text blob from ALL translated paragraphs,
  // split into words, and use proportional mapping against original words.
  // This is less precise per-paragraph but works across any paragraph alignment.
  std::string allTranslated;
  for (int i = 0; i < translatedParagraphCount; i++) {
    if (i > 0) allTranslated += ' ';
    allTranslated += translatedParagraphs[i];
  }

  // Split translated text into words
  transWordCount = 0;
  const char* p = allTranslated.c_str();
  while (*p && transWordCount < MAX_TRANS_WORDS) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* wordStart = p;
    while (*p && *p != ' ') p++;
    transWordStorage[transWordCount] = std::string(wordStart, p - wordStart);
    transWordPtrs[transWordCount] = transWordStorage[transWordCount].c_str();
    transWordCount++;
  }
}

void TooltipOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  transWordCount = 0;

  // In CT_TOOLTIP mode, the page was built with CT_NO_RENDER — all words are original (grayLevel == 0).
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();

    for (auto wIt = words.begin(); wIt != words.end(); ++wIt) {
      if (origWordCount < MAX_WORDS) {
        origWordPtrs[origWordCount++] = wIt->c_str();
      }
    }
  }

  splits = splitSentences(origWordPtrs, origWordCount);

  // Load translations from the .translated.html file and build translated word arrays.
  loadTranslationsFromHtml();
  matchPageTranslations(page);
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
    const auto& xpositions = block->getWordXpos();

    auto wIt = words.begin();
    auto xIt = xpositions.begin();
    for (; wIt != words.end() && xIt != xpositions.end(); ++wIt, ++xIt) {
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

// Find the Y coordinate of the LAST line of a sentence on the page.
static int findSentenceLastLineY(const Page& page, const SentenceSpan& span, int yOffset) {
  int origWordIdx = 0;
  int lastLineY = 0;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();

    for (auto wIt = words.begin(); wIt != words.end(); ++wIt) {
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

  // Find the last line Y of this sentence (for positioning tooltip below multi-line sentences)
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

  // Position: above the first line or below the LAST line of the sentence
  const int spaceAbove = bounds.firstLineY - yOffset;
  int tooltipX = xOffset + PADDING;
  int tooltipY;

  if (tooltipHeight + GAP <= spaceAbove) {
    // Place above the first line
    tooltipY = bounds.firstLineY - GAP - tooltipHeight;
  } else {
    // Place below the LAST line of the sentence (not first line)
    tooltipY = lastLineY + lineHeight + GAP;
  }

  // Clamp to viewport
  if (tooltipY < yOffset + PADDING) tooltipY = yOffset + PADDING;
  if (tooltipY + tooltipHeight > yOffset + viewportHeight - PADDING) {
    tooltipY = yOffset + viewportHeight - PADDING - tooltipHeight;
  }

  // Draw tooltip background (white fill to erase text underneath)
  renderer.fillRect(tooltipX - 1, tooltipY - 1, tooltipWidth + 2, tooltipHeight + 2, false);

  // Draw border
  renderer.drawRoundedRect(tooltipX, tooltipY, tooltipWidth, tooltipHeight, 1, CORNER_RADIUS, true);

  // Draw tooltip text (word-wrapped)
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

  // Draw underline under the active sentence
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
