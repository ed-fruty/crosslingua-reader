#include "ModalOverlay.h"

#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "SentenceSplitter.h"
#include "fontIds.h"

// ── State management ─────────────────────────────────────────────────────────

void ModalOverlay::setTranslatedHtmlPath(const std::string& path) { translatedHtmlPath = path; }

void ModalOverlay::open() {
  active = true;
  scrollOffset = 0;
}

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

// ── HTML parsing: selective forward SAX over translated chapter HTML ──────────
//
// Compared to the fork (which parsed the full chapter, ~40 KB peak RAM), we:
//   1. Discard paragraph text for `idx < wantFirst - 1` (no allocation).
//   2. Capture paragraphs only when `idx` ∈ [wantFirst, wantLast].
//   3. Retain `origText` ONLY for boundary entries (idx == wantFirst or idx == wantLast)
//      — those are the only paragraphs that can be partially visible on the page
//      and therefore need sentence-skip lookup via `countSentencesBefore`.
//   4. Call XML_StopParser() once we have walked past wantLast, so the parser
//      bails out immediately without scanning the rest of the chapter.
// Peak heap stays under ~5 KB even for long chapters.

static const char* BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "div", nullptr};

static bool isBlockTag(const char* name) {
  for (int i = 0; BLOCK_TAGS[i]; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

struct ModalParseCtx {
  std::vector<ModalOverlay::ParagraphPair>* entries;
  int wantFirst;
  int wantLast;
  // Tracks the original (non-translation) paragraph index — matches the
  // outermost-block counter that the chapter parser uses (Site A, Task 8).
  // Pre-increment semantics: starts at -1 so the FIRST original block becomes idx 0.
  int paragraphCounter = -1;
  bool inBlock = false;
  bool isTranslation = false;
  int blockDepth = 0;
  std::string currentText;
  XML_Parser parser = nullptr;  // for XML_StopParser
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
        // Standardize on lang= / xml:lang= per spec §3.3. Fork's `data-translation`
        // fallback (an old attribute name from earlier fork iterations) is dropped.
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0) {
          ctx->isTranslation = true;
        }
      }
    }
  } else if (ctx->inBlock) {
    ctx->blockDepth++;
  }
}

static void XMLCALL modalOnEnd(void* ud, const XML_Char* name) {
  (void)name;
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (!ctx->inBlock) return;
  ctx->blockDepth--;
  if (ctx->blockDepth > 0) return;
  ctx->inBlock = false;

  auto& t = ctx->currentText;
  // Trim leading/trailing whitespace (matches fork behaviour).
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (t.empty()) {
    ctx->currentText.clear();
    return;
  }

  if (ctx->isTranslation) {
    // Translation block: attach to the most recent original entry IF that
    // entry corresponds to the same paragraphCounter (i.e. the original we
    // just saw is in our wanted range).
    if (!ctx->entries->empty() && ctx->entries->back().paragraphIdx == ctx->paragraphCounter) {
      ctx->entries->back().translation = std::move(t);
      ctx->entries->back().transSentenceCount = static_cast<int16_t>(countSentences(ctx->entries->back().translation));
    }
  } else {
    // Original block: bump the counter, then decide whether to keep it.
    const int idx = ++ctx->paragraphCounter;

    if (idx < ctx->wantFirst - 1) {
      // Too early — discard, no allocation. (We keep idx == wantFirst - 1 only
      // because the translation for it could not affect our page; safe to discard.)
      ctx->currentText.clear();
      return;
    }
    if (idx > ctx->wantLast) {
      // Past the page — stop the parser. expat will return XML_STATUS_ERROR
      // with error code XML_ERROR_ABORTED, which we treat as success below.
      if (ctx->parser) XML_StopParser(ctx->parser, XML_FALSE);
      ctx->currentText.clear();
      return;
    }

    const bool needsOrigText = (idx == ctx->wantFirst || idx == ctx->wantLast);
    ModalOverlay::ParagraphPair entry;
    entry.paragraphIdx = static_cast<int16_t>(idx);
    if (needsOrigText) {
      // Boundary paragraph — keep origText for sentence-skip lookup
      // (countSentencesBefore). Sentence count is computed before moving.
      entry.origSentenceCount = static_cast<int16_t>(countSentences(t));
      entry.origText = std::move(t);
    } else {
      // Middle paragraph — count sentences but drop the text to save RAM.
      entry.origSentenceCount = static_cast<int16_t>(countSentences(t));
      entry.origText.clear();
    }
    ctx->entries->push_back(std::move(entry));
  }
  ctx->currentText.clear();
}

