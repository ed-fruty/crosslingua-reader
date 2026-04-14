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

void ModalOverlay::setTranslatedHtmlPath(const std::string& path) {
  translatedHtmlPath = path;
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

// ── Paragraph key (first 6 words, normalized) ────────────────────────────────

static constexpr int KEY_WORDS = 6;

std::string ModalOverlay::paragraphKey(const char* const* words, int count) {
  std::string key;
  int added = 0;
  for (int i = 0; i < count && added < KEY_WORDS; i++) {
    const char* w = words[i];
    if (!w || !w[0]) continue;
    if (strlen(w) == 1 && ispunct(static_cast<unsigned char>(w[0]))) continue;
    if (!key.empty()) key += ' ';
    key += w;
    added++;
  }
  for (auto& c : key) c = tolower(static_cast<unsigned char>(c));
  return key;
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
  std::vector<ModalOverlay::ParagraphEntry>* entries;
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
      std::vector<const char*> wordPtrs;
      std::string origCopy = ctx->lastOrigText;
      char* p = &origCopy[0];
      while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        wordPtrs.push_back(p);
        while (*p && *p != ' ') p++;
        if (*p) {
          *p = '\0';
          p++;
        }
      }
      std::string key = ModalOverlay::paragraphKey(wordPtrs.data(), (int)wordPtrs.size());
      ctx->entries->push_back({std::move(key), std::move(t)});
      ctx->hasLastOrig = false;
    }
  } else {
    ctx->lastOrigText = std::move(t);
    ctx->hasLastOrig = true;
  }
  ctx->currentText.clear();
}

static void XMLCALL modalOnText(void* ud, const XML_Char* s, int len) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (ctx->inBlock) ctx->currentText.append(s, len);
}

