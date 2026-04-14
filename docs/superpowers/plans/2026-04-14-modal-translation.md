# Modal Translation Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CT_MODAL translation mode that shows a full-page white overlay with all translated paragraphs for the current page, scrollable with short presses.

**Architecture:** New `ModalOverlay` class in `src/tooltip/` handles input, HTML parsing, paragraph matching, and rendering. It follows the same integration pattern as `TooltipOverlay` — created/destroyed based on settings, input dispatched before page-turn buttons, rendered after page content. Reuses `tooltipButtons` and `tooltipBehavior` settings.

**Tech Stack:** C++20, ESP32-C3 Arduino, PlatformIO, expat XML parser, GfxRenderer

**Spec:** `docs/superpowers/specs/2026-04-14-modal-translation-design.md`

**Branch:** Create `feat/modal-translation` off current `feat/book-translation`

---

### Task 1: Create branch and add CT_MODAL enum + i18n string

**Files:**
- Modify: `src/CrossPointSettings.h:186`
- Modify: `src/SettingsList.h:76-80`
- Modify: `lib/I18n/translations/english.yaml` (and 7 other language files)
- Modify: `lib/I18n/I18nKeys.h` (auto-generated)

- [ ] **Step 1: Create the feature branch**

```bash
git checkout -b feat/modal-translation
```

- [ ] **Step 2: Add CT_MODAL = 7 to the COLOR_TEXT_STYLE enum**

In `src/CrossPointSettings.h:186`, change:

```cpp
enum COLOR_TEXT_STYLE { CT_NORMAL = 0, CT_DARK = 1, CT_LIGHT = 2, CT_NO_RENDER = 3, CT_INVERT = 4, CT_SIDE_BY_SIDE = 5, CT_TOOLTIP = 6, COLOR_TEXT_STYLE_COUNT };
```

to:

```cpp
enum COLOR_TEXT_STYLE { CT_NORMAL = 0, CT_DARK = 1, CT_LIGHT = 2, CT_NO_RENDER = 3, CT_INVERT = 4, CT_SIDE_BY_SIDE = 5, CT_TOOLTIP = 6, CT_MODAL = 7, COLOR_TEXT_STYLE_COUNT };
```

- [ ] **Step 3: Add STR_MODAL i18n string to all 8 language files**

In `lib/I18n/translations/english.yaml`, add after the `STR_TOOLTIP` line:

```yaml
STR_MODAL: "Modal"
```

Add equivalent entries in all other 7 YAML files (`czech.yaml`, `french.yaml`, `german.yaml`, `portuguese.yaml`, `russia.yaml`, `spanish.yaml`, `swedish.yaml`). Translations:
- czech: `"Modální"`
- french: `"Modale"`
- german: `"Modal"`
- portuguese: `"Modal"`
- russian: `"Модальное"`
- spanish: `"Modal"`
- swedish: `"Modal"`

- [ ] **Step 4: Run i18n code generation**

```bash
python3 scripts/gen_i18n.py
```

This regenerates `I18nKeys.h`, `I18nStrings.h`, and `I18nStrings.cpp` with the new `STR_MODAL` enum.

- [ ] **Step 5: Add CT_MODAL to the translation mode setting in SettingsList.h**

In `src/SettingsList.h:76-80`, the translation mode enum setting currently lists:

```cpp
      SettingInfo::Enum(StrId::STR_TRANSLATION_MODE, &CrossPointSettings::colorTextStyle,
                        {StrId::STR_NORMAL, StrId::STR_TRANSLATION_GREY, StrId::STR_TRANSLATION_LIGHT_GREY,
                         StrId::STR_NO_RENDER, StrId::STR_INVERT_TRANSLATION, StrId::STR_SIDE_BY_SIDE,
                         StrId::STR_TOOLTIP},
                        "colorTextStyle", StrId::STR_CAT_READER),
```

Change to:

```cpp
      SettingInfo::Enum(StrId::STR_TRANSLATION_MODE, &CrossPointSettings::colorTextStyle,
                        {StrId::STR_NORMAL, StrId::STR_TRANSLATION_GREY, StrId::STR_TRANSLATION_LIGHT_GREY,
                         StrId::STR_NO_RENDER, StrId::STR_INVERT_TRANSLATION, StrId::STR_SIDE_BY_SIDE,
                         StrId::STR_TOOLTIP, StrId::STR_MODAL},
                        "colorTextStyle", StrId::STR_CAT_READER),
```

