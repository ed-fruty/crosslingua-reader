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
  const bool pageTurnMode = (SETTINGS.tooltipBehavior == 1);
  constexpr unsigned long longPressMs = 700;

  // Next button.
  if (input.wasReleased(nextBtn)) {
    // Long press: turn page forward (like non-tooltip mode), dismiss tooltip.
    if (input.getHeldTime() >= longPressMs) {
      pendingPageForward = true;
      currentSentenceIndex = -1;
      return true;
    }
    skipDirection = 1;
    if (currentSentenceIndex < 0) {
      currentSentenceIndex = 0;  // activate; auto-skip finds first with translation
      return true;
    }
    if (currentSentenceIndex < splits.count - 1) {
      currentSentenceIndex++;
      return true;
    }
    // At last sentence.
    if (pageTurnMode) {
      pendingPageForward = true;
      activateOnNextPage = true;
      currentSentenceIndex = -1;
    } else {
      // Loop: wrap to first.
      currentSentenceIndex = 0;
    }
    return true;
  }

  // Back button.
  if (input.wasReleased(backBtn)) {
    // Long press: turn page backward (like non-tooltip mode), dismiss tooltip.
    if (input.getHeldTime() >= longPressMs) {
      pendingPageBack = true;
      currentSentenceIndex = -1;
      return true;
    }
    skipDirection = -1;
    if (currentSentenceIndex < 0) {
      currentSentenceIndex = 0;  // activate; auto-skip with dir=-1 wraps to last
      return true;
    }
    if (currentSentenceIndex > 0) {
      currentSentenceIndex--;
      return true;
    }
    // At first sentence.
    if (pageTurnMode) {
      pendingPageBack = true;
      activateOnNextPage = true;
      currentSentenceIndex = -1;
    } else {
      // Loop: wrap to last.
      currentSentenceIndex = std::max(0, splits.count - 1);
    }
    return true;
  }

  // ESC/Back button: dismiss tooltip if active.
  if (input.wasReleased(MappedInputManager::Button::Back)) {
    if (currentSentenceIndex >= 0) {
      currentSentenceIndex = -1;
      return true;
    }
    return false;
  }

  return false;
}

void TooltipOverlay::onPageChanged() {
  bool shouldActivate = activateOnNextPage;
  int8_t dir = skipDirection;
  currentSentenceIndex = -1;
  skipDirection = 1;
  pagePrepared = false;
  origWordCount = 0;
  sentenceTranslations.clear();
  splits.count = 0;
  activateOnNextPage = false;
  activateFromEnd = false;
  pendingPageForward = false;
  pendingPageBack = false;

  // After a tooltip-triggered page turn, auto-activate on the new page.
  if (shouldActivate) {
    currentSentenceIndex = 0;  // preparePage will run; auto-skip uses direction
    skipDirection = dir;
    activateFromEnd = (dir < 0);  // going back → show last sentence
  }
}

// ── Chapter parsing: extract (orig, trans) paragraph pairs ───────────────────

static const char* BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "div", nullptr};

