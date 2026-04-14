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
      ctx->entries->push_back({std::move(t)});
      ctx->hasLastOrig = false;
    }
  } else {
    // Original paragraph — reserve a slot. If no translation follows,
    // the slot stays from the previous push (translations are paired by index).
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

  // Log first few pairs for debugging
  for (int i = 0; i < std::min((int)pairs.size(), 5); i++) {
    LOG_DBG("MOD", "  pair[%d] translation: '%.60s%s'", i, pairs[i].translation.c_str(),
            pairs[i].translation.size() > 60 ? "..." : "");
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

  // Collect translations for the paragraph range on this page.
  for (int i = page.firstParagraphIdx; i <= page.lastParagraphIdx && i < (int)pairs.size(); i++) {
    LOG_DBG("MOD", "  idx %d: translation %s (len=%d)", i, pairs[i].translation.empty() ? "EMPTY" : "OK",
            (int)pairs[i].translation.size());
    if (!pairs[i].translation.empty()) {
      pageTranslations.push_back(pairs[i].translation);
    }
  }

  LOG_DBG("MOD", "Result: %d translations collected", (int)pageTranslations.size());
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
    if (totalContentHeight > 0 && scrollOffset < totalContentHeight) {
      const int overlap = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int screenScroll = cachedViewportHeight > overlap ? cachedViewportHeight - overlap : 200;
      scrollOffset += screenScroll;
      if (scrollOffset > totalContentHeight) scrollOffset = totalContentHeight;
    } else {
      active = false;
      scrollOffset = 0;
    }
    return true;
  }

  // Back button
  if (input.wasReleased(backBtn)) {
    if (!active) return false;
    if (scrollOffset > 0) {
      const int overlap = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int screenScroll = cachedViewportHeight > overlap ? cachedViewportHeight - overlap : 200;
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
  const int paraSpacing = lh / 2;

  cachedViewportHeight = viewportHeight;
  cachedLineHeight = lh;

  int contentH = 0;
  for (const auto& trans : pageTranslations) {
    contentH += measureParagraphHeight(renderer, modalFontId, trans.c_str(), maxTextW, lh, spW);
    contentH += paraSpacing;
  }
  if (!pageTranslations.empty()) contentH -= paraSpacing;
  totalContentHeight = std::max(0, contentH - viewportHeight);

  if (scrollOffset > totalContentHeight) scrollOffset = totalContentHeight;
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
    if (y + lh > clipTop && y < clipBottom) {
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
