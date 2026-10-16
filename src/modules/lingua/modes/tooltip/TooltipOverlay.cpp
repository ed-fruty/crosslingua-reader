#include "modules/lingua/modes/tooltip/TooltipOverlay.h"

#include <CrossPointSettings.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "modules/lingua/modes/common/PageTextJoiner.h"
#include "modules/lingua/parsers/TranslationPairWalker.h"
#include "modules/lingua/utils/SentencePairing.h"
#include "modules/lingua/utils/TextNormalize.h"

// ── Button handling ───────────────────────────────────────────────────────────

bool TooltipOverlay::handleInput(MappedInputManager& input) {
  const bool useFrontButtons = (SETTINGS.tooltipButtons == CrossPointSettings::OVERLAY_BUTTONS_FRONT);
  const auto nextBtn = useFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
  const auto backBtn = useFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;
  const bool pageTurnMode = (SETTINGS.tooltipBehavior == CrossPointSettings::TOOLTIP_NAV_TURN_PAGE);
  constexpr unsigned long longPressMs = 700;

  // Next button. Steps are translation units (see steps[]/groupTranslationSteps), so one
  // press advances past a whole merged group rather than re-showing its translation.
  if (input.wasReleased(nextBtn)) {
    // Long press: turn page forward (like non-tooltip mode), dismiss tooltip.
    if (input.getHeldTime() >= longPressMs) {
      pendingPageForward = true;
      currentStepIndex = -1;
      return true;
    }
    skipDirection = 1;
    if (currentStepIndex < 0) {
      currentStepIndex = 0;  // activate; render lands on the first translated step
      return true;
    }
    if (currentStepIndex < stepCount - 1) {
      currentStepIndex++;
      return true;
    }
    // At last step.
    if (pageTurnMode) {
      pendingPageForward = true;
      activateOnNextPage = true;
      currentStepIndex = -1;
    } else {
      // Loop: wrap to first.
      currentStepIndex = 0;
    }
    return true;
  }

  // Back button.
  if (input.wasReleased(backBtn)) {
    // Long press: turn page backward (like non-tooltip mode), dismiss tooltip.
    if (input.getHeldTime() >= longPressMs) {
      pendingPageBack = true;
      currentStepIndex = -1;
      return true;
    }
    skipDirection = -1;
    if (currentStepIndex < 0) {
      currentStepIndex = 0;  // activate; render lands on the first translated step
      return true;
    }
    if (currentStepIndex > 0) {
      currentStepIndex--;
      return true;
    }
    // At first step.
    if (pageTurnMode) {
      pendingPageBack = true;
      activateOnNextPage = true;
      currentStepIndex = -1;
    } else {
      // Loop: wrap to last.
      currentStepIndex = std::max(0, stepCount - 1);
    }
    return true;
  }

  // ESC/Back button: dismiss tooltip if active.
  if (input.wasReleased(MappedInputManager::Button::Back)) {
    if (currentStepIndex >= 0) {
      currentStepIndex = -1;
      return true;
    }
    return false;
  }

  return false;
}

void TooltipOverlay::onPageChanged() {
  bool shouldActivate = activateOnNextPage;
  int8_t dir = skipDirection;
  currentStepIndex = -1;
  skipDirection = 1;
  pagePrepared = false;
  origWordCount = 0;
  sentenceTranslations.clear();
  splits.count = 0;
  stepCount = 0;
  activateOnNextPage = false;
  activateFromEnd = false;
  pendingPageForward = false;
  pendingPageBack = false;

  // After a tooltip-triggered page turn, auto-activate on the new page.
  if (shouldActivate) {
    currentStepIndex = 0;  // preparePage will run; activateFromEnd picks the end step for a back turn
    skipDirection = dir;
    activateFromEnd = (dir < 0);  // going back → show last step
  }
}

// ── Paragraph data: one shared walker over the chapter HTML ──────────────────
//
// This overlay no longer carries its own copy of ChapterHtmlSlimParser's paragraph counter. The
// pairs come from lingua::matchPageParagraphs, matched to the page's paragraphs by text, so nothing
// here can drift from the parser. See TranslationPairWalker.h.