static bool isBlockTag(const char* name) {
  for (int i = 0; BLOCK_TAGS[i]; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

struct SentEntry {
  std::string key;          // first 6 words (normalized) of original sentence
  std::string translation;  // mapped translation text
};

// Forward declaration — builds index entries for one paragraph pair.
static void addPairToIndex(const std::string& origText, const std::string& transText,
                           std::vector<SentEntry>& index);

struct ParseCtx {
  std::vector<SentEntry>* index;
  int blockDepth = 0;
  bool inBlock = false;
  bool isTranslation = false;
  std::string currentText;
  std::string lastOrigText;
  bool hasLastOrig = false;
  int pairCount = 0;
  // Selective parse (mirrors ModalOverlay): only build index entries for paragraphs the current
  // page actually shows. Without this the index holds every translated sentence in the chapter,
  // which exhausts the heap on long chapters and reboots the device.
  int wantFirst = 0;
  int wantLast = 0;
  int paragraphCounter = -1;  // pre-increment on each ORIGINAL block; first original becomes idx 0
  int lastOrigIdx = -1;       // paragraph index of the most recent original block
  XML_Parser parser = nullptr;  // for XML_StopParser once we walk past the page
};

static void XMLCALL chOnStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ParseCtx*>(ud);
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

static void XMLCALL chOnEnd(void* ud, const XML_Char* name) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  if (!ctx->inBlock) return;
  ctx->blockDepth--;
  if (ctx->blockDepth > 0) return;
  ctx->inBlock = false;

  auto& t = ctx->currentText;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (t.empty()) return;

  if (ctx->isTranslation) {
    // hasLastOrig is set only for in-range originals, so this naturally skips out-of-range pairs.
    if (ctx->hasLastOrig) {
      // Process pair immediately — don't accumulate all pairs in memory.
      addPairToIndex(ctx->lastOrigText, t, *ctx->index);
      ctx->pairCount++;
      ctx->lastOrigText.clear();
      ctx->hasLastOrig = false;
    }
  } else {
    // Original block: bump the paragraph counter, then decide whether it falls on the page.
    const int idx = ++ctx->paragraphCounter;
    ctx->lastOrigIdx = idx;
    if (idx > ctx->wantLast) {
      // Past the page (and the prior paragraph's translation has already been processed) — stop
      // the parser so expat doesn't scan the rest of the chapter. Returns XML_ERROR_ABORTED,
      // which parseAndBuildIndex treats as the success path.
      if (ctx->parser) XML_StopParser(ctx->parser, XML_FALSE);
      ctx->lastOrigText.clear();
      ctx->hasLastOrig = false;
      return;
    }
    if (idx < ctx->wantFirst) {
      ctx->lastOrigText.clear();  // before the page — discard, never pairs
      ctx->hasLastOrig = false;
      return;
    }
    ctx->lastOrigText = std::move(t);
    ctx->hasLastOrig = true;
  }
}

static void XMLCALL chOnChar(void* ud, const XML_Char* data, int len) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  if (!ctx->inBlock) return;
  for (int i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && !ctx->currentText.empty() && ctx->currentText.back() == ' ') continue;
    ctx->currentText += c;
  }
}

// ── Sentence index: map original sentences to translations ───────────────────

static inline bool isAsciiSpace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static std::vector<std::string> tokenizeWords(const std::string& text) {
  std::vector<std::string> words;
  const char* p = text.c_str();
  while (*p) {
    while (*p && isAsciiSpace(*p)) p++;
    if (!*p) break;
    const char* ws = p;
    while (*p && !isAsciiSpace(*p)) p++;
    if (p > ws) words.emplace_back(ws, p);
  }
  return words;
}

static std::string joinSpan(const std::vector<std::string>& words, const SentenceSpan& span) {
  std::string result;
  for (int i = span.startWord; i < span.endWord && i < (int)words.size(); i++) {
    if (!result.empty()) result += ' ';
    result += words[i];
  }
  return result;
}