- [ ] **Step 6: Add CT_MODAL to ChapterHtmlSlimParser no-op comment**

In `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:123-126`, the comment currently says:

```cpp
  // Tooltip: keep both original and translated text as-is.
  // Translated words retain their grayLevel marker and will be hidden
  // at render time via colorTextGrayLevel=3, but stay in TextBlock for extraction.
  // (No transformation needed — wordColor passes through unchanged.)
```

Change to:

```cpp
  // Tooltip & Modal: keep both original and translated text as-is.
  // Translated words retain their grayLevel marker and will be hidden
  // at render time via colorTextGrayLevel=3, but stay in TextBlock for extraction.
  // (No transformation needed — wordColor passes through unchanged.)
```

- [ ] **Step 7: Add CT_MODAL to effectiveColorTextStyle mapping in EpubReaderActivity**

In `src/activities/reader/EpubReaderActivity.cpp:853-858`, the effective style mapping currently handles CT_TOOLTIP:

```cpp
    // CT_TOOLTIP renders exactly like Original Only (CT_NO_RENDER).
    // Translation data comes from the HTML file, not the section cache.
    const uint8_t effectiveColorTextStyle =
        !hasTranslation                                                          ? CrossPointSettings::CT_NORMAL
        : SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP ? CrossPointSettings::CT_NO_RENDER
                                                                                 : SETTINGS.colorTextStyle;
```

Change to:

```cpp
    // CT_TOOLTIP and CT_MODAL render exactly like Original Only (CT_NO_RENDER).
    // Translation data comes from the HTML file, not the section cache.
    const uint8_t effectiveColorTextStyle =
        !hasTranslation ? CrossPointSettings::CT_NORMAL
        : (SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP ||
           SETTINGS.colorTextStyle == CrossPointSettings::CT_MODAL)
            ? CrossPointSettings::CT_NO_RENDER
            : SETTINGS.colorTextStyle;
```

- [ ] **Step 8: Build to verify everything compiles**

```bash
pio run 2>&1 | tail -20
```

Expected: successful build with no errors.

- [ ] **Step 9: Commit**

```bash
git add src/CrossPointSettings.h src/SettingsList.h lib/I18n/ lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: add CT_MODAL enum, i18n string, and cache/parser support"
```

---

### Task 2: Create ModalOverlay header with state and public interface

**Files:**
- Create: `src/tooltip/ModalOverlay.h`

- [ ] **Step 1: Create the ModalOverlay header**

Create `src/tooltip/ModalOverlay.h`:

```cpp
#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include <string>
#include <vector>

class ModalOverlay {
 public:
  // Public because static XML callbacks in the .cpp need access.
  struct ParagraphEntry {
    std::string key;          // first N words of original paragraph, normalized
    std::string translation;  // full translated paragraph text
  };
  static std::string paragraphKey(const char* const* words, int count);

  void setTranslatedHtmlPath(const std::string& path);

  bool handleInput(MappedInputManager& input);

  void render(GfxRenderer& renderer, const Page& page, int fontId, int modalFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  void onPageChanged();
  void onSectionChanged();

  bool isActive() const { return active; }

  // Set by handleInput when page turn is needed at content boundary.
  // EpubReaderActivity checks these after handleInput and triggers the page turn.
  bool pendingPageForward = false;
  bool pendingPageBack = false;
  bool activateOnNextPage = false;

 private:
  bool active = false;
  int16_t scrollOffset = 0;
  int16_t totalContentHeight = 0;
  bool pagePrepared = false;
  bool sectionParsed = false;

  std::string translatedHtmlPath;

  // Chapter-level: all (original_key, translation) pairs from HTML.
  std::vector<ParagraphEntry> chapterParagraphs;

  // Page-level: translations for paragraphs visible on current page.
  std::vector<std::string> pageTranslations;

  void parseChapterHtml();
  void preparePage(const Page& page);
};

int getModalFontId();
```

- [ ] **Step 2: Build to verify the header compiles (no .cpp yet, just syntax check)**

```bash
pio run 2>&1 | tail -5
```

Expected: builds fine (header is not included anywhere yet).

- [ ] **Step 3: Commit**

```bash
git add src/tooltip/ModalOverlay.h
git commit -m "feat: add ModalOverlay header with state and public interface"
```

---

### Task 3: Implement ModalOverlay HTML parsing and paragraph key matching

**Files:**
- Create: `src/tooltip/ModalOverlay.cpp`