// Parse HTML and return (key, translation) pairs. Caller owns the result.
// Temporary — freed after page matching completes.
static std::vector<ModalOverlay::ParagraphEntry> parseChapterHtml(const std::string& htmlPath) {
  std::vector<ModalOverlay::ParagraphEntry> entries;

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

// ── Page preparation: match page paragraphs to chapter index ──────────────────

void ModalOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  pageTranslations.clear();
  totalContentHeight = 0;

  // Parse HTML into a temporary vector — freed at end of this function.
  // This avoids keeping all chapter translations in RAM permanently.
  auto chapterParagraphs = parseChapterHtml(translatedHtmlPath);
  if (chapterParagraphs.empty()) return;

  // Collect ALL words from the page into a flat array.
  // PageLines each have their own TextBlock (paragraphs are split into lines),
  // so we can't group by TextBlock pointer. Instead, we match chapter paragraph
  // keys against the flat word sequence.
  static constexpr int MAX_WORDS = 500;
  const char* pageWords[MAX_WORDS];
  int pageWordCount = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    for (const auto& w : line->getTextBlock()->getWords()) {
      if (pageWordCount < MAX_WORDS) pageWords[pageWordCount++] = w.c_str();
    }
  }

  if (pageWordCount == 0) return;

  // Build a key from the first words on the page to find our starting position
  // in the chapter. Then collect consecutive paragraph translations.
  std::string pageKey = paragraphKey(pageWords, pageWordCount);
  if (pageKey.empty()) return;

  // Normalize page key.
  std::string normPageKey;
  for (char c : pageKey) {
    if (c == '.') continue;
    if (c == ' ' && (normPageKey.empty() || normPageKey.back() == ' ')) continue;
    normPageKey += c;
  }
  while (!normPageKey.empty() && normPageKey.back() == ' ') normPageKey.pop_back();

  // Find the chapter paragraph that starts this page by matching the page key.
  int startIdx = -1;
  int bestLen = 0;
  for (int j = 0; j < (int)chapterParagraphs.size(); j++) {
    // Normalize chapter key for comparison.
    std::string nk;
    for (char c : chapterParagraphs[j].key) {
      if (c == '.') continue;
      if (c == ' ' && (nk.empty() || nk.back() == ' ')) continue;
      nk += c;
    }
    while (!nk.empty() && nk.back() == ' ') nk.pop_back();

    int cl = (int)std::min(normPageKey.size(), nk.size());
    if (cl >= 3 && normPageKey.compare(0, cl, nk, 0, cl) == 0 && cl > bestLen) {
      bestLen = cl;
      startIdx = j;
    }
  }

  if (startIdx < 0) {
    LOG_DBG("MOD", "Page: %d words, no matching paragraph found (key: '%.40s')", pageWordCount, normPageKey.c_str());
    return;
  }

  // Collect translations starting from the matched paragraph.
  // A page typically shows 5-15 paragraphs worth of content.
  // We take translations until we've likely passed the page boundary.
  // Heuristic: take up to pageWordCount * 2 words worth of translations
  // (translated text is often longer than original).
  int totalOrigWords = 0;
  for (int j = startIdx; j < (int)chapterParagraphs.size(); j++) {
    if (!chapterParagraphs[j].translation.empty()) {
      pageTranslations.push_back(chapterParagraphs[j].translation);
    }
    // Count words in this paragraph's key to estimate original length.
    int keyWords = 0;
    for (char c : chapterParagraphs[j].key) {
      if (c == ' ') keyWords++;
    }
    keyWords++;  // Last word has no trailing space.
    totalOrigWords += std::max(keyWords, KEY_WORDS);  // At least KEY_WORDS per paragraph.
    // Stop when we've collected enough paragraphs to cover the page.
    if (totalOrigWords > pageWordCount && (int)pageTranslations.size() >= 2) break;
  }

  LOG_DBG("MOD", "Page: %d words, matched from idx %d, %d translations", pageWordCount, startIdx,
          (int)pageTranslations.size());
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
      // Long press while IDLE: activate modal.
      if (input.getHeldTime() >= longPressMs) {
        active = true;
        scrollOffset = 0;
        return true;
      }
      // Short press while IDLE: don't consume — let normal page turn handle it.
      return false;
    }
    // Active: short or long press both scroll/close.
    if (totalContentHeight > 0 && scrollOffset < totalContentHeight) {
      const int overlap = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int screenScroll = cachedViewportHeight > overlap ? cachedViewportHeight - overlap : 200;
      scrollOffset += screenScroll;
      if (scrollOffset > totalContentHeight) scrollOffset = totalContentHeight;
    } else {
      // At bottom (or no content): close modal.
      active = false;
      scrollOffset = 0;
    }
    return true;
  }

  // Back button
  if (input.wasReleased(backBtn)) {
    if (!active) {
      // Not active: don't consume, let normal page turn handle it.
      return false;
    }
    // Active: scroll up or close.
    if (scrollOffset > 0) {
      const int overlap = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int screenScroll = cachedViewportHeight > overlap ? cachedViewportHeight - overlap : 200;
      scrollOffset -= screenScroll;
      if (scrollOffset < 0) scrollOffset = 0;
    } else {
      // At top: close modal.
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

  // Draw white overlay covering entire viewport.
  renderer.fillRect(xOffset, yOffset, viewportWidth, viewportHeight, false);

  const int lh = renderer.getLineHeight(modalFontId);
  const int spW = renderer.getSpaceWidth(modalFontId);
  constexpr int PAD = 10;
  const int maxTextW = viewportWidth - 2 * PAD;
  const int paraSpacing = lh / 2;

  // Cache for handleInput scroll computation.
  cachedViewportHeight = viewportHeight;
  cachedLineHeight = lh;

  // Measure total content height.
  int contentH = 0;
  for (const auto& trans : pageTranslations) {
    contentH += measureParagraphHeight(renderer, modalFontId, trans.c_str(), maxTextW, lh, spW);
    contentH += paraSpacing;
  }
  if (!pageTranslations.empty()) contentH -= paraSpacing;
  totalContentHeight = std::max(0, contentH - viewportHeight);

  // Clamp scrollOffset.
  if (scrollOffset > totalContentHeight) scrollOffset = totalContentHeight;
  if (scrollOffset < 0) scrollOffset = 0;

  // Draw pass.
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
