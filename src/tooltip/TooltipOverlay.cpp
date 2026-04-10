#include "TooltipOverlay.h"

#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "fontIds.h"

// ── Button handling ───────────────────────────────────────────────────────────

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

// ── Expat-based translation extraction (two-pass: count then extract slice) ──

static const char* BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "div", nullptr};

static bool isBlockTag(const char* name) {
  for (int i = 0; BLOCK_TAGS[i]; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

struct ExpatState {
  int blockDepth = 0;
  bool inBlock = false;
  bool isTranslation = false;
  std::string currentText;

  // Mode: countOnly=true just counts, countOnly=false extracts paragraphs in [startPara, endPara)
  bool countOnly = true;
  int totalTransParas = 0;
  int startPara = 0;
  int endPara = 0;
  std::string collected;  // concatenated translations for the slice
};

static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* s = static_cast<ExpatState*>(ud);
  if (isBlockTag(name)) {
    s->inBlock = true;
    s->blockDepth = 1;
    s->isTranslation = false;
    s->currentText.clear();
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0 ||
            strcmp(atts[i], "data-translation") == 0) {
          s->isTranslation = true;
        }
      }
    }
  } else if (s->inBlock) {
    s->blockDepth++;
  }
}

static void XMLCALL onEnd(void* ud, const XML_Char* name) {
  auto* s = static_cast<ExpatState*>(ud);
  if (!s->inBlock) return;
  s->blockDepth--;
  if (s->blockDepth > 0) return;
  s->inBlock = false;

  if (!s->isTranslation || s->currentText.empty()) return;

  // Trim
  auto& t = s->currentText;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (t.empty()) return;

  if (s->countOnly) {
    s->totalTransParas++;
  } else {
    if (s->totalTransParas >= s->startPara && s->totalTransParas < s->endPara) {
      if (!s->collected.empty()) s->collected += ' ';
      s->collected += t;
    }
    s->totalTransParas++;
  }
}

static void XMLCALL onChar(void* ud, const XML_Char* data, int len) {
  auto* s = static_cast<ExpatState*>(ud);
  if (!s->inBlock) return;
  for (int i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && !s->currentText.empty() && s->currentText.back() == ' ') continue;
    s->currentText += c;
  }
}

static bool runExpat(const std::string& path, ExpatState& state) {
  FsFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f) return false;

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) { f.close(); return false; }

  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onChar);

  char buf[1024];
  const size_t fileSize = f.size();
  size_t totalRead = 0;

  while (totalRead < fileSize) {
    const size_t toRead = std::min(fileSize - totalRead, sizeof(buf));
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (n <= 0) break;
    totalRead += n;
    if (XML_Parse(parser, buf, n, (totalRead >= fileSize) ? 1 : 0) == XML_STATUS_ERROR) {
      LOG_ERR("TIP", "XML error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      break;
    }
  }

  XML_ParserFree(parser);
  f.close();
  return true;
}

// ── Page preparation ──────────────────────────────────────────────────────────

void TooltipOverlay::preparePage(const Page& page, int pageIndex, int pageCount) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  transWordCount = 0;
  transWordStorage.clear();

  // 1. Collect original words from page (CT_NO_RENDER = only originals in cache).
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    for (const auto& w : line->getTextBlock()->getWords()) {
      if (origWordCount < MAX_WORDS) origWordPtrs[origWordCount++] = w.c_str();
    }
  }
  splits = splitSentences(origWordPtrs, origWordCount);

  if (translatedHtmlPath.empty() || pageCount <= 0) return;

  // 2. Pass 1: count translated paragraphs in the HTML.
  ExpatState countState;
  countState.countOnly = true;
  if (!runExpat(translatedHtmlPath, countState) || countState.totalTransParas == 0) return;

  // 3. Pass 2: extract only the page's paragraph slice.
  const int total = countState.totalTransParas;
  ExpatState extractState;
  extractState.countOnly = false;
  extractState.startPara = total * pageIndex / pageCount;
  extractState.endPara = total * (pageIndex + 1) / pageCount;
  if (extractState.startPara >= extractState.endPara) extractState.endPara = extractState.startPara + 1;
  if (extractState.endPara > total) extractState.endPara = total;

  if (!runExpat(translatedHtmlPath, extractState) || extractState.collected.empty()) return;

  // 4. Split collected translation into words.
  const char* p = extractState.collected.c_str();
  while (*p && transWordCount < MAX_WORDS) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* ws = p;
    while (*p && *p != ' ') p++;
    transWordStorage.emplace_back(ws, p - ws);
    transWordPtrs[transWordCount] = transWordStorage.back().c_str();
    transWordCount++;
  }

  LOG_DBG("TIP", "Page %d/%d: %d orig, %d trans words, %d sentences (paras %d-%d of %d)",
          pageIndex, pageCount, origWordCount, transWordCount, splits.count,
          extractState.startPara, extractState.endPara, total);
}