static void XMLCALL modalOnText(void* ud, const XML_Char* s, int len) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (ctx->inBlock) ctx->currentText.append(s, len);
}

static std::vector<ModalOverlay::ParagraphPair> parseChapterHtml(const std::string& htmlPath, int wantFirst,
                                                                 int wantLast) {
  std::vector<ModalOverlay::ParagraphPair> entries;

  if (htmlPath.empty()) return entries;

  HalFile file;
  if (!Storage.openFileForRead("MOD", htmlPath, file)) {
    LOG_ERR("MOD", "Cannot open %s", htmlPath.c_str());
    return entries;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("MOD", "Failed to create expat parser");
    return entries;
  }

  ModalParseCtx ctx;
  ctx.entries = &entries;
  ctx.wantFirst = wantFirst;
  ctx.wantLast = wantLast;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, modalOnStart, modalOnEnd);
  XML_SetCharacterDataHandler(parser, modalOnText);

  char buf[1024];
  bool done = false;
  bool stopped = false;
  while (!done && !stopped) {
    int len = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    done = (len < (int)sizeof(buf));
    if (XML_Parse(parser, buf, len, done) == XML_STATUS_ERROR) {
      // XML_StopParser() callbacks cause expat to return ERROR with code
      // XML_ERROR_ABORTED. That's the *success* path for us — bail cleanly.
      const auto code = XML_GetErrorCode(parser);
      if (code == XML_ERROR_ABORTED) {
        stopped = true;
      } else {
        LOG_ERR("MOD", "XML parse error at line %lu", XML_GetCurrentLineNumber(parser));
      }
      break;
    }
  }
  XML_ParserFree(parser);
  LOG_DBG("MOD", "Parsed %d pair(s) in [%d..%d] from %s%s", (int)entries.size(), wantFirst, wantLast, htmlPath.c_str(),
          stopped ? " (early-stop)" : "");
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

  // Parse HTML for ONLY the paragraphs in [first..last]. The selective SAX parse
  // caps peak RAM at ~5 KB (vs ~40 KB for fork's full-chapter parse).
  auto pairs = parseChapterHtml(translatedHtmlPath, page.firstParagraphIdx, page.lastParagraphIdx);
  if (pairs.empty()) {
    LOG_DBG("MOD", "No pairs parsed from HTML");
    return;
  }

  LOG_DBG("MOD", "Parsed %d pair(s), page wants indices [%d..%d]", (int)pairs.size(), page.firstParagraphIdx,
          page.lastParagraphIdx);

  // Log pairs for debugging
  for (size_t i = 0; i < pairs.size(); i++) {
    LOG_DBG("MOD", "  pair[%d]: idx=%d origS=%d transS=%d trans=%s(len=%d)", (int)i, pairs[i].paragraphIdx,
            pairs[i].origSentenceCount, pairs[i].transSentenceCount, pairs[i].translation.empty() ? "EMPTY" : "OK",
            (int)pairs[i].translation.size());
  }

  // Log page text content for debugging
  std::string firstWords;
  int wc = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (!firstWords.empty()) firstWords += ' ';
      firstWords += block->wordText(i);
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
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (!pageParas.back().text.empty()) pageParas.back().text += ' ';
      pageParas.back().text += block->wordText(i);
    }
  }

  const int first = page.firstParagraphIdx;
  const int last = page.lastParagraphIdx;

  // Sparse iteration: each pair knows its own paragraphIdx.
  for (const auto& pair : pairs) {
    if (pair.translation.empty()) continue;
    const int i = pair.paragraphIdx;
    if (i < first || i > last) continue;  // defensive — parser already filtered

    // Find this paragraph's visible text on the page.
    int visibleSentences = 0;
    std::string visibleText;
    for (const auto& pp : pageParas) {
      if (pp.idx == i) {
        visibleText = pp.text;
        visibleSentences = countSentences(pp.text);
        break;
      }
    }

    const int origTotal = pair.origSentenceCount;
    const bool isFirst = (i == first);
    const bool isLast = (i == last);

    if (visibleSentences > 0 && visibleSentences < origTotal) {
      if (isFirst && !isLast) {
        // Tail — only START is broken (+1), end is full.
        const int show = std::min(visibleSentences + 1, (int)pair.transSentenceCount);
        const int skipSentences = countSentencesBefore(pair.origText, visibleText);
        const int fromSentence = skipSentences;
        const int toSentence = std::min(fromSentence + show, (int)pair.transSentenceCount);
        std::string trimmed = trimToSentences(pair.translation, toSentence);
        if (fromSentence > 0) trimmed = trimToLastSentences(trimmed, toSentence - fromSentence);
        pageTranslations.push_back(std::move(trimmed));
        LOG_DBG("MOD", "  idx %d: TAIL, skip=%d show=%d/%d (visible=%d)", i, skipSentences, show, origTotal,
                visibleSentences);
      } else if (isLast && !isFirst) {
        // Head — only END is broken (+1), start is full.
        const int show = std::min(visibleSentences + 1, (int)pair.transSentenceCount);
        pageTranslations.push_back(trimToSentences(pair.translation, show));
        LOG_DBG("MOD", "  idx %d: HEAD, %d/%d sentences (visible=%d)", i, show, origTotal, visibleSentences);
      } else if (isFirst && isLast) {
        // PARTIAL — both edges broken (+2).
        const int show = std::min(visibleSentences + 2, (int)pair.transSentenceCount);
        const int skipSentences = countSentencesBefore(pair.origText, visibleText);
        const int fromSentence = skipSentences;
        const int toSentence = std::min(fromSentence + show, (int)pair.transSentenceCount);
        std::string trimmed = trimToSentences(pair.translation, toSentence);
        if (fromSentence > 0) trimmed = trimToLastSentences(trimmed, toSentence - fromSentence);
        pageTranslations.push_back(std::move(trimmed));
        LOG_DBG("MOD", "  idx %d: PARTIAL, skip=%d show=%d/%d (visible=%d)", i, skipSentences, show, origTotal,
                visibleSentences);
      }
    } else {
      // Full paragraph on page.
      pageTranslations.push_back(pair.translation);
      LOG_DBG("MOD", "  idx %d: FULL, %d sentences", i, origTotal);
    }
  }

  LOG_DBG("MOD", "Result: %d translation(s)", (int)pageTranslations.size());
}