// Build a match key from the first N meaningful words of a sentence.
// Skips whitespace-only words (from NBSP), strips leading em-space,
// and stops before hyphenated line-break fragments (e.g., "dun-").
static std::string sentenceKey(const char* const* words, int start, int end, int n = 6) {
  std::string key;
  int count = 0;
  for (int i = start; i < end && count < n; i++) {
    const char* w = words[i];
    int wlen = (int)strlen(w);
    if (wlen == 0) continue;
    // Skip whitespace-only words
    bool allSpace = true;
    for (int j = 0; j < wlen; j++) { if (!isAsciiSpace(w[j])) { allSpace = false; break; } }
    if (allSpace) continue;
    // Skip single-char punctuation words (spaced ellipsis ".", stray punctuation)
    if (wlen == 1 && !((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z') ||
                       (w[0] >= '0' && w[0] <= '9') || (uint8_t)w[0] >= 0x80)) continue;
    // Skip Unicode ellipsis … (U+2026 = E2 80 A6) — standalone or as entire word
    if (wlen == 3 && (uint8_t)w[0] == 0xE2 && (uint8_t)w[1] == 0x80 && (uint8_t)w[2] == 0xA6) continue;
    // Join hyphenated line-break fragments: "over-" + "whelming" → "overwhelming"
    if (wlen > 1 && w[wlen - 1] == '-' && i + 1 < end) {
      if (!key.empty()) key += ' ';
      key.append(w, wlen - 1);         // "over" (strip hyphen)
      key.append(words[i + 1]);         // + "whelming" → "overwhelming"
      i++;                              // skip the continuation word
      count++;
      continue;
    }
    // Strip leading em-space (U+2003 = E2 80 83)
    while (wlen >= 3 && (uint8_t)w[0] == 0xE2 && (uint8_t)w[1] == 0x80 && (uint8_t)w[2] == 0x83) {
      w += 3; wlen -= 3;
    }
    // Strip trailing Unicode ellipsis … (U+2026 = E2 80 A6) — e.g., "relief…" → "relief"
    while (wlen >= 3 && (uint8_t)w[wlen - 3] == 0xE2 && (uint8_t)w[wlen - 2] == 0x80 &&
           (uint8_t)w[wlen - 1] == 0xA6) {
      wlen -= 3;
    }
    // Strip trailing ASCII dots (e.g., "word..." → "word")
    while (wlen > 0 && w[wlen - 1] == '.') wlen--;
    if (wlen == 0) continue;
    if (!key.empty()) key += ' ';
    key.append(w, wlen);
    count++;
  }
  return key;
}

// Process one (original, translation) paragraph pair: split both into sentences,
// map by character-midpoint, and add entries to the index.
// Called during HTML parsing — only one pair in memory at a time.
static void addPairToIndex(const std::string& origText, const std::string& transText,
                           std::vector<SentEntry>& index) {
  auto origWords = tokenizeWords(origText);
  auto transWords = tokenizeWords(transText);
  if (origWords.empty() || transWords.empty()) return;

  std::vector<const char*> origPtrs, transPtrs;
  origPtrs.reserve(origWords.size());
  transPtrs.reserve(transWords.size());
  for (auto& w : origWords) origPtrs.push_back(w.c_str());
  for (auto& w : transWords) transPtrs.push_back(w.c_str());

  SentenceSplitResult origSplits = splitSentences(origPtrs.data(), (int)origPtrs.size());
  SentenceSplitResult transSplits = splitSentences(transPtrs.data(), (int)transPtrs.size());
  if (origSplits.count == 0 || transSplits.count == 0) return;

  int origTotalChars = 0;
  for (auto& w : origWords) origTotalChars += (int)w.size() + 1;
  int transTotalChars = 0;
  for (auto& w : transWords) transTotalChars += (int)w.size() + 1;

  // Precompute translation sentence midpoints as fractions.
  float transMidFrac[MAX_SENTENCES];
  {
    int tCum = 0;
    for (int ts = 0; ts < transSplits.count; ts++) {
      int tStart = tCum;
      for (int w = transSplits.spans[ts].startWord; w < transSplits.spans[ts].endWord; w++)
        tCum += (int)transWords[w].size() + 1;
      transMidFrac[ts] = transTotalChars > 0 ? (float)(tStart + tCum) / 2.0f / transTotalChars : 0;
    }
  }

  int oCum = 0;
  for (int os = 0; os < origSplits.count; os++) {
    int oStart = oCum;
    for (int w = origSplits.spans[os].startWord; w < origSplits.spans[os].endWord; w++)
      oCum += (int)origWords[w].size() + 1;
    float origStartFrac = origTotalChars > 0 ? (float)oStart / origTotalChars : 0;
    float origEndFrac = origTotalChars > 0 ? (float)oCum / origTotalChars : 1;

    // Collect ALL translation sentences whose midpoint falls in [origStart, origEnd).
    std::string trans;
    for (int ts = 0; ts < transSplits.count; ts++) {
      if (transMidFrac[ts] >= origStartFrac && transMidFrac[ts] < origEndFrac) {
        if (!trans.empty()) trans += ' ';
        trans += joinSpan(transWords, transSplits.spans[ts]);
      }
    }
    // Fallback: closest midpoint if none fell in range.
    if (trans.empty()) {
      float origMid = (origStartFrac + origEndFrac) / 2;
      int bestTs = 0;
      float bestDist = 999.0f;
      for (int ts = 0; ts < transSplits.count; ts++) {
        float dist = std::abs(transMidFrac[ts] - origMid);
        if (dist < bestDist) { bestDist = dist; bestTs = ts; }
      }
      trans = joinSpan(transWords, transSplits.spans[bestTs]);
    }

    std::string key = sentenceKey(origPtrs.data(), origSplits.spans[os].startWord,
                                  origSplits.spans[os].endWord);
    if (!key.empty() && !trans.empty()) {
      index.push_back({std::move(key), std::move(trans)});
    }
  }
}

// Parse HTML and build sentence index in one pass — no intermediate storage. Only paragraphs in
// [wantFirst, wantLast] (the current page's range) get index entries, so peak RAM is bounded by one
// page's worth of translations rather than the whole chapter.
static std::vector<SentEntry> parseAndBuildIndex(const std::string& path, int wantFirst, int wantLast) {
  std::vector<SentEntry> index;
  if (path.empty()) return index;

  FsFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f) return index;

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) { f.close(); return index; }

  ParseCtx ctx;
  ctx.index = &index;
  ctx.wantFirst = wantFirst;
  ctx.wantLast = wantLast;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, chOnStart, chOnEnd);
  XML_SetCharacterDataHandler(parser, chOnChar);

  char buf[1024];
  const size_t fileSize = f.size();
  size_t totalRead = 0;
  bool stopped = false;
  while (totalRead < fileSize) {
    const size_t toRead = std::min(fileSize - totalRead, sizeof(buf));
    const int n = f.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (n <= 0) break;
    totalRead += n;
    if (XML_Parse(parser, buf, n, (totalRead >= fileSize) ? 1 : 0) == XML_STATUS_ERROR) {
      // XML_StopParser() (we walked past the page) makes expat return ERROR with code
      // XML_ERROR_ABORTED — that's our success path, not a parse failure.
      if (XML_GetErrorCode(parser) == XML_ERROR_ABORTED) stopped = true;
      break;
    }
  }
  XML_ParserFree(parser);
  f.close();

  LOG_DBG("TIP", "Index: %d entries from %d pairs in [%d..%d]%s", (int)index.size(), ctx.pairCount, wantFirst,
          wantLast, stopped ? " (early-stop)" : "");
  return index;
}

