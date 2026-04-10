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

// Skip leading whitespace and em-space (U+2003 = \xe2\x80\x83) for text comparison.
// The layout engine prepends em-space for text-indent, which isn't in the source HTML.
static const char* skipLeadingSpace(const char* s) {
  while (*s) {
    if (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
      s++;
    } else if (static_cast<uint8_t>(s[0]) == 0xE2 && static_cast<uint8_t>(s[1]) == 0x80 &&
               static_cast<uint8_t>(s[2]) == 0x83) {
      s += 3;  // skip em-space (3 bytes UTF-8)
    } else {
      break;
    }
  }
  return s;
}

// Compare two strings ignoring leading whitespace/em-space. Returns true if first N chars match.
static bool prefixMatch(const std::string& a, const std::string& b, int minLen = 5) {
  const char* pa = skipLeadingSpace(a.c_str());
  const char* pb = skipLeadingSpace(b.c_str());
  int la = strlen(pa);
  int lb = strlen(pb);
  int cmpLen = std::min({la, lb, 30});
  if (cmpLen < minLen) return false;
  return strncmp(pa, pb, cmpLen) == 0;
}

// Streaming HTML parser that extracts translated paragraph texts matching the given page paragraphs.
// Reads the file in small chunks to avoid large allocations on ESP32.
// Returns concatenated translations for matched paragraphs (space-separated).
static std::string extractMatchedTranslations(const std::string& path, const std::vector<std::string>& pageParas) {
  std::string result;
  if (path.empty() || pageParas.empty()) return result;

  FsFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f) {
    LOG_ERR("TIP", "Cannot open HTML: %s", path.c_str());
    return result;
  }

  // Stream the file in chunks, accumulating tag-level fragments.
  // We track: last original paragraph text (for matching) and whether we're inside a data-translation block.
  static constexpr int CHUNK = 512;
  char buf[CHUNK];
  std::string accum;       // rolling buffer of HTML being scanned
  std::string lastOrigPara;  // last non-translation paragraph text
  size_t pageParaIdx = 0;  // which page paragraph we're trying to match next

  while (f.available() || !accum.empty()) {
    // Read more data if available
    if (f.available()) {
      int n = f.read(reinterpret_cast<uint8_t*>(buf), CHUNK - 1);
      if (n > 0) {
        buf[n] = '\0';
        accum.append(buf, n);
      }
    }

    bool madeProgress = false;

    // Find opening block tags (p, h1-h6) and process them
    for (size_t i = 0; i < accum.size(); i++) {
      if (accum[i] != '<') continue;
      if (i + 2 >= accum.size()) break;
      char t = accum[i + 1];
      bool isBlock = false;
      if (t == 'p' && (accum[i + 2] == ' ' || accum[i + 2] == '>')) isBlock = true;
      if (t == 'h' && accum[i + 2] >= '1' && accum[i + 2] <= '6') isBlock = true;
      if (!isBlock) continue;

      // Find end of this opening tag
      size_t tagEnd = accum.find('>', i);
      if (tagEnd == std::string::npos) break;

      // Determine tag name for finding closing tag
      char tagName[4] = {t, 0, 0, 0};
      if (t == 'h') { tagName[1] = accum[i + 2]; }
      std::string closeStr = std::string("</") + tagName;

      size_t closePos = accum.find(closeStr, tagEnd);
      if (closePos == std::string::npos) break;  // incomplete, need more data

      size_t endOfClose = accum.find('>', closePos);
      if (endOfClose == std::string::npos) break;

      // Extract tag attributes and content
      std::string openTag = accum.substr(i, tagEnd - i + 1);
      std::string content = accum.substr(tagEnd + 1, closePos - tagEnd - 1);
      std::string plainText = stripTags(content);

      // Trim
      while (!plainText.empty() && (plainText.front() == ' ' || plainText.front() == '\n' || plainText.front() == '\r'))
        plainText.erase(0, 1);
      while (!plainText.empty() && (plainText.back() == ' ' || plainText.back() == '\n' || plainText.back() == '\r'))
        plainText.pop_back();

      // A paragraph is a translation if it has data-translation="true" (our translator)
      // OR a lang= attribute (Calibre/embedded translations).
      // Skip lang on <html> and <body> which we already excluded (only p/h1-h6 reach here).
      bool isTrans = openTag.find("data-translation") != std::string::npos ||
                     openTag.find("lang=") != std::string::npos ||
                     openTag.find("xml:lang=") != std::string::npos;

      if (isTrans) {
        // This is a translated paragraph. Check if lastOrigPara matches the current page paragraph.
        if (!plainText.empty() && !lastOrigPara.empty() && pageParaIdx < pageParas.size()) {
          if (prefixMatch(pageParas[pageParaIdx], lastOrigPara)) {
            if (!result.empty()) result += ' ';
            result += plainText;
            pageParaIdx++;
          }
        }
        lastOrigPara.clear();
      } else {
        if (!plainText.empty()) {
          lastOrigPara = std::move(plainText);
        }
      }

      // Consume up to end of closing tag
      accum.erase(0, endOfClose + 1);
      madeProgress = true;
      break;  // restart scan from beginning of accum
    }

    if (!madeProgress) {
      if (!f.available()) break;  // no more data and no progress
      // If accum is getting too large without progress, trim old data
      if (accum.size() > CHUNK * 8) {
        accum.erase(0, accum.size() - CHUNK * 2);
      }
    }

    // Early exit if all page paragraphs matched
    if (pageParaIdx >= pageParas.size()) break;
  }

  f.close();
  LOG_DBG("TIP", "Matched %d/%d page paras from HTML", (int)pageParaIdx, (int)pageParas.size());
  if (pageParaIdx == 0 && !pageParas.empty()) {
    LOG_DBG("TIP", "First page para (%.40s...)", skipLeadingSpace(pageParas[0].c_str()));
    LOG_DBG("TIP", "Last HTML orig (%.40s...)", lastOrigPara.empty() ? "<empty>" : lastOrigPara.c_str());
  }
  return result;
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

  // 3. Stream through the HTML file, match page paragraphs, extract translations.
  std::string matchedTranslation = extractMatchedTranslations(translatedHtmlPath, pageParas);
  if (matchedTranslation.empty()) {
    LOG_DBG("TIP", "No translations matched for this page");
    return;
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