// ── Sentence bounds and underline (all words are original in CT_NO_RENDER) ───

TooltipOverlay::SentenceBounds TooltipOverlay::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                   int fontId, int xOffset, int yOffset) const {
  SentenceBounds bounds = {0, 0, 0};
  int idx = 0;
  bool found = false;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    const auto& xpositions = line->getTextBlock()->getWordXpos();

    auto wIt = words.begin();
    auto xIt = xpositions.begin();
    for (; wIt != words.end() && xIt != xpositions.end(); ++wIt, ++xIt) {
      if (idx >= span.startWord && idx < span.endWord) {
        const int wx = *xIt + line->xPos + xOffset;
        const int wy = line->yPos + yOffset;
        if (!found) { bounds.firstLineY = wy; bounds.startX = wx; bounds.endX = wx; found = true; }
        if (wy == bounds.firstLineY) {
          if (wx < bounds.startX) bounds.startX = wx;
          if (wx > bounds.endX) bounds.endX = wx;
        }
      }
      idx++;
    }
  }
  return bounds;
}

void TooltipOverlay::drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span,
                                           int fontId, int xOffset, int yOffset) const {
  int idx = 0;
  int curY = -1, startX = 0, endX = 0;
  const int ulOff = renderer.getFontAscenderSize(fontId) + 2;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    const auto& styles = line->getTextBlock()->getWordStyles();
    const auto& xpos = line->getTextBlock()->getWordXpos();

    auto wIt = words.begin(); auto sIt = styles.begin(); auto xIt = xpos.begin();
    for (; wIt != words.end() && sIt != styles.end() && xIt != xpos.end(); ++wIt, ++sIt, ++xIt) {
      if (idx >= span.startWord && idx < span.endWord) {
        const int wx = *xIt + line->xPos + xOffset;
        const int wy = line->yPos + yOffset;
        const int ww = renderer.getTextWidth(fontId, wIt->c_str(),
                                              static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(*sIt) & 0x1F));
        if (wy != curY) {
          if (curY >= 0) renderer.drawLine(startX, curY + ulOff, endX, curY + ulOff, true);
          curY = wy; startX = wx; endX = wx + ww;
        } else {
          endX = wx + ww;
        }
      }
      idx++;
    }
  }
  if (curY >= 0) renderer.drawLine(startX, curY + ulOff, endX, curY + ulOff, true);
}

// ── Tooltip rendering ─────────────────────────────────────────────────────────

static int findLastLineY(const Page& page, const SentenceSpan& span, int yOffset) {
  int idx = 0, lastY = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    for (const auto& w : line->getTextBlock()->getWords()) {
      (void)w;
      if (idx >= span.startWord && idx < span.endWord) lastY = line->yPos + yOffset;
      idx++;
    }
  }
  return lastY;
}

void TooltipOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset,
                            int yOffset, int viewportWidth, int viewportHeight, int pageIndex, int pageCount) {
  if (currentSentenceIndex < 0) return;

  preparePage(page, pageIndex, pageCount);

  if (currentSentenceIndex >= splits.count) { currentSentenceIndex = -1; return; }

  const auto& span = splits.spans[currentSentenceIndex];

  // Map translations to sentences. Use heap buffer to avoid stack overflow with UTF-8.
  static char translationBuffer[2048];
  MappedSentenceResult mapped = mapSentenceTranslations(origWordPtrs, origWordCount, transWordPtrs, transWordCount,
                                                        splits, translationBuffer, sizeof(translationBuffer));

  const char* text = (currentSentenceIndex < mapped.count) ? mapped.sentences[currentSentenceIndex].translatedText : "";
  if (!text || text[0] == '\0') return;

  const int lh = renderer.getLineHeight(fontId);
  auto bounds = findSentenceBounds(page, span, fontId, xOffset, yOffset);
  if (bounds.firstLineY == 0 && bounds.startX == 0) return;

  const int lastY = findLastLineY(page, span, yOffset);

  constexpr int PAD = 6, RAD = 3, GAP = 4;
  const int maxW = viewportWidth - 2 * PAD;
  const int tw = renderer.getTextWidth(tooltipFontId, text);
  const int tlh = renderer.getLineHeight(tooltipFontId);

  int tipW, tipH, nLines = 1;
  if (tw <= maxW - 2 * PAD) {
    tipW = tw + 2 * PAD;
    tipH = tlh + 2 * PAD;
  } else {
    tipW = maxW;
    nLines = (tw + (tipW - 2 * PAD) - 1) / (tipW - 2 * PAD);
    const int maxL = (viewportHeight * 4 / 10) / tlh;
    if (nLines > maxL) nLines = maxL;
    tipH = nLines * tlh + 2 * PAD;
  }

  int tipX = xOffset + PAD;
  int tipY = (tipH + GAP <= bounds.firstLineY - yOffset)
                 ? bounds.firstLineY - GAP - tipH
                 : lastY + lh + GAP;
  if (tipY < yOffset + PAD) tipY = yOffset + PAD;
  if (tipY + tipH > yOffset + viewportHeight - PAD) tipY = yOffset + viewportHeight - PAD - tipH;

  renderer.fillRect(tipX - 1, tipY - 1, tipW + 2, tipH + 2, false);
  renderer.drawRoundedRect(tipX, tipY, tipW, tipH, 1, RAD, true);

  // Word-wrap and draw text
  const int avail = tipW - 2 * PAD;
  const int spW = renderer.getSpaceWidth(tooltipFontId);
  const char* p = text;
  int textY = tipY + PAD, drawn = 0;

  while (*p && drawn < nLines) {
    int lineW = 0;
    const char* ls = p;
    const char* le = p;
    while (*p) {
      const char* ws = p;
      while (*p && *p != ' ') p++;
      char wb[128];
      int wl = std::min((int)(p - ws), 127);
      memcpy(wb, ws, wl); wb[wl] = '\0';
      int ww = renderer.getTextWidth(tooltipFontId, wb);
      if (lineW > 0 && lineW + spW + ww > avail) { p = ws; break; }
      lineW += (lineW > 0 ? spW : 0) + ww;
      le = p;
      while (*p == ' ') p++;
    }
    int dl = (int)(le - ls);
    if (dl > 0) {
      char lb[512];
      int cl = std::min(dl, 511);
      memcpy(lb, ls, cl); lb[cl] = '\0';
      renderer.drawText(tooltipFontId, tipX + PAD, textY, lb);
      textY += tlh;
      drawn++;
    }
    if (p == ls) break;
  }

  drawSentenceUnderline(renderer, page, span, fontId, xOffset, yOffset);
}

// ── Font helper ───────────────────────────────────────────────────────────────

int getTooltipFontId() {
  if (SETTINGS.fontSize <= CrossPointSettings::SMALL) return SETTINGS.getReaderFontId();
  const uint8_t sz = SETTINGS.fontSize - 1;
  switch (SETTINGS.fontFamily) {
    case CrossPointSettings::BOOKERLY: default:
      switch (sz) {
        case CrossPointSettings::SMALL: return BOOKERLY_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return BOOKERLY_14_FONT_ID;
        case CrossPointSettings::LARGE: return BOOKERLY_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return BOOKERLY_18_FONT_ID;
      }
    case CrossPointSettings::EDSLAB:
      switch (sz) {
        case CrossPointSettings::SMALL: return EDSLAB_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return EDSLAB_14_FONT_ID;
        case CrossPointSettings::LARGE: return EDSLAB_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return EDSLAB_18_FONT_ID;
      }
    case CrossPointSettings::ALEGREYA:
      switch (sz) {
        case CrossPointSettings::SMALL: return ALEGREYA_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return ALEGREYA_14_FONT_ID;
        case CrossPointSettings::LARGE: return ALEGREYA_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return ALEGREYA_18_FONT_ID;
      }
    case CrossPointSettings::GPRO:
      switch (sz) {
        case CrossPointSettings::SMALL: return GPRO_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return GPRO_14_FONT_ID;
        case CrossPointSettings::LARGE: return GPRO_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return GPRO_18_FONT_ID;
      }
  }
}