- [ ] **Step 1: Create ModalOverlay.cpp with HTML parsing and paragraph matching**

Create `src/tooltip/ModalOverlay.cpp`:

```cpp
#include "ModalOverlay.h"

#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "fontIds.h"

// ── Paragraph key (first 6 words, normalized) ────────────────────────────────

static constexpr int KEY_WORDS = 6;

std::string ModalOverlay::paragraphKey(const char* const* words, int count) {
  std::string key;
  int added = 0;
  for (int i = 0; i < count && added < KEY_WORDS; i++) {
    const char* w = words[i];
    if (!w || !w[0]) continue;
    // Skip whitespace-only and single-char punctuation
    if (strlen(w) == 1 && ispunct(static_cast<unsigned char>(w[0]))) continue;
    if (!key.empty()) key += ' ';
    key += w;
    added++;
  }
  // Normalize: lowercase, strip trailing dots/ellipsis
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
      // Build key from original text words
      std::vector<const char*> wordPtrs;
      std::string origCopy = ctx->lastOrigText;
      char* p = &origCopy[0];
      while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        wordPtrs.push_back(p);
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
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
  if (!parser) { file.close(); return; }

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

    // Normalize page key
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
```

- [ ] **Step 2: Build to check compilation (not yet linked from EpubReaderActivity)**

```bash
pio run 2>&1 | tail -20
```

Expected: builds successfully. The .cpp will be compiled because PlatformIO picks up all source files in `src/`.

- [ ] **Step 3: Commit**

```bash
git add src/tooltip/ModalOverlay.cpp
git commit -m "feat: implement ModalOverlay HTML parsing and paragraph matching"
```

---

### Task 4: Implement ModalOverlay button handling and state machine

**Files:**
- Modify: `src/tooltip/ModalOverlay.cpp`

- [ ] **Step 1: Add setTranslatedHtmlPath, onPageChanged, and onSectionChanged**

Add at the top of `src/tooltip/ModalOverlay.cpp`, after the includes and before the paragraph key section:

```cpp
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
  pendingPageForward = false;
  pendingPageBack = false;
  activateOnNextPage = false;
}

void ModalOverlay::onPageChanged() {
  bool shouldActivate = activateOnNextPage;
  active = false;
  scrollOffset = 0;
  totalContentHeight = 0;
  pagePrepared = false;
  pageTranslations.clear();
  pendingPageForward = false;
  pendingPageBack = false;
  activateOnNextPage = false;

  if (shouldActivate) {
    active = true;
  }
}
```

- [ ] **Step 2: Add handleInput with scroll and page-turn logic**

Add after the state management section:

```cpp
// ── Button handling ──────────────────────────────────────────────────────────

bool ModalOverlay::handleInput(MappedInputManager& input) {
  const bool useFrontButtons = (SETTINGS.tooltipButtons == 0);
  const auto nextBtn =
      useFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
  const auto backBtn =
      useFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;
  const bool pageTurnMode = (SETTINGS.tooltipBehavior == 1);
  constexpr unsigned long longPressMs = 700;

  // Next button
  if (input.wasReleased(nextBtn)) {
    // Long press: page turn forward, dismiss modal.
    if (input.getHeldTime() >= longPressMs) {
      pendingPageForward = true;
      active = false;
      scrollOffset = 0;
      return true;
    }
    if (!active) {
      // Activate: show modal at top.
      active = true;
      scrollOffset = 0;
      return true;
    }
    // Active: scroll down.
    // totalContentHeight is computed during render, so on first press it may be 0.
    // In that case, just stay at offset 0 and let the first render compute it.
    if (totalContentHeight > 0) {
      const int screenScroll = std::max(1, (int)totalContentHeight / 4);  // temporary; real value set in render
      // This will be refined in the render task — for now, store a "scroll down" intent.
      // We use a simple approach: advance by a fixed amount, clamped.
      scrollOffset += screenScroll;
      if (scrollOffset >= totalContentHeight) {
        // At bottom.
        if (pageTurnMode) {
          pendingPageForward = true;
          activateOnNextPage = true;
          active = false;
          scrollOffset = 0;
        } else {
          // Loop: wrap to top.
          scrollOffset = 0;
        }
      }
    }
    return true;
  }

  // Back button
  if (input.wasReleased(backBtn)) {
    // Long press: page turn backward, dismiss modal.
    if (input.getHeldTime() >= longPressMs) {
      pendingPageBack = true;
      active = false;
      scrollOffset = 0;
      return true;
    }
    if (!active) {
      // Not active: don't consume, let page-turn handle it.
      return false;
    }
    // Active: scroll up or dismiss.
    if (scrollOffset > 0) {
      const int screenScroll = std::max(1, (int)totalContentHeight / 4);
      scrollOffset -= screenScroll;
      if (scrollOffset < 0) scrollOffset = 0;
    } else {
      // At top: dismiss modal.
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
```

