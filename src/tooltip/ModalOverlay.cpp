#include "ModalOverlay.h"

#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "TooltipOverlay.h"
#include "fontIds.h"

// ── State management ─────────────────────────────────────────────────────────

void ModalOverlay::setTranslatedHtmlPath(const std::string& path) { translatedHtmlPath = path; }

void ModalOverlay::onSectionChanged() {
  active = false;
  scrollOffset = 0;
  totalContentHeight = 0;
  pagePrepared = false;
  pageTranslations.clear();
}

void ModalOverlay::onPageChanged() {
  active = false;
  scrollOffset = 0;
  totalContentHeight = 0;
  pagePrepared = false;
  pageTranslations.clear();
}

// ── Sentence helpers ─────────────────────────────────────────────────────────

// Count sentences in text. A sentence ends with . ! ? followed by space/end.
static int countSentences(const std::string& text) {
  if (text.empty()) return 0;
  int count = 0;
  for (int i = 0; i < (int)text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      // Skip ellipsis (... or . . .)
      if (c == '.' && i + 1 < (int)text.size() && text[i + 1] == '.') continue;
      // Check followed by space, end, or closing quote
      int next = i + 1;
      // Skip closing quotes/brackets
      while (next < (int)text.size() && (text[next] == '"' || text[next] == '\'' || text[next] == ')' ||
                                          text[next] == ']' || (uint8_t)text[next] > 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        count++;
      }
    }
  }
  return std::max(1, count);  // At least 1 sentence per non-empty text
}

// Trim text to first N sentences. Returns the trimmed text.
static std::string trimToSentences(const std::string& text, int maxSentences) {
  if (maxSentences <= 0) return "";
  int count = 0;
  for (int i = 0; i < (int)text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      if (c == '.' && i + 1 < (int)text.size() && text[i + 1] == '.') continue;
      int next = i + 1;
      while (next < (int)text.size() && (text[next] == '"' || text[next] == '\'' || text[next] == ')' ||
                                          text[next] == ']' || (uint8_t)text[next] > 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        count++;
        if (count >= maxSentences) {
          return text.substr(0, next);
        }
      }
    }
  }
  return text;  // Fewer sentences than max — return all
}

// Trim text to LAST N sentences.
static std::string trimToLastSentences(const std::string& text, int maxSentences) {
  if (maxSentences <= 0) return "";
  // Find all sentence end positions
  std::vector<int> ends;
  for (int i = 0; i < (int)text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      if (c == '.' && i + 1 < (int)text.size() && text[i + 1] == '.') continue;
      int next = i + 1;
      while (next < (int)text.size() && (text[next] == '"' || text[next] == '\'' || text[next] == ')' ||
                                          text[next] == ']' || (uint8_t)text[next] > 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        ends.push_back(next);
      }
    }
  }
  if ((int)ends.size() <= maxSentences) return text;
  int startFrom = ends[ends.size() - maxSentences];
  // Skip leading space
  while (startFrom < (int)text.size() && text[startFrom] == ' ') startFrom++;
  return text.substr(startFrom);
}

// Count sentences in origText that end BEFORE the position where visibleText starts.
// Used to determine how many sentences to skip in translation for tail paragraphs.
static int countSentencesBefore(const std::string& origText, const std::string& visibleStart) {
  if (visibleStart.empty() || origText.empty()) return 0;

  // Normalize visibleStart: collapse whitespace, take first 40 chars
  std::string needle;
  bool lastSp = true;
  for (char c : visibleStart) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!lastSp) needle += ' ';
      lastSp = true;
    } else {
      needle += c;
      lastSp = false;
    }
    if ((int)needle.size() >= 40) break;
  }
  while (!needle.empty() && needle.back() == ' ') needle.pop_back();
  if (needle.size() < 3) return 0;

  // Normalize origText too
  std::string normOrig;
  lastSp = true;
  for (char c : origText) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!lastSp) normOrig += ' ';
      lastSp = true;
    } else {
      normOrig += c;
      lastSp = false;
    }
  }

  // Find where the visible text starts in the original
  auto pos = normOrig.find(needle);
  if (pos == std::string::npos) return 0;

  // Count sentences that end before this position
  return countSentences(normOrig.substr(0, pos));
}