struct SentEntry {
  std::string key;          // first 6 words (normalized) of original sentence
  std::string translation;  // mapped translation text
};

// Forward declaration — builds index entries for one paragraph pair.
static void addPairToIndex(const std::string& origText, const std::string& transText, SentencePairScratch& scratch,
                           std::vector<SentEntry>& index);

// ── Sentence index: map original sentences to translations ───────────────────

// Split text into whitespace-delimited words. Separators are recognized via the shared
// textnorm::whitespaceLenAt SSOT — the SAME set the char-offset SentenceSplitter fix and
// foldForMatch use — so ASCII space/tab/CR/LF AND Unicode NBSP-style separators (U+00A0,
// U+2000..200A, U+202F, U+205F) all break a token. chOnChar keeps raw NBSP in the accumulated
// text; splitting on ASCII whitespace alone (the old behavior) left an NBSP-joined pair as ONE
// token, so classifyTerminator — which only sees a terminator at the END of a token — never saw
// the interior "." and undercounted the sentence, shifting every tooltip mapping by one (gap B).
// Splitting on whitespaceLenAt guarantees no token can contain an inter-word separator, so a
// terminator immediately before an NBSP now lands at a token boundary and is recognized.
static std::vector<std::string> tokenizeWords(const std::string& text) {
  std::vector<std::string> words;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    int wl;
    while (i < n && (wl = textnorm::whitespaceLenAt(text, i)) > 0) i += static_cast<size_t>(wl);
    if (i >= n) break;
    const size_t start = i;
    while (i < n && textnorm::whitespaceLenAt(text, i) == 0) i++;
    words.emplace_back(text.data() + start, i - start);
  }
  return words;
}

// Process one (original, translation) paragraph pair: split both into sentences, map by
// character-midpoint, and add entries to the index. Called during HTML parsing — only one pair in
// memory at a time, and the aligner's fixed scratch is the caller's (ParseCtx), so this frame stays
// small.
//
// The alignment RULE itself lives in SentencePairing (shared with LinguaLayout::Interlinear and
// host-tested); this function is now just tokenize -> align -> materialize the index strings.
static void addPairToIndex(const std::string& origText, const std::string& transText, SentencePairScratch& scratch,
                           std::vector<SentEntry>& index) {
  auto origWords = tokenizeWords(origText);
  auto transWords = tokenizeWords(transText);
  if (origWords.empty() || transWords.empty()) return;

  std::vector<const char*> origPtrs, transPtrs;
  origPtrs.reserve(origWords.size());
  transPtrs.reserve(transWords.size());
  for (auto& w : origWords) origPtrs.push_back(w.c_str());
  for (auto& w : transWords) transPtrs.push_back(w.c_str());

  if (!splitSentencePair(origPtrs.data(), (int)origPtrs.size(), transPtrs.data(), (int)transPtrs.size(), scratch)) {
    return;
  }
  // No junk merge here, deliberately: the index is keyed per SOURCE sentence and preparePage merges
  // the PAGE's sentences before looking anything up, so merging here too would drop keys the page
  // still asks for.
  mapSentenceSpans(origPtrs.data(), transPtrs.data(), scratch);

  for (int os = 0; os < scratch.origSplits.count; os++) {
    // One contiguous span == exactly the string the previous per-sentence joining produced (see
    // mapSentenceSpans): splitSentences' spans abut, so joining the merged span is the same text.
    std::string trans = joinSpan(transWords, scratch.transFor[os]);
    std::string key =
        sentenceKey(origPtrs.data(), scratch.origSplits.spans[os].startWord, scratch.origSplits.spans[os].endWord);
    if (!key.empty() && !trans.empty()) {
      index.push_back({std::move(key), std::move(trans)});
    }
  }
}