- [ ] **Step 3: Build to verify**

```bash
pio run 2>&1 | tail -20
```

Expected: successful build.

- [ ] **Step 4: Commit**

```bash
git add src/tooltip/ModalOverlay.cpp
git commit -m "feat: implement ModalOverlay button handling and state machine"
```

---

### Task 5: Implement ModalOverlay rendering with word-wrap and scrolling

**Files:**
- Modify: `src/tooltip/ModalOverlay.cpp`

- [ ] **Step 1: Add the render method and font helper**

Add at the end of `src/tooltip/ModalOverlay.cpp`:

```cpp
// ── Rendering ────────────────────────────────────────────────────────────────

void ModalOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int modalFontId, int xOffset,
                          int yOffset, int viewportWidth, int viewportHeight) {
  if (!active) return;

  preparePage(page);

  if (pageTranslations.empty()) {
    // No translations for this page — deactivate.
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

  // Two-pass approach:
  // Pass 1 (measure): compute totalContentHeight.
  // Pass 2 (draw): render text with scrollOffset applied.
  // We always do both passes since totalContentHeight can change if font changes.
  int contentH = 0;
  for (const auto& trans : pageTranslations) {
    contentH += measureParagraphHeight(renderer, modalFontId, trans.c_str(), maxTextW, lh, spW);
    contentH += paraSpacing;
  }
  if (!pageTranslations.empty()) contentH -= paraSpacing;  // No trailing spacing.
  totalContentHeight = std::max(0, contentH - viewportHeight);  // Scrollable range.

  // Clamp scrollOffset.
  if (scrollOffset > totalContentHeight) scrollOffset = totalContentHeight;
  if (scrollOffset < 0) scrollOffset = 0;

  // Draw pass.
  int curY = yOffset - scrollOffset;
  for (const auto& trans : pageTranslations) {
    curY = drawParagraph(renderer, modalFontId, trans.c_str(), xOffset + PAD, curY, maxTextW, lh, spW,
                         yOffset, yOffset + viewportHeight);
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
    if (p == ls) break;  // Safety: avoid infinite loop on unmeasurable word.
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
    // Draw this line if within visible area.
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
    if (p == lineStart) break;  // Safety.
  }
  return y;
}

// ── Font helper ──────────────────────────────────────────────────────────────

int getModalFontId() {
  // Same logic as getTooltipFontId() — one size smaller than current reader font.
  return getTooltipFontId();
}
```

- [ ] **Step 2: Add the private method declarations to the header**

In `src/tooltip/ModalOverlay.h`, add these two private method declarations inside the class, after `static std::string paragraphKey(...)`:

```cpp
  static int measureParagraphHeight(GfxRenderer& renderer, int fontId, const char* text, int maxW, int lh, int spW);
  static int drawParagraph(GfxRenderer& renderer, int fontId, const char* text, int x, int y, int maxW, int lh,
                           int spW, int clipTop, int clipBottom);
```

- [ ] **Step 3: Update handleInput scroll amounts to use viewportHeight**

The scroll amount in `handleInput` currently uses `totalContentHeight / 4` as a placeholder. We need to store the `viewportHeight` so the input handler can compute proper scroll distances. Add a member to the header:

In `src/tooltip/ModalOverlay.h`, add to private members:

```cpp
  int16_t cachedViewportHeight = 0;
```

Then in `ModalOverlay::render()`, after the clamp block, add:

```cpp
  cachedViewportHeight = viewportHeight;
```

And update `handleInput` to use it. Replace both occurrences of:

```cpp
      const int screenScroll = std::max(1, (int)totalContentHeight / 4);
```

with:

```cpp
      const int screenScroll = cachedViewportHeight > 0 ? cachedViewportHeight - lh : 200;
```

Wait — `handleInput` doesn't have access to `lh` (line height). We need to store it. Add another member to the header:

```cpp
  int16_t cachedLineHeight = 0;
```

Set it in `render()` after computing `lh`:

```cpp
  cachedLineHeight = lh;
```