// ── Button handling ──────────────────────────────────────────────────────────
//
// Upstream dropped the fork's tooltipButtons setting — overlay scroll/close
// uses side buttons unconditionally. The "longpress-opens-overlay" branch from
// the fork's handleInput is gone too: that gesture is now detected by
// EpubReaderActivity (Task 23), which calls ModalOverlay::open() externally.

bool ModalOverlay::handleInput(MappedInputManager& input) {
  const auto nextBtn = MappedInputManager::Button::PageForward;
  const auto backBtn = MappedInputManager::Button::PageBack;

  // Next button: scroll down one screenful, or close if we're at the end.
  if (input.wasReleased(nextBtn)) {
    if (!active) return false;
    const int lh = cachedLineHeight > 0 ? cachedLineHeight : 20;
    const int vpH = cachedViewportHeight > 0 ? cachedViewportHeight : 700;
    const int screenScroll = (vpH / lh) * lh;
    const int maxScroll = std::max(0, (int)totalContentHeight - vpH);
    LOG_DBG("MOD", "SCROLL NEXT: offset %d -> %d (step=%d, vpH=%d, lh=%d, totalH=%d, maxScroll=%d)", scrollOffset,
            scrollOffset + screenScroll, screenScroll, vpH, lh, totalContentHeight, maxScroll);
    if (scrollOffset + vpH < totalContentHeight) {
      // More content below — scroll down.
      scrollOffset += screenScroll;
    } else {
      // Already showing last content — close modal.
      active = false;
      scrollOffset = 0;
    }
    return true;
  }

  // Back button: scroll up, or close if already at the top.
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
  (void)fontId;
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

  cachedViewportHeight = static_cast<int16_t>(viewportHeight);
  cachedLineHeight = static_cast<int16_t>(lh);

  int contentH = 0;
  for (const auto& trans : pageTranslations) {
    contentH += measureParagraphHeight(renderer, modalFontId, trans.c_str(), maxTextW, lh, spW);
    contentH += paraSpacing;
  }
  if (!pageTranslations.empty()) contentH -= paraSpacing;

  // totalContentHeight = full content height (NOT minus viewport).
  // scrollOffset steps through it in viewport-sized chunks.
  totalContentHeight = static_cast<int16_t>(contentH);

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
//
// Fork's getModalFontId() returned getTooltipFontId() (which lives in the fork's
// tooltip module). We're not porting Tooltip, so we resolve to upstream's
// mid-size UI font directly. UI_12_FONT_ID is the most readable mid-size UI
// font available in upstream (see src/fontIds.h).

int getModalFontId() { return UI_12_FONT_ID; }