// ── HTML parsing: extract (original, translation) paragraph pairs ─────────────

static const char* BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "div", nullptr};

static bool isBlockTag(const char* name) {
  for (int i = 0; BLOCK_TAGS[i]; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

struct ModalParseCtx {
  std::vector<ModalOverlay::ParagraphPair>* entries;
  int blockDepth = 0;
  bool inBlock = false;
  bool isTranslation = false;
  std::string currentText;
  std::string lastOrigText;
  bool hasLastOrig = false;
};

static void XMLCALL modalOnStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (isBlockTag(name)) {
    ctx->inBlock = true;
    ctx->blockDepth = 1;
    ctx->isTranslation = false;
    ctx->currentText.clear();
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0 ||
            strcmp(atts[i], "data-translation") == 0) {
          ctx->isTranslation = true;
        }
      }
    }
  } else if (ctx->inBlock) {
    ctx->blockDepth++;
  }
}

static void XMLCALL modalOnEnd(void* ud, const XML_Char* name) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (!ctx->inBlock) return;
  ctx->blockDepth--;
  if (ctx->blockDepth > 0) return;
  ctx->inBlock = false;

  auto& t = ctx->currentText;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (t.empty()) return;

  if (ctx->isTranslation) {
    if (ctx->hasLastOrig) {
      ctx->entries->back().translation = std::move(t);
      ctx->entries->back().transSentenceCount = countSentences(ctx->entries->back().translation);
      ctx->hasLastOrig = false;
    }
  } else {
    // Every original block gets a slot — even if no translation follows.
    // This keeps indices aligned with ChapterHtmlSlimParser's paragraphCounter.
    // Store full original text temporarily for partial paragraph matching.
    int origSentences = countSentences(t);
    ctx->entries->push_back({std::move(t), "", (int16_t)origSentences, 0});
    ctx->hasLastOrig = true;
  }
  ctx->currentText.clear();
}

static void XMLCALL modalOnText(void* ud, const XML_Char* s, int len) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (ctx->inBlock) ctx->currentText.append(s, len);
}

static std::vector<ModalOverlay::ParagraphPair> parseChapterHtml(const std::string& htmlPath) {
  std::vector<ModalOverlay::ParagraphPair> entries;

  if (htmlPath.empty()) return entries;

  FsFile file;
  if (!Storage.openFileForRead("MOD", htmlPath, file)) {
    LOG_ERR("MOD", "Cannot open %s", htmlPath.c_str());
    return entries;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    file.close();
    return entries;
  }

  ModalParseCtx ctx;
  ctx.entries = &entries;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, modalOnStart, modalOnEnd);
  XML_SetCharacterDataHandler(parser, modalOnText);

  char buf[1024];
  bool done = false;
  while (!done) {
    int len = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    done = (len < (int)sizeof(buf));
    if (XML_Parse(parser, buf, len, done) == XML_STATUS_ERROR) {
      LOG_ERR("MOD", "XML parse error at line %lu", XML_GetCurrentLineNumber(parser));
      break;
    }
  }
  XML_ParserFree(parser);
  file.close();
  LOG_DBG("MOD", "Parsed %d paragraph pairs from %s", (int)entries.size(), htmlPath.c_str());
  return entries;
}

// ── Page preparation: use paragraph indices stored during page building ────────

void ModalOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  pageTranslations.clear();
  totalContentHeight = 0;

  // The page knows exactly which paragraph indices it contains (set by the parser).
  LOG_DBG("MOD", "Page paragraph indices: first=%d last=%d", page.firstParagraphIdx, page.lastParagraphIdx);

  if (page.firstParagraphIdx < 0 || page.lastParagraphIdx < 0) {
    LOG_DBG("MOD", "Page has no paragraph indices (old cache?) — clear cache and retry");
    return;
  }

  LOG_DBG("MOD", "HTML path: %s", translatedHtmlPath.c_str());

  // Parse HTML to get translation entries. Temporary — freed after this function.
  auto pairs = parseChapterHtml(translatedHtmlPath);
  if (pairs.empty()) {
    LOG_DBG("MOD", "No pairs parsed from HTML");
    return;
  }

  LOG_DBG("MOD", "Parsed %d pairs, page wants indices [%d..%d]", (int)pairs.size(), page.firstParagraphIdx,
          page.lastParagraphIdx);

  // Log pairs around the page range for debugging
  int logStart = std::max(0, (int)page.firstParagraphIdx - 2);
  int logEnd = std::min((int)pairs.size(), (int)page.lastParagraphIdx + 3);
  for (int i = logStart; i < logEnd; i++) {
    LOG_DBG("MOD", "  pair[%d] origS=%d transS=%d trans=%s(len=%d)", i, pairs[i].origSentenceCount,
            pairs[i].transSentenceCount, pairs[i].translation.empty() ? "EMPTY" : "OK",
            (int)pairs[i].translation.size());
  }

  // Log page text content for debugging
  std::string firstWords;
  int wc = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    for (const auto& w : line->getTextBlock()->getWords()) {
      if (!firstWords.empty()) firstWords += ' ';
      firstWords += w;
      if (++wc >= 10) break;
    }
    if (wc >= 10) break;
  }
  LOG_DBG("MOD", "Page first words: '%.80s'", firstWords.c_str());

  // Build per-paragraph text from page lines using paragraphIdx on each PageLine.
  // This lets us count sentences per paragraph accurately.
  struct ParaText {
    int16_t idx;
    std::string text;
  };
  std::vector<ParaText> pageParas;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    int16_t pIdx = line->paragraphIdx;
    if (pIdx < 0) continue;
    if (pageParas.empty() || pageParas.back().idx != pIdx) {
      pageParas.push_back({pIdx, ""});
    }
    for (const auto& w : line->getTextBlock()->getWords()) {
      if (!pageParas.back().text.empty()) pageParas.back().text += ' ';
      pageParas.back().text += w;
    }
  }

  int first = page.firstParagraphIdx;
  int last = page.lastParagraphIdx;

  for (int i = first; i <= last && i < (int)pairs.size(); i++) {
    if (pairs[i].translation.empty()) continue;

    // Find this paragraph's visible text on the page.
    int visibleSentences = 0;
    for (const auto& pp : pageParas) {
      if (pp.idx == i) {
        visibleSentences = countSentences(pp.text);
        break;
      }
    }

    int origTotal = pairs[i].origSentenceCount;
    bool isFirst = (i == first);
    bool isLast = (i == last);

    // Find visible text for this paragraph on page.
    std::string visibleText;
    for (const auto& pp : pageParas) {
      if (pp.idx == i) { visibleText = pp.text; break; }
    }

    if (visibleSentences > 0 && visibleSentences < origTotal) {
      // +1 for the incomplete sentence fragment at the boundary.
      // +1 for the incomplete sentence fragment at the end boundary.
      int showSentences = std::min(visibleSentences + 1, (int)pairs[i].transSentenceCount);

      if (isFirst && !isLast) {
        // Tail — continuation from previous page.
        // Find how many sentences are BEFORE the visible portion, skip them.
        int skipSentences = countSentencesBefore(pairs[i].origText, visibleText);
        // -2: one because the broken sentence at the boundary isn't counted,
        // and one more to include the sentence that ends right at the page start.
        int fromSentence = std::max(0, skipSentences - 2);
        int toSentence = std::min(fromSentence + showSentences, (int)pairs[i].transSentenceCount);
        // Use trimToSentences to get first `toSentence`, then trimToLastSentences to drop first `fromSentence`.
        std::string trimmed = trimToSentences(pairs[i].translation, toSentence);
        if (fromSentence > 0) trimmed = trimToLastSentences(trimmed, toSentence - fromSentence);
        pageTranslations.push_back(std::move(trimmed));
        LOG_DBG("MOD", "  idx %d: TAIL, skip=%d show=%d/%d (visible=%d)", i, skipSentences, showSentences, origTotal,
                visibleSentences);
      } else if (isLast && !isFirst) {
        // Head — continues on next page. Show first N+1 sentences.
        pageTranslations.push_back(trimToSentences(pairs[i].translation, showSentences));
        LOG_DBG("MOD", "  idx %d: HEAD, %d/%d sentences (visible=%d)", i, showSentences, origTotal, visibleSentences);
      } else if (isFirst && isLast) {
        // Single partial paragraph (middle of a large paragraph).
        // Find where visible text starts, skip sentences before it, show visible count.
        int skipSentences = countSentencesBefore(pairs[i].origText, visibleText);
        int fromSentence = skipSentences;
        int toSentence = std::min(fromSentence + showSentences, (int)pairs[i].transSentenceCount);
        std::string trimmed = trimToSentences(pairs[i].translation, toSentence);
        if (fromSentence > 0) trimmed = trimToLastSentences(trimmed, toSentence - fromSentence);
        pageTranslations.push_back(std::move(trimmed));
        LOG_DBG("MOD", "  idx %d: PARTIAL, skip=%d show=%d/%d (visible=%d)", i, skipSentences, showSentences,
                origTotal, visibleSentences);
      }
    } else {
      // Full paragraph on page.
      pageTranslations.push_back(pairs[i].translation);
      LOG_DBG("MOD", "  idx %d: FULL, %d sentences", i, origTotal);
    }
  }

  LOG_DBG("MOD", "Result: %d translations", (int)pageTranslations.size());
}