Now replace both scroll computations in `handleInput` with:

```cpp
      const int overlap = cachedLineHeight > 0 ? cachedLineHeight : 20;
      const int screenScroll = cachedViewportHeight > overlap ? cachedViewportHeight - overlap : 200;
```

- [ ] **Step 4: Build to verify**

```bash
pio run 2>&1 | tail -20
```

Expected: successful build.

- [ ] **Step 5: Commit**

```bash
git add src/tooltip/ModalOverlay.h src/tooltip/ModalOverlay.cpp
git commit -m "feat: implement ModalOverlay rendering with word-wrap and scrolling"
```

---

### Task 6: Integrate ModalOverlay into EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h:10,15`
- Modify: `src/activities/reader/EpubReaderActivity.cpp:126-128,300-328,376-378,430-432,709-731,907-926,1014-1021`

- [ ] **Step 1: Add ModalOverlay member to EpubReaderActivity header**

In `src/activities/reader/EpubReaderActivity.h`, add the include after the TooltipOverlay include (line 10):

```cpp
#include "tooltip/ModalOverlay.h"
```

Add the member after `tooltipOverlay` (line 15):

```cpp
  std::unique_ptr<ModalOverlay> modalOverlay;
```

- [ ] **Step 2: Create/destroy modalOverlay in onEnter**

In `src/activities/reader/EpubReaderActivity.cpp`, after the tooltip creation block at line 126-128:

```cpp
  if (SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP) {
    tooltipOverlay = std::make_unique<TooltipOverlay>();
  }
```

Add:

```cpp
  if (SETTINGS.colorTextStyle == CrossPointSettings::CT_MODAL) {
    modalOverlay = std::make_unique<ModalOverlay>();
  }
```

- [ ] **Step 3: Add modal input dispatch in loop()**

In `EpubReaderActivity::loop()`, after the tooltip input dispatch block (lines 300-328), add the same pattern for modal:

```cpp
  // Modal mode: let overlay consume button presses for scrolling
  if (modalOverlay && modalOverlay->handleInput(mappedInput)) {
    if (modalOverlay->pendingPageForward || modalOverlay->pendingPageBack) {
      const bool forward = modalOverlay->pendingPageForward;
      modalOverlay->onPageChanged();
      if (forward) {
        if (section->currentPage < section->pageCount - 1) {
          section->currentPage++;
        } else {
          RenderLock lock(*this);
          nextPageNumber = 0;
          currentSpineIndex++;
          section.reset();
        }
      } else {
        if (section->currentPage > 0) {
          section->currentPage--;
        } else {
          RenderLock lock(*this);
          nextPageNumber = UINT16_MAX;
          currentSpineIndex--;
          section.reset();
        }
      }
    }
    requestUpdate();
    return;
  }
```

- [ ] **Step 4: Add modal to button-skip logic**

In `EpubReaderActivity::loop()`, at lines 376-378, update the button skip logic to also account for modal mode:

```cpp
  const bool isTooltipMode = SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP && tooltipOverlay;
  const bool tooltipUsesFront = isTooltipMode && SETTINGS.tooltipButtons == 0;
  const bool tooltipUsesSide = isTooltipMode && SETTINGS.tooltipButtons == 1;
```

Change to:

```cpp
  const bool isTooltipMode = SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP && tooltipOverlay;
  const bool isModalMode = SETTINGS.colorTextStyle == CrossPointSettings::CT_MODAL && modalOverlay;
  const bool overlayMode = isTooltipMode || isModalMode;
  const bool tooltipUsesFront = overlayMode && SETTINGS.tooltipButtons == 0;
  const bool tooltipUsesSide = overlayMode && SETTINGS.tooltipButtons == 1;
```

- [ ] **Step 5: Reset modal on page change**

At lines 430-432, after the tooltip page-change reset:

```cpp
  if (tooltipOverlay && (prevTriggered || nextTriggered)) {
    tooltipOverlay->onPageChanged();
  }
```

Add:

```cpp
  if (modalOverlay && (prevTriggered || nextTriggered)) {
    modalOverlay->onPageChanged();
  }
```

- [ ] **Step 6: Handle modal in applyTranslationMode**

In `applyTranslationMode()` (lines 709-731), update the overlay create/destroy block:

```cpp
    // Create or destroy tooltip overlay based on mode
    if (translationMode == CrossPointSettings::CT_TOOLTIP) {
      tooltipOverlay = std::make_unique<TooltipOverlay>();
    } else {
      tooltipOverlay.reset();
    }
```

