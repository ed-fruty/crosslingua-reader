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
  if (path != translatedHtmlPath) {
    translatedHtmlPath = path;
    sectionParsed = false;
    chapterParagraphs.clear();
  }
}

void ModalOverlay::onSectionChanged() {
  active = false;
  scrollOffset = 0;
  totalContentHeight = 0;
  pagePrepared = false;
  sectionParsed = false;
  chapterParagraphs.clear();
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

void ModalOverlay::parseChapterHtml() {
  sectionParsed = true;
  chapterParagraphs.clear();

  if (translatedHtmlPath.empty()) return;

  FsFile file;
  if (!Storage.openFileForRead("MOD", translatedHtmlPath, file)) {
    LOG_ERR("MOD", "Cannot open %s", translatedHtmlPath.c_str());
    return;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    file.close();
    return;
  }

  ModalParseCtx ctx;
  ctx.entries = &chapterParagraphs;
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
  LOG_DBG("MOD", "Parsed %d paragraph pairs from %s", (int)chapterParagraphs.size(), translatedHtmlPath.c_str());
}

// ── Page preparation: match page paragraphs to chapter index ──────────────────

void ModalOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  pageTranslations.clear();
  totalContentHeight = 0;

  if (!sectionParsed) parseChapterHtml();
  if (chapterParagraphs.empty()) return;

  // Collect page paragraphs by grouping consecutive PageLines that share the same TextBlock.
  struct PagePara {
    std::vector<const char*> words;
  };
  std::vector<PagePara> pageParas;
  const TextBlock* lastBlock = nullptr;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const TextBlock* block = line->getTextBlock().get();
    if (block != lastBlock) {
      pageParas.emplace_back();
      lastBlock = block;
    }
    for (const auto& w : block->getWords()) {
      pageParas.back().words.push_back(w.c_str());
    }
  }

  // Pre-normalize chapter keys for matching.
  std::vector<std::string> normChapterKeys(chapterParagraphs.size());
  for (int i = 0; i < (int)chapterParagraphs.size(); i++) {
    auto& nk = normChapterKeys[i];
    for (char c : chapterParagraphs[i].key) {
      if (c == '.') continue;
      if (c == ' ' && (nk.empty() || nk.back() == ' ')) continue;
      nk += c;
    }
    while (!nk.empty() && nk.back() == ' ') nk.pop_back();
  }

  // Match each page paragraph against chapter index.
  int lastIdx = -1;
  for (const auto& pp : pageParas) {
    if (pp.words.empty()) continue;
    std::string pk = paragraphKey(pp.words.data(), (int)pp.words.size());
    if (pk.empty()) continue;

    std::string np;
    for (char c : pk) {
      if (c == '.') continue;
      if (c == ' ' && (np.empty() || np.back() == ' ')) continue;
      np += c;
    }
    while (!np.empty() && np.back() == ' ') np.pop_back();

    int foundIdx = -1;

    // Sequential hint: try next after last match.
    if (lastIdx >= 0 && lastIdx + 1 < (int)chapterParagraphs.size()) {
      int cl = (int)std::min(np.size(), normChapterKeys[lastIdx + 1].size());
      if (cl >= 3 && np.compare(0, cl, normChapterKeys[lastIdx + 1], 0, cl) == 0) foundIdx = lastIdx + 1;
    }

    // Full search fallback.
    if (foundIdx < 0) {
      int bestLen = 0;
      for (int j = 0; j < (int)chapterParagraphs.size(); j++) {
        int cl = (int)std::min(np.size(), normChapterKeys[j].size());
        if (cl < 3) continue;
        if (np.compare(0, cl, normChapterKeys[j], 0, cl) == 0 && cl > bestLen) {
          bestLen = cl;
          foundIdx = j;
        }
      }
    }

    if (foundIdx >= 0 && !chapterParagraphs[foundIdx].translation.empty()) {
      pageTranslations.push_back(chapterParagraphs[foundIdx].translation);
      lastIdx = foundIdx;
    }
  }

  LOG_DBG("MOD", "Page: %d paragraphs, %d matched translations", (int)pageParas.size(), (int)pageTranslations.size());
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