// ── Button handling ──────────────────────────────────────────────────────────

bool ModalOverlay::handleInput(MappedInputManager& input) {
  const bool useFrontButtons = (SETTINGS.tooltipButtons == 0);
  const auto nextBtn =
      useFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
  const auto backBtn =
      useFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;
  constexpr unsigned long longPressMs = 700;

  // Next button
  if (input.wasReleased(nextBtn)) {
    if (!active) {
      if (input.getHeldTime() >= longPressMs) {
        active = true;
        scrollOffset = 0;
        return true;
      }
      return false;
    }
    {
      const int lh = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int vpH = cachedViewportHeight > 0 ? cachedViewportHeight : 700;
      const int screenScroll = (vpH / lh) * lh;
      const int maxScroll = std::max(0, (int)totalContentHeight - vpH);
      LOG_DBG("MOD", "SCROLL NEXT: offset %d -> %d (step=%d, vpH=%d, lh=%d, totalH=%d, maxScroll=%d)",
              scrollOffset, scrollOffset + screenScroll, screenScroll, vpH, lh, totalContentHeight, maxScroll);
      if (scrollOffset + vpH < totalContentHeight) {
        // More content below — scroll down.
        scrollOffset += screenScroll;
      } else {
        // Already showing last content — close modal.
        active = false;
        scrollOffset = 0;
      }
    }
    return true;
  }

  // Back button
  if (input.wasReleased(backBtn)) {
    if (!active) return false;
    if (scrollOffset > 0) {
      // Scroll by exactly one screenful — no overlap, next press = next content.
      const int lh = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int screenScroll = (cachedViewportHeight / lh) * lh;
      scrollOffset -= screenScroll;
      if (scrollOffset < 0) scrollOffset = 0;
    } else {
      active = false;
      scrollOffset = 0;
    }
    return true;
  }

  // ESC/Back button: dismiss modal if active.
  if (input.wasReleased(MappedInputManager::Button::Back)) {
    if (active) {
      active = false;
      scrollOffset = 0;
      return true;
    }
    return false;
  }

  return false;
}

