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
  transWordStorage.clear();
  splits.count = 0;
}

// ── HTML parsing helpers ──────────────────────────────────────────────────────

// Strip HTML tags and decode basic entities. Returns plain text.
static std::string stripTags(const std::string& html) {
  std::string out;
  out.reserve(html.size());
  bool inTag = false;
  for (size_t i = 0; i < html.size(); i++) {
    char c = html[i];
    if (c == '<') {
      inTag = true;
    } else if (c == '>') {
      inTag = false;
    } else if (!inTag) {
      if (c == '&') {
        if (html.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; continue; }
        if (html.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; continue; }
        if (html.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; continue; }
        if (html.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; continue; }
        if (html.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; continue; }
        if (html.compare(i, 5, "&#39;") == 0) { out += '\''; i += 4; continue; }
      }
      out += c;
    }
  }
  return out;
}

// Parse the translated HTML file and extract (original_paragraph, translated_paragraph) pairs.
// Original paragraphs are block elements WITHOUT data-translation; translated have it.
// They alternate: original, then its translation.
struct ParagraphPair {
  std::string original;
  std::string translated;
};

static std::vector<ParagraphPair> extractParagraphPairs(const std::string& path) {
  std::vector<ParagraphPair> pairs;

  FsFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f) return pairs;

  // Read entire file (chapter HTML is typically <50KB, fits in RAM briefly)
  const size_t fileSize = f.size();
  if (fileSize == 0 || fileSize > 200000) {
    f.close();
    return pairs;
  }
  std::string html;
  html.resize(fileSize);
  f.read(reinterpret_cast<uint8_t*>(&html[0]), fileSize);
  f.close();

  // Walk through the HTML collecting translated paragraph texts.
  // Format: <p ... data-translation="true" ...>text</p>
  // The PRECEDING block element (without data-translation) is the original.
  std::string lastOriginalText;
  size_t pos = 0;

  while (pos < html.size()) {
    // Find next opening tag
    size_t tagStart = html.find('<', pos);
    if (tagStart == std::string::npos) break;

    // Skip closing tags, comments, doctype
    if (tagStart + 1 < html.size() && (html[tagStart + 1] == '/' || html[tagStart + 1] == '!')) {
      pos = html.find('>', tagStart);
      if (pos == std::string::npos) break;
      pos++;
      continue;
    }

    // Find end of opening tag
    size_t tagEnd = html.find('>', tagStart);
    if (tagEnd == std::string::npos) break;

    std::string tag = html.substr(tagStart, tagEnd - tagStart + 1);

    // Check if this is a block element (p, h1-h6, li, blockquote, div)
    bool isBlock = false;
    if (tag.size() > 2) {
      char t = tag[1];
      if (t == 'p' && (tag[2] == ' ' || tag[2] == '>')) isBlock = true;
      if (t == 'h' && tag[2] >= '1' && tag[2] <= '6') isBlock = true;
      if (tag.compare(1, 2, "li") == 0) isBlock = true;
      if (tag.compare(1, 10, "blockquote") == 0) isBlock = true;
      if (tag.compare(1, 3, "div") == 0) isBlock = true;
    }

    if (!isBlock) {
      pos = tagEnd + 1;
      continue;
    }

    // Find the matching closing tag
    // Determine tag name
    size_t nameEnd = tag.find_first_of(" >", 1);
    std::string tagName = tag.substr(1, nameEnd - 1);
    std::string closeTag = "</" + tagName;

    size_t contentStart = tagEnd + 1;
    size_t closePos = html.find(closeTag, contentStart);
    if (closePos == std::string::npos) {
      pos = tagEnd + 1;
      continue;
    }

    std::string content = html.substr(contentStart, closePos - contentStart);
    std::string plainText = stripTags(content);

    // Trim whitespace
    while (!plainText.empty() && (plainText.front() == ' ' || plainText.front() == '\n')) plainText.erase(0, 1);
    while (!plainText.empty() && (plainText.back() == ' ' || plainText.back() == '\n')) plainText.pop_back();

    bool isTranslation = tag.find("data-translation") != std::string::npos;

    if (isTranslation) {
      if (!plainText.empty() && !lastOriginalText.empty()) {
        pairs.push_back({lastOriginalText, plainText});
      }
      lastOriginalText.clear();
    } else {
      if (!plainText.empty()) {
        lastOriginalText = plainText;
      }
    }

    // Move past closing tag
    pos = html.find('>', closePos);
    if (pos == std::string::npos) break;
    pos++;
  }

  LOG_DBG("TIP", "Extracted %d paragraph pairs from HTML", (int)pairs.size());
  return pairs;
}

// ── Page preparation ──────────────────────────────────────────────────────────

void TooltipOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  transWordCount = 0;
  transWordStorage.clear();

  // 1. Collect original words from page TextBlocks (all words — CT_NO_RENDER has only originals).
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    for (auto wIt = words.begin(); wIt != words.end(); ++wIt) {
      if (origWordCount < MAX_WORDS) {
        origWordPtrs[origWordCount++] = wIt->c_str();
      }
    }
  }

  splits = splitSentences(origWordPtrs, origWordCount);

  // 2. Extract page paragraph texts (group words by Y-gap > 30px).
  std::vector<std::string> pageParas;
  int16_t prevY = -1;
  std::string curPara;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    if (words.empty()) continue;

    int16_t gap = (prevY >= 0) ? (el->yPos - prevY) : 0;
    if (prevY >= 0 && gap > 30) {
      if (!curPara.empty()) {
        pageParas.push_back(std::move(curPara));
        curPara.clear();
      }
    }
    prevY = el->yPos;
    for (const auto& w : words) {
      if (!curPara.empty()) curPara += ' ';
      curPara += w;
    }
  }
  if (!curPara.empty()) pageParas.push_back(std::move(curPara));

  // 3. Extract (original, translated) paragraph pairs from the HTML file.
  auto pairs = extractParagraphPairs(translatedHtmlPath);
  if (pairs.empty() || pageParas.empty()) {
    LOG_DBG("TIP", "No pairs (%d) or no page paras (%d)", (int)pairs.size(), (int)pageParas.size());
    return;
  }

  // 4. Match page paragraphs to chapter paragraph pairs.
  // Page paragraphs are a contiguous subset of chapter originals. Find the starting match.
  // Compare first 30 chars of each page paragraph to chapter originals.
  std::string matchedTranslation;
  size_t chapterIdx = 0;
  for (const auto& pagePara : pageParas) {
    const int cmpLen = std::min((int)pagePara.size(), 30);
    bool found = false;
    for (size_t j = chapterIdx; j < pairs.size(); j++) {
      const int pairCmpLen = std::min((int)pairs[j].original.size(), cmpLen);
      if (pairCmpLen >= 5 && pagePara.compare(0, pairCmpLen, pairs[j].original, 0, pairCmpLen) == 0) {
        if (!matchedTranslation.empty()) matchedTranslation += ' ';
        matchedTranslation += pairs[j].translated;
        chapterIdx = j + 1;  // advance cursor
        found = true;
        break;
      }
    }
    if (!found) {
      LOG_DBG("TIP", "No match for page para: %.30s...", pagePara.c_str());
    }
  }

  // 5. Split matched translation into words.
  transWordStorage.clear();
  transWordCount = 0;
  const char* p = matchedTranslation.c_str();
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* wordStart = p;
    while (*p && *p != ' ') p++;
    transWordStorage.emplace_back(wordStart, p - wordStart);
  }
  for (int i = 0; i < (int)transWordStorage.size() && i < MAX_WORDS; i++) {
    transWordPtrs[i] = transWordStorage[i].c_str();
    transWordCount++;
  }

  LOG_DBG("TIP", "Page: %d orig words, %d trans words, %d sentences, %d page paras matched",
          origWordCount, transWordCount, splits.count, (int)pageParas.size());
}

// ── Sentence bounds and underline ─────────────────────────────────────────────

TooltipOverlay::SentenceBounds TooltipOverlay::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                   int fontId, int xOffset, int yOffset) const {
  SentenceBounds bounds = {0, 0, 0};
  int origWordIdx = 0;
  bool foundFirst = false;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    const auto& xpositions = line->getTextBlock()->getWordXpos();

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
    const auto& words = line->getTextBlock()->getWords();
    const auto& styles = line->getTextBlock()->getWordStyles();
    const auto& xpositions = line->getTextBlock()->getWordXpos();

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

// ── Tooltip rendering ─────────────────────────────────────────────────────────

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

// ── Font helper ───────────────────────────────────────────────────────────────

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
        case CrossPointSettings::MEDIUM: default: return BOOKERLY_14_FONT_ID;
        case CrossPointSettings::LARGE: return BOOKERLY_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return BOOKERLY_18_FONT_ID;
      }
    case CrossPointSettings::EDSLAB:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return EDSLAB_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return EDSLAB_14_FONT_ID;
        case CrossPointSettings::LARGE: return EDSLAB_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return EDSLAB_18_FONT_ID;
      }
    case CrossPointSettings::ALEGREYA:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return ALEGREYA_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return ALEGREYA_14_FONT_ID;
        case CrossPointSettings::LARGE: return ALEGREYA_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return ALEGREYA_18_FONT_ID;
      }
    case CrossPointSettings::GPRO:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return GPRO_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return GPRO_14_FONT_ID;
        case CrossPointSettings::LARGE: return GPRO_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return GPRO_18_FONT_ID;
      }
  }
}