// Strip dots and collapse spaces; the same fold for index keys and page keys.
static std::string normalizeSentenceKey(const std::string& key) {
  std::string out;
  out.reserve(key.size());
  for (char c : key) {
    if (c == '.') continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Parse HTML and build sentence index in one pass — no intermediate storage. Only paragraphs in
// [wantFirst, wantLast] (the current page's range) get index entries, so peak RAM is bounded by one
// page's worth of translations rather than the whole chapter.
// Build the page's sentence index from the pairs behind the paragraphs it shows. `visibleTexts` are
// those paragraphs in reading order; the walker matches each to its pair by text, so this overlay
// needs no paragraph numbering of its own.
static std::vector<SentEntry> buildIndexForPage(const std::string& cachePath, const std::string& htmlPath,
                                                const std::string& bookPrimaryLang,
                                                const std::vector<std::string>& visibleTexts) {
  std::vector<SentEntry> index;
  if (visibleTexts.empty()) return index;

  std::vector<lingua::PageMatch> matches(visibleTexts.size());
  if (!lingua::matchPageParagraphsCached(cachePath, htmlPath, bookPrimaryLang, visibleTexts.data(),
                                         static_cast<int>(visibleTexts.size()), matches.data())) {
    LOG_DBG("TIP", "Cannot read translation source %s", htmlPath.c_str());
    return index;
  }

  // One aligner scratch (~800 B) for the whole page, not one per pair and not on the stack:
  // SentencePairScratch is far past the 256-byte local budget.
  const auto scratch = makeUniqueNoThrow<SentencePairScratch>();
  if (!scratch) {
    LOG_ERR("TIP", "OOM: sentence pairing scratch");
    return index;
  }

  int pairCount = 0;
  for (const auto& match : matches) {
    if (!match.matched || match.original.empty() || match.translation.empty()) continue;
    addPairToIndex(match.original, match.translation, *scratch, index);
    pairCount++;
  }

  LOG_DBG("TIP", "Index: %d entries from %d pair(s) for %d paragraph(s)", (int)index.size(), pairCount,
          (int)visibleTexts.size());
  return index;
}

// ── Page preparation ──────────────────────────────────────────────────────────

void TooltipOverlay::collectPageGlyphText(const Page& page, std::string& out) {
  preparePage(page);  // idempotent (pagePrepared guard); render() will find it already done
  size_t need = 0;
  for (const auto& s : sentenceTranslations) need += s.size() + 1;
  const char* const marker = tr(STR_NO_TRANSLATION);
  need += std::strlen(marker) + 1;
  out.reserve(out.size() + need);
  for (const auto& s : sentenceTranslations) {
    out += s;
    out += ' ';
  }
  out += marker;  // shown for any sentence with no mapped translation (Option C)
}

void TooltipOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  sentenceTranslations.clear();
  stepCount = 0;  // stays 0 on any early return below (no paragraph indices / empty index)

  // 1. Collect original words from page, REMEMBERING WHERE EACH PARAGRAPH STARTS. v2 flattened
  //    TextBlock word storage: iterate by index (wordCount/wordText), NOT the fork's getWords()
  //    container. wordText(i) returns a NUL-terminated pointer into the block's arena, stable for
  //    the block's (and page's) lifetime, which spans every render() call between page changes.
  //
  //    The paragraph runs are not decoration: step 3 builds the index one entry per PARAGRAPH pair,
  //    so the page side has to be cut at the same places or the two sides count differently. Lines
  //    of one paragraph are contiguous and carry its index (PageLine::paragraphIdx, serialized with
  //    the page), so a change of index opens a new run — the same grouping PageTranslationOverlay
  //    does. An old cache leaves every index at -1, which collapses to a single run and reproduces
  //    the previous flat behaviour rather than mis-cutting.
  //
  //    More than MAX_SENTENCES runs cannot produce distinguishable sentences anyway (every run
  //    yields at least one, and the split budget is MAX_SENTENCES), so the table is capped there.
  uint16_t paraRunStarts[MAX_SENTENCES];
  int paraRunCount = 0;
  // One joined text per run, for the walker (step 3). A page shows a handful of paragraphs.
  std::vector<std::string> visibleTexts;
  visibleTexts.reserve(8);
  lingua::PageTextJoiner joiner;
  // How each page word continues the previous one in the source, for the page-side sentence keys
  // (step 4). Transient; without it the keys fall back to plain space-joined words.
  const auto origWordGlue = makeUniqueNoThrow<WordGlue[]>(MAX_WORDS);
  if (!origWordGlue) LOG_ERR("TIP", "OOM: word glue (%d bytes)", MAX_WORDS);
  int16_t lastParagraphIdx = -1;
  bool haveRun = false;  // the first line always opens a run, whatever its index
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    if (!haveRun || line->paragraphIdx != lastParagraphIdx) {
      haveRun = true;
      if (paraRunCount < MAX_SENTENCES) {
        paraRunStarts[paraRunCount++] = static_cast<uint16_t>(origWordCount);
        visibleTexts.emplace_back();
        joiner.reset();
      }
      lastParagraphIdx = line->paragraphIdx;
    }
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (origWordCount < MAX_WORDS) {
        const WordGlue glue = joiner.append(visibleTexts.back(), *block, i);
        if (origWordGlue) origWordGlue[origWordCount] = glue;
        origWordPtrs[origWordCount++] = block->wordText(i);
      }
    }
  }

  // 2. Split into sentences — never across a paragraph edge — then merge any "empty" sentences
  //    (dots, fragments) into the previous sentence so the user doesn't click through junk. The
  //    merge gets the same run table: it folds BACKWARDS, so without it a junk-only paragraph (a
  //    bare numeral heading) would swallow the paragraph before it and undo the split.
  splits = splitSentencesByParagraph(origWordPtrs, origWordCount, paraRunStarts, paraRunCount);
  mergeJunkSentences(splits, origWordPtrs, paraRunStarts, paraRunCount);
  LOG_DBG("TIP", "Page: %d words, %d paragraphs, %d sentences (after merge)", origWordCount, paraRunCount,
          splits.count);

  // 3. Build the sentence index from the pairs behind the paragraphs this page shows. The runs above
  //    already cut the page at paragraph edges, and step 1 joined each run's words (source-glued
  //    pieces without a space, see PageTextJoiner) into exactly the texts the walker matches against
  //    -- and only this page's pairs are ever held.
  auto index = buildIndexForPage(pairCachePath, translationSourcePath, bookPrimaryLang, visibleTexts);
  if (index.empty()) {
    LOG_DBG("TIP", "No index entries for this page");
    return;
  }

  // 4. Match each page sentence against the index by key, then gap-fill neighbors.
  sentenceTranslations.resize(splits.count);
  std::vector<int> matchedIdx(splits.count, -1);
  int matched = 0, lastIdx = -1;

  // Pre-normalize index keys (strip dots, collapse spaces).
  std::vector<std::string> normKeys(index.size());
  for (int j = 0; j < (int)index.size(); j++) normKeys[j] = normalizeSentenceKey(index[j].key);

  // The page sentences that carry matchable text, in order. A sentence whose key is EMPTY has none:
  // sentenceKey drops single-character punctuation words, so a scene-break ornament paragraph ("*",
  // "◆") keys to nothing. Cutting the page at paragraph edges now keeps such a paragraph as a page
  // sentence of its own instead of folding it into a neighbour, so it has to be excluded from the
  // gap fill as well as from the match — otherwise it inherits a NEIGHBOUR's translation and becomes
  // a navigation step that underlines a stray glyph. Left unfilled, its translation stays empty and
  // groupTranslationSteps drops it from the step list: no step, no underline.
  //
  // The fill must still CHAIN ACROSS it, which is why this is a list of the keyed sentences rather
  // than a skip flag: the passes below take each keyed sentence's keyed neighbour. On a page with no
  // keyless sentence — every page of the sample book — this is 0,1,2,… and the two passes are
  // exactly the neighbour fills they have always been.
  uint8_t keyed[MAX_SENTENCES];
  int keyedCount = 0;

  // Forward pass: match by key.
  for (int s = 0; s < splits.count; s++) {
    std::string pk =
        sentenceKeyGlued(origWordPtrs, origWordGlue.get(), splits.spans[s].startWord, splits.spans[s].endWord);
    if (pk.empty()) continue;
    if (keyedCount < MAX_SENTENCES) keyed[keyedCount++] = static_cast<uint8_t>(s);
    const std::string np = normalizeSentenceKey(pk);

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
        if (np.compare(0, cl, normKeys[j], 0, cl) == 0 && cl > bestLen) {
          bestLen = cl;
          foundIdx = j;
        }
      }
    }
    if (foundIdx >= 0) {
      sentenceTranslations[s] = index[foundIdx].translation;
      matchedIdx[s] = foundIdx;
      lastIdx = foundIdx;
      matched++;
    }
  }

  // Gap fill: infer unmatched sentences from their KEYED neighbors (see `keyed` above).
  // Backward: if the next keyed sentence matched at idx N, this one gets idx N-1.
  for (int k = keyedCount - 2; k >= 0; k--) {
    const int s = keyed[k], next = keyed[k + 1];
    if (matchedIdx[s] >= 0) continue;
    if (matchedIdx[next] > 0) {
      int idx = matchedIdx[next] - 1;
      sentenceTranslations[s] = index[idx].translation;
      matchedIdx[s] = idx;
      matched++;
    }
  }
  // Forward: if the previous keyed sentence matched at idx N, this one gets idx N+1.
  for (int k = 1; k < keyedCount; k++) {
    const int s = keyed[k], prev = keyed[k - 1];
    if (matchedIdx[s] >= 0) continue;
    if (matchedIdx[prev] >= 0 && matchedIdx[prev] + 1 < (int)index.size()) {
      int idx = matchedIdx[prev] + 1;
      sentenceTranslations[s] = index[idx].translation;
      matchedIdx[s] = idx;
      matched++;
    }
  }

  LOG_DBG("TIP", "Matched %d/%d sentences (incl gap-fill)", matched, splits.count);

  // 5. Collapse consecutive source sentences sharing one translated sentence into a
  //    single navigation step (fixes the "same translation shown on each of the merged
  //    source sentences" bug). Steps drive both navigation and the underline span.
  stepCount = groupTranslationSteps(sentenceTranslations, steps, MAX_SENTENCES);
  LOG_DBG("TIP", "Steps: %d translation units (from %d sentences)", stepCount, splits.count);
}