// ── Rendering ────────────────────────────────────────────────────────────────

void ModalOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int modalFontId, int xOffset,
                          int yOffset, int viewportWidth, int viewportHeight) {
  if (!active) return;

  preparePage(page);

  if (pageTranslations.empty()) {
    active = false;
    return;
  }

  renderer.fillRect(xOffset, yOffset, viewportWidth, viewportHeight, false);

  const int lh = renderer.getLineHeight(modalFontId);
  const int spW = renderer.getSpaceWidth(modalFontId);
  constexpr int PAD = 10;
  const int maxTextW = viewportWidth - 2 * PAD;
  const int paraSpacing = lh;  // Full line height so scroll stays aligned

  cachedViewportHeight = viewportHeight;
  cachedLineHeight = lh;

  int contentH = 0;
  for (const auto& trans : pageTranslations) {
    contentH += measureParagraphHeight(renderer, modalFontId, trans.c_str(), maxTextW, lh, spW);
    contentH += paraSpacing;
  }
  if (!pageTranslations.empty()) contentH -= paraSpacing;

  // totalContentHeight = full content height (NOT minus viewport).
  // scrollOffset steps through it in viewport-sized chunks.
  totalContentHeight = contentH;

  if (scrollOffset < 0) scrollOffset = 0;

  int curY = yOffset - scrollOffset;
  for (const auto& trans : pageTranslations) {
    curY = drawParagraph(renderer, modalFontId, trans.c_str(), xOffset + PAD, curY, maxTextW, lh, spW, yOffset,
                         yOffset + viewportHeight);
    curY += paraSpacing;
  }
}

int ModalOverlay::measureParagraphHeight(GfxRenderer& renderer, int fontId, const char* text, int maxW, int lh,
                                         int spW) {
  int lines = 0;
  const char* p = text;
  while (*p) {
    int lineW = 0;
    const char* ls = p;
    while (*p) {
      const char* ws = p;
      while (*p && *p != ' ') p++;
      char wb[128];
      int wl = std::min((int)(p - ws), 127);
      memcpy(wb, ws, wl);
      wb[wl] = '\0';
      int ww = renderer.getTextWidth(fontId, wb);
      if (lineW > 0 && lineW + spW + ww > maxW) {
        p = ws;
        break;
      }
      lineW += (lineW > 0 ? spW : 0) + ww;
      while (*p == ' ') p++;
    }
    lines++;
    if (p == ls) break;
  }
  return lines * lh;
}

int ModalOverlay::drawParagraph(GfxRenderer& renderer, int fontId, const char* text, int x, int y, int maxW, int lh,
                                int spW, int clipTop, int clipBottom) {
  const char* p = text;
  while (*p) {
    int lineW = 0;
    const char* lineStart = p;
    const char* lineEnd = p;
    while (*p) {
      const char* ws = p;
      while (*p && *p != ' ') p++;
      char wb[128];
      int wl = std::min((int)(p - ws), 127);
      memcpy(wb, ws, wl);
      wb[wl] = '\0';
      int ww = renderer.getTextWidth(fontId, wb);
      if (lineW > 0 && lineW + spW + ww > maxW) {
        p = ws;
        break;
      }
      lineW += (lineW > 0 ? spW : 0) + ww;
      lineEnd = p;
      while (*p == ' ') p++;
    }
    if (y + lh > clipTop && y + lh <= clipBottom) {
      int dl = (int)(lineEnd - lineStart);
      if (dl > 0) {
        char lb[512];
        int cl = std::min(dl, 511);
        memcpy(lb, lineStart, cl);
        lb[cl] = '\0';
        renderer.drawText(fontId, x, y, lb);
      }
    }
    y += lh;
    if (p == lineStart) break;
  }
  return y;
}

// ── Font helper ──────────────────────────────────────────────────────────────

int getModalFontId() { return getTooltipFontId(); }
