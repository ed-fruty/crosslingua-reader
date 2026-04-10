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
    LOG_DBG("TIP", "Next button released, currentSentence=%d, splits=%d", currentSentenceIndex, splits.count);
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

// ── Expat-based HTML parser for extracting translated paragraphs ───────────────
// Uses the same expat library as ChapterHtmlSlimParser for robust HTML handling.

#include <expat.h>

static const char* BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "div", nullptr};

static bool isBlockTag(const char* name) {
  for (int i = 0; BLOCK_TAGS[i]; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

struct ExpatExtractState {
  // Collects ALL translated paragraph texts from the chapter, in order.
  std::vector<std::string> allTranslations;

  int blockDepth = 0;
  bool inBlock = false;
  bool isTranslation = false;
  std::string currentText;
};

static void XMLCALL expatStartEl(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* s = static_cast<ExpatExtractState*>(ud);

  if (isBlockTag(name)) {
    s->inBlock = true;
    s->blockDepth = 1;
    s->isTranslation = false;
    s->currentText.clear();

    // Check for lang or data-translation attributes
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0 ||
            strcmp(atts[i], "data-translation") == 0) {
          // Skip lang on html/body (but those aren't block tags so won't reach here)
          s->isTranslation = true;
        }
      }
    }
  } else if (s->inBlock) {
    s->blockDepth++;
  }
}

static void XMLCALL expatEndEl(void* ud, const XML_Char* name) {
  auto* s = static_cast<ExpatExtractState*>(ud);

  if (s->inBlock) {
    s->blockDepth--;
    if (s->blockDepth <= 0) {
      s->inBlock = false;

      auto& t = s->currentText;
      while (!t.empty() && (t.front() == ' ' || t.front() == '\n' || t.front() == '\r')) t.erase(0, 1);
      while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r')) t.pop_back();

      if (!t.empty() && s->isTranslation) {
        s->allTranslations.push_back(t);
      }
      s->currentText.clear();
    }
  }
}

static void XMLCALL expatCharData(void* ud, const XML_Char* data, int len) {
  auto* s = static_cast<ExpatExtractState*>(ud);
  if (!s->inBlock) return;

  // Append text, collapsing whitespace
  for (int i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && !s->currentText.empty() && s->currentText.back() == ' ') continue;
    s->currentText += c;
  }
}

// Extract ALL translated paragraphs from the HTML using expat.
static std::vector<std::string> extractAllTranslations(const std::string& path) {
  std::vector<std::string> empty;
  if (path.empty()) return empty;

  FsFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f) {
    LOG_ERR("TIP", "Cannot open HTML: %s", path.c_str());
    return empty;
  }

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    f.close();
    return empty;
  }

  ExpatExtractState state;
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, expatStartEl, expatEndEl);
  XML_SetCharacterDataHandler(parser, expatCharData);

  static constexpr int CHUNK = 1024;
  char buf[CHUNK];
  const size_t fileSize = f.size();
  size_t totalRead = 0;

  while (totalRead < fileSize) {
    const size_t toRead = ((fileSize - totalRead) < CHUNK) ? (fileSize - totalRead) : CHUNK;
    const int bytesRead = f.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (bytesRead <= 0) break;
    totalRead += bytesRead;
    const bool done = (totalRead >= fileSize);
    if (XML_Parse(parser, buf, bytesRead, done ? 1 : 0) == XML_STATUS_ERROR) {
      LOG_ERR("TIP", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      break;
    }
  }

  XML_ParserFree(parser);
  f.close();

  LOG_DBG("TIP", "Extracted %d translated paragraphs from HTML", (int)state.allTranslations.size());
  return std::move(state.allTranslations);
}

// ── Page preparation ──────────────────────────────────────────────────────────

void TooltipOverlay::preparePage(const Page& page, int pageIndex, int pageCount) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  transWordCount = 0;
  transWordStorage.clear();

  // 1. Collect original words from page TextBlocks.
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

  // 2. Extract ALL translated paragraphs from the HTML.
  auto allTrans = extractAllTranslations(translatedHtmlPath);
  if (allTrans.empty()) {
    LOG_DBG("TIP", "No translated paragraphs found in HTML");
    return;
  }

  // 3. Estimate which portion of translations belongs to this page.
  // Split all translations into words first, then take the page's proportional slice.
  std::vector<std::string> allTransWords;
  for (const auto& para : allTrans) {
    const char* p = para.c_str();
    while (*p) {
      while (*p == ' ') p++;
      if (!*p) break;
      const char* ws = p;
      while (*p && *p != ' ') p++;
      allTransWords.emplace_back(ws, p - ws);
    }
  }
  allTrans.clear();  // free paragraph strings

  if (allTransWords.empty() || pageCount <= 0) {
    LOG_DBG("TIP", "No translated words or invalid pageCount");
    return;
  }

  // Calculate the slice of translated words for this page.
  const int totalTransWords = static_cast<int>(allTransWords.size());
  const int startWord = totalTransWords * pageIndex / pageCount;
  const int endWord = totalTransWords * (pageIndex + 1) / pageCount;

  // Copy the slice into transWordStorage.
  for (int i = startWord; i < endWord && transWordCount < MAX_WORDS; i++) {
    transWordStorage.push_back(std::move(allTransWords[i]));
    transWordPtrs[transWordCount] = transWordStorage.back().c_str();
    transWordCount++;
  }

  LOG_DBG("TIP", "Page %d/%d: %d orig words, %d trans words (slice %d-%d of %d), %d sentences",
          pageIndex, pageCount, origWordCount, transWordCount, startWord, endWord, totalTransWords, splits.count);

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
                            int yOffset, int viewportWidth, int viewportHeight, int pageIndex, int pageCount) {
  if (currentSentenceIndex < 0) return;

  LOG_DBG("TIP", "Rendering tooltip for sentence %d", currentSentenceIndex);

  preparePage(page, pageIndex, pageCount);

  if (currentSentenceIndex >= splits.count) {
    LOG_DBG("TIP", "Sentence %d >= splits.count %d, dismissing", currentSentenceIndex, splits.count);
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

  if (tooltipText[0] == '\0') {
    LOG_DBG("TIP", "No translation text for sentence %d (mapped.count=%d)", currentSentenceIndex, mapped.count);
    return;
  }
  LOG_DBG("TIP", "Drawing tooltip: '%.40s...'", tooltipText);

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