// ── Page preparation ──────────────────────────────────────────────────────────

void TooltipOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  sentenceTranslations.clear();

  // 1. Collect original words from page.
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    for (const auto& w : line->getTextBlock()->getWords()) {
      if (origWordCount < MAX_WORDS) origWordPtrs[origWordCount++] = w.c_str();
    }
  }

  // 2. Split into sentences, then merge any "empty" sentences (dots, fragments)
  //    into the previous sentence so the user doesn't click through junk.
  splits = splitSentences(origWordPtrs, origWordCount);
  for (int i = splits.count - 1; i > 0; i--) {
    std::string key = sentenceKey(origWordPtrs, splits.spans[i].startWord, splits.spans[i].endWord);
    if (key.empty() || (key.size() <= 2 && splits.spans[i].endWord - splits.spans[i].startWord <= 3)) {
      // Merge into previous sentence: extend its endWord.
      splits.spans[i - 1].endWord = splits.spans[i].endWord;
      for (int j = i; j < splits.count - 1; j++) splits.spans[j] = splits.spans[j + 1];
      splits.count--;
    }
  }
  LOG_DBG("TIP", "Page: %d words, %d sentences (after merge)", origWordCount, splits.count);

  // 3. Parse HTML and build sentence index in one pass (memory-efficient). Restrict to the
  //    paragraph range this page shows — otherwise the index holds the whole chapter's
  //    translations and exhausts the heap on long chapters.
  if (page.firstParagraphIdx < 0 || page.lastParagraphIdx < 0) {
    LOG_DBG("TIP", "Page has no paragraph indices (old cache?) — skipping tooltip index");
    return;
  }
  auto index = parseAndBuildIndex(translatedHtmlPath, page.firstParagraphIdx, page.lastParagraphIdx);
  if (index.empty()) {
    LOG_DBG("TIP", "No index entries from %s", translatedHtmlPath.c_str());
    return;
  }

  // 4. Match each page sentence against the index by key, then gap-fill neighbors.
  sentenceTranslations.resize(splits.count);
  std::vector<int> matchedIdx(splits.count, -1);
  int matched = 0, lastIdx = -1;

  // Pre-normalize index keys (strip dots, collapse spaces).
  std::vector<std::string> normKeys(index.size());
  for (int j = 0; j < (int)index.size(); j++) {
    auto& nk = normKeys[j];
    for (char c : index[j].key) {
      if (c == '.') continue;
      if (c == ' ' && (nk.empty() || nk.back() == ' ')) continue;
      nk += c;
    }
    while (!nk.empty() && nk.back() == ' ') nk.pop_back();
  }

  // Forward pass: match by key.
  for (int s = 0; s < splits.count; s++) {
    std::string pk = sentenceKey(origWordPtrs, splits.spans[s].startWord, splits.spans[s].endWord);
    if (pk.empty()) continue;
    std::string np;
    for (char c : pk) {
      if (c == '.') continue;
      if (c == ' ' && (np.empty() || np.back() == ' ')) continue;
      np += c;
    }
    while (!np.empty() && np.back() == ' ') np.pop_back();

    int foundIdx = -1;
    // Sequential hint.
    if (lastIdx >= 0 && lastIdx + 1 < (int)index.size()) {
      int cl = (int)std::min(np.size(), normKeys[lastIdx + 1].size());
      if (cl >= 3 && np.compare(0, cl, normKeys[lastIdx + 1], 0, cl) == 0) foundIdx = lastIdx + 1;
    }
    // Full search fallback.
    if (foundIdx < 0) {
      int bestLen = 0;
      for (int j = 0; j < (int)index.size(); j++) {
        int cl = (int)std::min(np.size(), normKeys[j].size());
        if (cl < 3) continue;
        if (np.compare(0, cl, normKeys[j], 0, cl) == 0 && cl > bestLen) { bestLen = cl; foundIdx = j; }
      }
    }
    if (foundIdx >= 0) {
      sentenceTranslations[s] = index[foundIdx].translation;
      matchedIdx[s] = foundIdx;
      lastIdx = foundIdx;
      matched++;
    }
  }

  // Gap fill: infer unmatched sentences from neighbors.
  // Backward: if s+1 matched at idx N, s gets idx N-1.
  for (int s = splits.count - 2; s >= 0; s--) {
    if (matchedIdx[s] >= 0) continue;
    if (matchedIdx[s + 1] > 0) {
      int idx = matchedIdx[s + 1] - 1;
      sentenceTranslations[s] = index[idx].translation;
      matchedIdx[s] = idx;
      matched++;
    }
  }
  // Forward: if s-1 matched at idx N, s gets idx N+1.
  for (int s = 1; s < splits.count; s++) {
    if (matchedIdx[s] >= 0) continue;
    if (matchedIdx[s - 1] >= 0 && matchedIdx[s - 1] + 1 < (int)index.size()) {
      int idx = matchedIdx[s - 1] + 1;
      sentenceTranslations[s] = index[idx].translation;
      matchedIdx[s] = idx;
      matched++;
    }
  }

  LOG_DBG("TIP", "Matched %d/%d sentences (incl gap-fill)", matched, splits.count);
}