// ── Sentence bounds and underline ─────────────────────────────────────────────

TooltipOverlay::SentenceBounds TooltipOverlay::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                  int fontId, int xOffset, int yOffset) const {
  (void)fontId;
  SentenceBounds bounds = {0, 0, 0};
  int idx = 0;
  bool found = false;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (idx >= span.startWord && idx < span.endWord) {
        int wx = block->wordXpos(i) + line->xPos + xOffset;
        int wy = line->yPos + yOffset;
        if (!found) {
          bounds.firstLineY = wy;
          bounds.startX = wx;
          bounds.endX = wx;
          found = true;
        }
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
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (idx >= span.startWord && idx < span.endWord) {
        int wx = block->wordXpos(i) + line->xPos + xOffset;
        int wy = line->yPos + yOffset;
        // Width measured with the word's actual style so the underline matches the rendered glyph
        // advance (the fork masked to font-variant bits; v2's getTextWidth accepts the full Style
        // and getFont() ignores decoration/translation bits above bit 1).
        int ww = renderer.getTextWidth(fontId, block->wordText(i), block->wordStyle(i));
        if (wy != curY) {
          if (curY >= 0) renderer.drawLine(sx, curY + ulOff, ex, curY + ulOff, true);
          curY = wy;
          sx = wx;
          ex = wx + ww;
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
    const auto* line = static_cast<const PageLine*>(el.get());
    const uint16_t n = line->getBlock()->wordCount();
    for (uint16_t i = 0; i < n; i++) {
      if (idx >= span.startWord && idx < span.endWord) lastY = line->yPos + yOffset;
      idx++;
    }
  }
  return lastY;
}

// Byte length of the UTF-8 code point starting at s[0], clamped to maxLen. Used so a mid-word hard
// break never splits a multi-byte sequence (which would draw a garbage glyph / fault the decoder).
static int utf8CharLen(const char* s, int maxLen) {
  if (maxLen <= 0) return 0;
  const unsigned char c = static_cast<unsigned char>(s[0]);
  int n;
  if ((c & 0x80) == 0)
    n = 1;
  else if ((c & 0xE0) == 0xC0)
    n = 2;
  else if ((c & 0xF0) == 0xE0)
    n = 3;
  else if ((c & 0xF8) == 0xF0)
    n = 4;
  else
    n = 1;  // stray continuation / invalid lead byte — advance one byte
  return n > maxLen ? maxLen : n;
}

// Largest UTF-8-safe byte prefix of s[0..len) whose rendered width (measured with the SAME
// getTextWidth call the draw path uses) is <= avail. Returns 0 if not even one code point fits.
static int fitPrefixBytes(GfxRenderer& renderer, int fontId, const char* s, int len, int avail) {
  char buf[512];
  int fit = 0;
  int i = 0;
  while (i < len) {
    const int cl = utf8CharLen(s + i, len - i);
    const int cand = i + cl;
    if (cand > static_cast<int>(sizeof(buf)) - 1) break;
    memcpy(buf, s, cand);
    buf[cand] = '\0';
    if (renderer.getTextWidth(fontId, buf) > avail) break;
    fit = cand;
    i = cand;
  }
  return fit;
}

// A single wrapped tooltip line: a [start, len) byte range into the tooltip text. The draw path
// renders EXACTLY these ranges, and each was accepted by measuring that exact substring — so the
// wrapped line count and the drawn line count are structurally identical (no height slack needed).
struct TipLine {
  const char* start;
  int len;
};

// Wrap `text` into lines no wider than `avail` px, measuring each candidate line with
// renderer.getTextWidth on the EXACT substring that will later be drawn (never word-widths +
// getSpaceWidth, whose rounding vs. the real space-glyph advance was the old height-estimate bug).
// A word longer than `avail` on its own is hard-broken mid-word on a UTF-8 boundary — the same clip
// the draw path then renders. Fills at most `maxLines`; returns the line count produced.
static int wrapTooltipLines(GfxRenderer& renderer, int fontId, const char* text, int avail, TipLine* out,
                            int maxLines) {
  if (avail < 1) avail = 1;
  char buf[512];
  int count = 0;
  const char* p = text;
  while (*p && count < maxLines) {
    while (*p == ' ') p++;  // strip leading spaces; lines never begin with a space
    if (!*p) break;
    const char* lineStart = p;
    int lineLen = 0;  // committed bytes on this line, measured from lineStart (no trailing space)
    while (*p) {
      const char* wordStart = p;
      while (*p && *p != ' ') p++;  // p now at end of this word
      int cand = static_cast<int>(p - lineStart);
      int measLen = cand < static_cast<int>(sizeof(buf)) - 1 ? cand : static_cast<int>(sizeof(buf)) - 1;
      memcpy(buf, lineStart, measLen);
      buf[measLen] = '\0';
      if (renderer.getTextWidth(fontId, buf) <= avail) {
        lineLen = cand;         // whole run up to here fits; commit through this word
        while (*p == ' ') p++;  // consume the separating spaces, then try the next word
        continue;
      }
      if (lineLen > 0) {
        p = wordStart;  // this word overflows; break the line before it (its spaces already skipped)
        break;
      }
      // First word alone exceeds avail: hard-break it mid-word so the line (and box) can't overflow.
      const int wordLen = static_cast<int>(p - wordStart);
      int fit = fitPrefixBytes(renderer, fontId, wordStart, wordLen, avail);
      if (fit <= 0) fit = utf8CharLen(wordStart, wordLen);  // guarantee forward progress
      lineLen = fit;
      p = wordStart + fit;  // remainder wraps onto the next line
      break;
    }
    out[count].start = lineStart;
    out[count].len = lineLen;
    count++;
  }
  return count;
}

void TooltipOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset,
                            int yOffset, int viewportWidth, int viewportHeight) {
  if (currentStepIndex < 0) return;

  preparePage(page);

  // After a back page-turn: land on the LAST step now that steps are populated.
  if (activateFromEnd) {
    currentStepIndex = stepCount > 0 ? stepCount - 1 : 0;
    activateFromEnd = false;
  }

  // Clamp a stale index — stepCount can shrink relative to the page this index was set on.
  if (stepCount > 0 && currentStepIndex >= stepCount) currentStepIndex = stepCount - 1;

  // Choose the underline span and the tooltip text for the current step. A step groups one
  // or more CONSECUTIVE source sentences that share ONE translation (the merged-sentence
  // case), so the span covers ALL of them (spans are a contiguous partition, so the union is
  // simply [first.startWord, last.endWord)) and the translation is drawn exactly ONCE.
  bool dim = false;
  SentenceSpan span{};
  const char* text = nullptr;
  if (stepCount == 0) {
    // Nothing translated on this page (Option C parity with PageTranslationOverlay): the source still
    // shows through, so surface the marker over the first source sentence rather than a blank
    // popup. Never triggers for a correctly-translated book. No sentences at all → nothing to do.
    if (splits.count == 0) return;
    span = splits.spans[0];
    text = tr(STR_NO_TRANSLATION);
    dim = true;
  } else {
    const TooltipStep& st = steps[currentStepIndex];
    span.startWord = splits.spans[st.firstSentence].startWord;
    span.endWord = splits.spans[st.lastSentence].endWord;
    text = sentenceTranslations[st.firstSentence].c_str();
    if (!text || text[0] == '\0') {  // defensive: a step always carries a non-empty translation
      text = tr(STR_NO_TRANSLATION);
      dim = true;
    }
  }
  LOG_DBG("TIP", "Drawing step %d/%d%s: '%.80s'", currentStepIndex, stepCount, dim ? " (marker)" : "", text);

  const int lh = renderer.getLineHeight(fontId);
  auto bounds = findSentenceBounds(page, span, fontId, xOffset, yOffset);
  if (bounds.firstLineY == 0 && bounds.startX == 0) return;
  const int lastY = findLastLineY(page, span, yOffset);

  constexpr int PAD = 6, RAD = 3, GAP = 4;
  const int maxW = viewportWidth - 2 * PAD;
  const int tlh = renderer.getLineHeight(tooltipFontId);

  // Exact sizing: wrap ONCE into the concrete line list the draw loop below will render, measuring
  // each line with the SAME renderer.getTextWidth call the draw uses. Box height is then exactly
  // lineCount * lineHeight + padding — no "+1" slack line. The old code sized from
  // ceil(getTextWidth(wholeText) / avail) + 1, a DIFFERENT measurement than the word-by-word draw
  // (word widths + getSpaceWidth), so it reserved a phantom empty line to hide the mismatch. Wrapping
  // is done at the FULL-width text area (maxW - 2*PAD): that keeps the SAME line list valid whether
  // the box is later shrunk to a single line or clamped up/down near a screen edge — the wrap width
  // never depends on the final tipY/tipW.
  const int maxAvail = maxW - 2 * PAD;
  static constexpr int MAX_TIP_LINES = 30;  // exceeds any box that fits within 40% of the tallest viewport
  TipLine lines[MAX_TIP_LINES];
  int lineCount = wrapTooltipLines(renderer, tooltipFontId, text, maxAvail, lines, MAX_TIP_LINES);
  if (lineCount < 1) {
    // Degenerate (empty / all-whitespace) text — draw a minimal one-line box, never read lines[0]
    // uninitialized. In practice `text` is a non-blank translation or the STR_NO_TRANSLATION marker.
    lines[0].start = text;
    lines[0].len = 0;
    lineCount = 1;
  }

  int maxL = (viewportHeight * 4 / 10) / tlh;
  if (maxL < 1) maxL = 1;
  const int nLines = lineCount > maxL ? maxL : lineCount;  // clamp tall tooltips; excess text is clipped

  int tipW;
  if (nLines == 1) {
    // Single line: shrink the box to that line's real drawn width (measured as it will be drawn).
    char lb[512];
    const int cl = std::min(lines[0].len, 511);
    memcpy(lb, lines[0].start, cl);
    lb[cl] = '\0';
    tipW = renderer.getTextWidth(tooltipFontId, lb) + 2 * PAD;
    if (tipW > maxW) tipW = maxW;
  } else {
    tipW = maxW;
  }
  const int tipH = nLines * tlh + 2 * PAD;

  int tipX = xOffset + PAD;
  int tipY = (tipH + GAP <= bounds.firstLineY - yOffset) ? bounds.firstLineY - GAP - tipH : lastY + lh + GAP;
  if (tipY < yOffset + PAD) tipY = yOffset + PAD;
  if (tipY + tipH > yOffset + viewportHeight - PAD) tipY = yOffset + viewportHeight - PAD - tipH;

  renderer.fillRect(tipX - 1, tipY - 1, tipW + 2, tipH + 2, false);
  renderer.drawRoundedRect(tipX, tipY, tipW, tipH, 1, RAD, true);

  // Draw the precomputed lines verbatim — the SAME [start, len) ranges the wrap measured, so what is
  // drawn is exactly what was sized (the invariant is structural, not padded). The fork dimmed the
  // "not translated" marker via drawText's grayLevel arg; v2's drawText has no grayLevel parameter
  // and the panel is monochrome, so the marker renders as ordinary black text (the fork's grayLevel=1
  // also fell back to black on pure-BW panels).
  int textY = tipY + PAD;
  for (int i = 0; i < nLines; i++) {
    if (lines[i].len > 0) {
      char lb[512];
      const int cl = std::min(lines[i].len, 511);
      memcpy(lb, lines[i].start, cl);
      lb[cl] = '\0';
      renderer.drawText(tooltipFontId, tipX + PAD, textY, lb, true, EpdFontFamily::REGULAR);
    }
    textY += tlh;
  }

  drawSentenceUnderline(renderer, page, span, fontId, xOffset, yOffset);
}

// ── Font helper ───────────────────────────────────────────────────────────────
//
// Tooltip text renders in the reader's own family, at the size the Lingua submenu's Translation Size
// row selects: Same = the body font, Smaller = one step down the family's point-size ladder, so the
// popup translation reads as a secondary annotation below the source. Adapted to v2's font set
// (EDSLAB / NOTOSANS + SD card fonts); the fork's family switch offered a wider built-in lineup
// (Bookerly/EdsLab/Caecilia/GPro), of which v2 ships only EdsLab.
//
// Reader size is a point size since 91900484 (CrossPointSettings::fontPointSize), not a
// SMALL/MEDIUM/LARGE slot, so "one smaller" means the previous entry in the active family's
// selectable point sizes rather than an enum decrement.
//
// The size is read through CrossPointSettings::getTooltipTranslationFontId() — the TOOLTIP's own
// stored size, not a value shared with the inline modes: the tooltip is composited over a finished
// page, so its size must not be able to re-break a line of the book (see TRANSLATION_SIZE). It
// returns 0 both for SIZE_SAME and for a SIZE_SMALLER that cannot be honoured, which is every SD
// family (the manager keeps exactly ONE reader-size face loaded and SdCardFontSystem::resolveFontId()
// ignores its pointSize argument by design, so there is no smaller SD face to drop to) and a
// built-in already at the smallest point size its family ships — which is also why the Lingua row
// reads Same and refuses to cycle there. Reading the setting here rather than smallerReaderFontId()
// directly is what makes the row mean something; before the row existed this stepped down
// unconditionally, which is exactly why tooltipTranslationSize defaults to SIZE_SMALLER.
//
// 0 becomes getReaderFontId() — the body font — which also keeps renderOverlayFrame's
// overlayFontId == fontId fast path (one shared prewarm for page + overlay) intact for SIZE_SAME.

int getTooltipFontId() {
  const int translationFontId = SETTINGS.getTooltipTranslationFontId();
  return translationFontId != 0 ? translationFontId : SETTINGS.getReaderFontId();
}