Change to:

```cpp
    // Create or destroy overlays based on mode
    if (translationMode == CrossPointSettings::CT_TOOLTIP) {
      tooltipOverlay = std::make_unique<TooltipOverlay>();
      modalOverlay.reset();
    } else if (translationMode == CrossPointSettings::CT_MODAL) {
      modalOverlay = std::make_unique<ModalOverlay>();
      tooltipOverlay.reset();
    } else {
      tooltipOverlay.reset();
      modalOverlay.reset();
    }
```

- [ ] **Step 7: Set up modal HTML source on section load**

After the tooltip HTML setup block (lines 907-926), add the equivalent for modal:

```cpp
    // Set up modal data source.
    if (modalOverlay) {
      modalOverlay->onSectionChanged();
      if (section->hasTranslatedHtml()) {
        modalOverlay->setTranslatedHtmlPath(section->getTranslatedHtmlPath());
      } else if (hasTranslation) {
        const auto modalPath =
            epub->getCachePath() + "/sections/" + std::to_string(currentSpineIndex) + ".modal.html";
        if (!Storage.exists(modalPath.c_str())) {
          const auto href = epub->getSpineItem(currentSpineIndex).href;
          FsFile tmpFile;
          if (Storage.openFileForWrite("MOD", modalPath, tmpFile)) {
            epub->readItemContentsToStream(href, tmpFile, 1024);
            tmpFile.close();
          }
        }
        modalOverlay->setTranslatedHtmlPath(modalPath);
      }
    }
```

- [ ] **Step 8: Add modal render call in renderContents**

In `renderContents()`, after the tooltip render block (lines 1014-1021):

```cpp
  if (tooltipOverlay && tooltipOverlay->isActive()) {
    const int tooltipFontId = getTooltipFontId();
    const int vpWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int vpHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    tooltipOverlay->render(renderer, *page, SETTINGS.getReaderFontId(), tooltipFontId, orientedMarginLeft,
                           orientedMarginTop, vpWidth, vpHeight);
  }
```

Add:

```cpp
  if (modalOverlay && modalOverlay->isActive()) {
    const int modalFontId = getModalFontId();
    const int vpWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int vpHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    modalOverlay->render(renderer, *page, SETTINGS.getReaderFontId(), modalFontId, orientedMarginLeft,
                         orientedMarginTop, vpWidth, vpHeight);
  }
```

- [ ] **Step 9: Build to verify full integration compiles**

```bash
pio run 2>&1 | tail -20
```

Expected: successful build.

- [ ] **Step 10: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: integrate ModalOverlay into EpubReaderActivity"
```

---

### Task 7: Build release and flash for manual testing

**Files:** None (verification only)

- [ ] **Step 1: Build release**

```bash
pio run 2>&1 | tail -20
```

Expected: successful build with no errors or warnings.

- [ ] **Step 2: Flash to device (if connected)**

```bash
pio run --target upload 2>&1 | tail -20
```

- [ ] **Step 3: Manual test checklist**

Test on device with a translated book:
1. Open Settings → Reader → Translation Mode → set to "Modal"
2. Open a translated book chapter
3. Press next button → white overlay appears with translated paragraphs
4. Press next again → overlay scrolls down (if content is long enough)
5. Press back → overlay scrolls up or dismisses at top
6. Press ESC/Back → overlay dismisses
7. Long-press next → page turns forward
8. Long-press back → page turns backward
9. Set Tooltip Navigation to "Page Turn" → verify page turn at end of scroll
10. Set Tooltip Navigation to "Loop" → verify wrap-around at end of scroll
11. Switch Tooltip Buttons between Front/Side → verify correct buttons control modal
12. Switch to an untranslated chapter → verify buttons work normally (no overlay)
13. Switch back to Tooltip mode → verify tooltip still works correctly

- [ ] **Step 4: Commit any fixes found during testing**

```bash
git add -u
git commit -m "fix: address issues found during modal translation testing"
```

(Only if fixes are needed.)

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | CT_MODAL enum + i18n + parser/cache support | 12 files |
| 2 | ModalOverlay header | 1 new file |
| 3 | HTML parsing + paragraph matching | 1 new file |
| 4 | Button handling + state machine | modify .cpp |
| 5 | Rendering with word-wrap + scrolling | modify .h + .cpp |
| 6 | EpubReaderActivity integration | 2 files |
| 7 | Build, flash, manual test | verification |