// ── Sentence bounds and underline ─────────────────────────────────────────────

TooltipOverlay::SentenceBounds TooltipOverlay::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                   int fontId, int xOffset, int yOffset) const {
  SentenceBounds bounds = {0, 0, 0};
  int idx = 0;
  bool found = false;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& words = line->getTextBlock()->getWords();
    const auto& xpos = line->getTextBlock()->getWordXpos();
    auto wIt = words.begin(); auto xIt = xpos.begin();
    for (; wIt != words.end() && xIt != xpos.end(); ++wIt, ++xIt) {
      if (idx >= span.startWord && idx < span.endWord) {
        int wx = *xIt + line->xPos + xOffset;
        int wy = line->yPos + yOffset;
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
  int idx = 0, curY = -1, sx = 0, ex = 0;
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
        int wx = *xIt + line->xPos + xOffset;
        int wy = line->yPos + yOffset;
        int ww = renderer.getTextWidth(fontId, wIt->c_str(),
                                        static_cast<EpdFontFamily::Style>((uint8_t)(*sIt) & 0x1F));
        if (wy != curY) {
          if (curY >= 0) renderer.drawLine(sx, curY + ulOff, ex, curY + ulOff, true);
          curY = wy; sx = wx; ex = wx + ww;
        } else {
          ex = wx + ww;
        }
      }
      idx++;
    }
  }
  if (curY >= 0) renderer.drawLine(sx, curY + ulOff, ex, curY + ulOff, true);
}

// ── Tooltip rendering ─────────────────────────────────────────────────────────

static int findLastLineY(const Page& page, const SentenceSpan& span, int yOffset) {
  int idx = 0, lastY = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    for (const auto& w : static_cast<const PageLine*>(el.get())->getTextBlock()->getWords()) {
      (void)w;
      if (idx >= span.startWord && idx < span.endWord) lastY = el->yPos + yOffset;
      idx++;
    }
  }
  return lastY;
}

void TooltipOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset,
                            int yOffset, int viewportWidth, int viewportHeight) {
  if (currentSentenceIndex < 0) return;

  preparePage(page);

  if (currentSentenceIndex >= splits.count) { currentSentenceIndex = -1; return; }

  // After back-page-turn: jump to last sentence now that splits are populated.
  if (activateFromEnd && splits.count > 0) {
    currentSentenceIndex = splits.count - 1;
    activateFromEnd = false;
  }

  // Auto-skip sentences without translations (e.g., partial sentences from previous page).
  // Search in the user's navigation direction, wrapping around.
  auto hasTranslation = [&](int i) {
    return i >= 0 && i < splits.count && i < (int)sentenceTranslations.size() &&
           !sentenceTranslations[i].empty();
  };

  if (!hasTranslation(currentSentenceIndex)) {
    bool found = false;
    int n = splits.count;
    // Walk up to n steps in skipDirection, wrapping around.
    for (int step = 1; step < n && !found; step++) {
      int i = ((currentSentenceIndex + skipDirection * step) % n + n) % n;
      if (hasTranslation(i)) { currentSentenceIndex = i; found = true; }
    }
    if (!found) { currentSentenceIndex = -1; return; }
  }

  if (currentSentenceIndex >= splits.count) { currentSentenceIndex = -1; return; }

  const auto& span = splits.spans[currentSentenceIndex];

  // Get pre-computed translation for this sentence.
  const char* text = (currentSentenceIndex < (int)sentenceTranslations.size())
                         ? sentenceTranslations[currentSentenceIndex].c_str()
                         : "";
  if (!text || text[0] == '\0') {
    currentSentenceIndex = -1;  // Nothing to show at all.
    return;
  }
  LOG_DBG("TIP", "Drawing: '%.80s'", text);

  const int lh = renderer.getLineHeight(fontId);
  auto bounds = findSentenceBounds(page, span, fontId, xOffset, yOffset);
  if (bounds.firstLineY == 0 && bounds.startX == 0) return;
  const int lastY = findLastLineY(page, span, yOffset);

  constexpr int PAD = 6, RAD = 3, GAP = 4;
  const int maxW = viewportWidth - 2 * PAD;
  const int tw = renderer.getTextWidth(tooltipFontId, text);
  const int tlh = renderer.getLineHeight(tooltipFontId);

  int tipW, tipH, nLines = 1;
  if (tw <= maxW - 2 * PAD) { tipW = tw + 2 * PAD; tipH = tlh + 2 * PAD; }
  else {
    tipW = maxW;
    // +1 line to account for word-wrap rounding (space widths differ from getTextWidth estimate).
    nLines = (tw + (tipW - 2 * PAD) - 1) / (tipW - 2 * PAD) + 1;
    int maxL = (viewportHeight * 4 / 10) / tlh;
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

  const int avail = tipW - 2 * PAD;
  const int spW = renderer.getSpaceWidth(tooltipFontId);
  const char* p = text;
  int textY = tipY + PAD, drawn = 0;
  while (*p && drawn < nLines) {
    int lineW = 0;
    const char* ls = p, *le = p;
    while (*p) {
      const char* ws = p;
      while (*p && *p != ' ') p++;
      char wb[128]; int wl = std::min((int)(p - ws), 127);
      memcpy(wb, ws, wl); wb[wl] = '\0';
      int ww = renderer.getTextWidth(tooltipFontId, wb);
      if (lineW > 0 && lineW + spW + ww > avail) { p = ws; break; }
      lineW += (lineW > 0 ? spW : 0) + ww;
      le = p;
      while (*p == ' ') p++;
    }
    int dl = (int)(le - ls);
    if (dl > 0) {
      char lb[512]; int cl = std::min(dl, 511);
      memcpy(lb, ls, cl); lb[cl] = '\0';
      renderer.drawText(tooltipFontId, tipX + PAD, textY, lb);
      textY += tlh; drawn++;
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
    case CrossPointSettings::CAECILIA:
      switch (sz) {
        case CrossPointSettings::SMALL: return CAECILIA_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return CAECILIA_14_FONT_ID;
        case CrossPointSettings::LARGE: return CAECILIA_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return CAECILIA_18_FONT_ID;
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
