#include "TooltipOverlay.h"

#include <CrossPointSettings.h>
#include <Epub/parsers/ParagraphBoundary.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "TextNormalize.h"
#include "fontIds.h"

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

// ── Chapter parsing: extract (orig, trans) paragraph pairs ───────────────────
//
// The paragraph-boundary rule (container block tags + <br/>) is NOT redefined
// here: this reparse and ChapterHtmlSlimParser's layout counter both call the
// shared paraboundary predicate (ParagraphBoundary.h) so their paragraph indices
// cannot diverge by a tag. See that header for the full contract.

struct SentEntry {
  std::string key;          // first 6 words (normalized) of original sentence
  std::string translation;  // mapped translation text
};

// Forward declaration — builds index entries for one paragraph pair.
static void addPairToIndex(const std::string& origText, const std::string& transText, std::vector<SentEntry>& index);

struct ParseCtx {
  std::vector<SentEntry>* index;
  int blockDepth = 0;
  bool inBlock = false;
  bool isTranslation = false;
  // >0 while inside a subtree the layout parser SKIPS after emitting a single synthetic paragraph —
  // a <table> ("[Table omitted]") or an undecodable <img> with alt text ("[Image: …]"). Mirrors
  // ChapterHtmlSlimParser::skipUntilDepth: nested starts bump it, nested ends drop it, the matching
  // close returns it to 0.
  int skipDepth = 0;
  // The innermost open block is an <li>. ChapterHtmlSlimParser seeds every <li> block with a
  // U+2022 bullet, so an <li> whose only content so far is that implicit bullet still flushes a
  // (bullet-only) paragraph when its first child block opens.
  bool currentBlockIsLi = false;
  std::string currentText;
  std::string lastOrigText;
  bool hasLastOrig = false;
  int pairCount = 0;
  // Selective parse (mirrors ModalOverlay): only build index entries for paragraphs the current
  // page actually shows. Without this the index holds every translated sentence in the chapter,
  // which exhausts the heap on long chapters and reboots the device.
  int wantFirst = 0;
  int wantLast = 0;
  int paragraphCounter = -1;    // pre-increment on each ORIGINAL block; first original becomes idx 0
  int lastOrigIdx = -1;         // paragraph index of the most recent original block
  XML_Parser parser = nullptr;  // for XML_StopParser once we walk past the page
};

// Finish the current ORIGINAL paragraph: trim it, and (if non-empty) advance the
// paragraph counter and retain the text for pairing when it falls on the page.
// Whitespace-only text is dropped without incrementing — matching
// ChapterHtmlSlimParser, which skips empty blocks. Factored out so <br/> handling
// in chOnStart can reuse it (flush-in-place) as well as the block-close in chOnEnd.
static void flushOriginalParagraph(ParseCtx* ctx) {
  auto& t = ctx->currentText;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (t.empty()) {
    ctx->currentText.clear();
    return;
  }
  // Bump the paragraph counter, then decide whether it falls on the page.
  const int idx = ++ctx->paragraphCounter;
  ctx->lastOrigIdx = idx;
  if (idx > ctx->wantLast) {
    // Past the page (and the prior paragraph's translation has already been processed) — stop
    // the parser so expat doesn't scan the rest of the chapter. Returns XML_ERROR_ABORTED,
    // which parseAndBuildIndex treats as the success path.
    if (ctx->parser) XML_StopParser(ctx->parser, XML_FALSE);
    ctx->lastOrigText.clear();
    ctx->hasLastOrig = false;
    ctx->currentText.clear();
    return;
  }
  if (idx < ctx->wantFirst) {
    ctx->lastOrigText.clear();  // before the page — discard, never pairs
    ctx->hasLastOrig = false;
    ctx->currentText.clear();
    return;
  }
  ctx->lastOrigText = std::move(t);
  ctx->hasLastOrig = true;
  ctx->currentText.clear();
}

// True if the accumulated block text holds any non-whitespace character. Used to tell an <li>
// that has only its implicit bullet (empty currentText) from one that has real direct text.
static bool hasVisibleText(const std::string& s) {
  for (char c : s) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return true;
  }
  return false;
}

// ChapterHtmlSlimParser seeds EVERY <li> block with a U+2022 bullet (addWord), so an <li> with no
// real direct text — empty, or holding only a media/table child (<li></li>, <li><img/></li>,
// <li><table>…</table></li>) — is still a NON-empty block it counts as one (bullet-only) paragraph.
// Seed the same bullet just before ANY flush of the current block so the reparser counts it too;
// a no-op when the block is not an <li> or already has real text.
static void seedLiBulletIfEmpty(ParseCtx* ctx) {
  if (ctx->currentBlockIsLi && !hasVisibleText(ctx->currentText)) {
    ctx->currentText = "\xe2\x80\xa2";
  }
}

// Mirror ChapterHtmlSlimParser's on-device decoder gate: only .jpg/.jpeg/.png images decode
// (ImageDecoderFactory). Any other extension — or a missing src — makes the layout parser fall
// back to the image's alt text, which it emits as exactly ONE "[Image: …]" paragraph. Replicating
// the extension test lets the reparser count that fallback WITHOUT counting a decodable image
// (which produces no paragraph). NOTE: a .jpg/.png that fails to decode at RUNTIME (corrupt data,
// dimension read failure) still falls back in the layout parser but is treated as decodable here —
// that residual case is not detectable from the HTML alone.
static bool imgSrcDecodable(const char* src) {
  if (!src || !*src) return false;
  const char* dot = strrchr(src, '.');
  if (!dot) return false;
  std::string ext(dot);
  for (auto& c : ext) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

static void XMLCALL chOnStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ParseCtx*>(ud);

  // Inside a skipped subtree (table cell / undecodable-image alt content): swallow every nested
  // element so it can never contribute a paragraph, exactly like ChapterHtmlSlimParser's
  // skipUntilDepth. Balanced by the matching drops in chOnEnd.
  if (ctx->skipDepth > 0) {
    ctx->skipDepth++;
    return;
  }

  // <br/> — a hard break is an EMPTY, NO-SCOPE element: it never opens a scope, so it must NEVER
  // touch blockDepth (chOnEnd skips its self-close too, keeping the count balanced in ALL cases,
  // including inside a translation block where a stray blockDepth++ would leave the <p> unclosed
  // and drop its translation). In an ORIGINAL block it ends the current paragraph IN PLACE (flush)
  // so the counter advances exactly as ChapterHtmlSlimParser's does on <br/>; in a TRANSLATION
  // block it is just an internal line break (keep accumulating).
  if (paraboundary::isHardBreak(name)) {
    if (ctx->inBlock && !ctx->isTranslation) flushOriginalParagraph(ctx);
    return;
  }

  // <table> — first flush the PRIOR block (a parent block's direct text for mixed content, or a
  // bullet-only <li>), then make "[Table omitted]" the RUNNING block. Crucially it is NOT flushed
  // now: the layout parser keeps filling the same block, so trailing text in the SAME container
  // (e.g. <div><table>…</table>tail</div>) stays in ONE paragraph — flushing it here would emit a
  // spurious extra paragraph. It flushes naturally at the next block-open or block-close. The
  // table subtree is swallowed via skipDepth. A top-level table (not yet inBlock) becomes its own
  // running block so it still flushes (at the next block or the enclosing structural close).
  if (strcmp(name, "table") == 0) {
    if (!ctx->isTranslation) {
      seedLiBulletIfEmpty(ctx);
      flushOriginalParagraph(ctx);  // counts the prior block (parent text / bullet-only li) if any
    }
    if (!ctx->inBlock) {
      ctx->inBlock = true;
      ctx->blockDepth = 1;
    }
    ctx->isTranslation = false;
    ctx->currentText = "[Table omitted]";
    ctx->currentBlockIsLi = false;  // the placeholder block is not an <li>
    ctx->skipDepth = 1;             // swallow cell content until the matching </table>
    return;
  }

  // <img> — a decodable image (.jpg/.jpeg/.png) renders with NO paragraph; an undecodable image
  // (any other src, or none) WITH alt text falls back to a single "[Image: …]" paragraph. Match
  // both so images never drift the count. An image is otherwise a no-scope empty element, so
  // (like <br/>) it must not touch blockDepth — chOnEnd treats <img> as a no-op.
  if (strcmp(name, "img") == 0) {
    const char* src = nullptr;
    const char* alt = nullptr;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0)
          src = atts[i + 1];
        else if (strcmp(atts[i], "alt") == 0)
          alt = atts[i + 1];
      }
    }
    if (!imgSrcDecodable(src) && alt && *alt) {
      // Same shape as <table>: flush the prior block (parent text / bullet-only li), then make
      // "[Image: …]" the RUNNING block (deferred flush) so trailing same-container text stays in
      // one paragraph. Swallow any image children via skipDepth.
      if (!ctx->isTranslation) {
        seedLiBulletIfEmpty(ctx);
        flushOriginalParagraph(ctx);
      }
      if (!ctx->inBlock) {
        ctx->inBlock = true;
        ctx->blockDepth = 1;
      }
      ctx->isTranslation = false;
      ctx->currentText = std::string("[Image: ") + alt + "]";
      ctx->currentBlockIsLi = false;
      ctx->skipDepth = 1;
    }
    return;  // decodable image / no alt: no paragraph, no scope
  }

  if (paraboundary::isContainerBlockTag(name)) {
    // A container opening while a block is ALREADY open (mixed content, or an <li> with a nested
    // block child): the layout parser first flushes the parent block's direct text as its own
    // counted paragraph when the child opens — and an <li> whose only content so far is its
    // implicit bullet flushes a bullet-only paragraph (the layout parser seeds every <li> with a
    // U+2022 bullet, so its block is non-empty). Do that flush BEFORE (re)starting the block; the
    // previous code cleared currentText here, discarding the parent text and dropping its count.
    //
    // We then start THIS container as a fresh block, re-detecting its lang, exactly like a
    // top-level block. Handling every container independently (rather than carrying outer nesting)
    // is what keeps a nested TRANSLATION block correctly recognized: our rewriter inserts the
    // translated paragraph INSIDE any wrapper <div>/<blockquote>, e.g.
    // <div><p>orig</p><p lang="fr">trad</p></div>. After the inner </p> the block is closed, so the
    // sibling <p lang="fr"> re-enters here and its lang IS detected — a nesting-depth counter that
    // stayed inside the div would miss it and drop the translation.
    if (ctx->inBlock && !ctx->isTranslation) {
      seedLiBulletIfEmpty(ctx);     // bullet-only <li> flushes a counted bullet paragraph here
      flushOriginalParagraph(ctx);  // counts the parent's direct text (or the li bullet) if present
    }
    ctx->inBlock = true;
    ctx->blockDepth = 1;
    ctx->isTranslation = false;
    ctx->currentBlockIsLi = (strcmp(name, "li") == 0);
    ctx->currentText.clear();
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0 ||
            strcmp(atts[i], "data-translation") == 0) {
          ctx->isTranslation = true;
        }
      }
    }
    return;
  }

  if (ctx->inBlock) {
    ctx->blockDepth++;  // inline element (span/b/i/…) inside a block — balanced by its end
  }
}

static void XMLCALL chOnEnd(void* ud, const XML_Char* name) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  // Leaving a skipped subtree (table cell / undecodable-image alt content). The matching close of
  // the <table>/<img> that opened the skip brings skipDepth back to 0. Mirrors skipUntilDepth.
  if (ctx->skipDepth > 0) {
    ctx->skipDepth--;
    return;
  }
  // <br/> is an empty element: expat fires this end event for its self-close.
  // It opened no scope (chOnStart handled it without touching blockDepth), so it
  // must not decrement here either — otherwise the enclosing block closes one tag
  // early and the text after <br/> is lost, desyncing the paragraph counter.
  if (paraboundary::isHardBreak(name)) return;
  // <img> is likewise a no-scope empty element on the decodable / no-alt path (the alt-text
  // fallback set skipDepth and was consumed above), so its self-close must not touch blockDepth.
  if (strcmp(name, "img") == 0) return;
  if (!ctx->inBlock) return;
  ctx->blockDepth--;
  if (ctx->blockDepth > 0) return;
  ctx->inBlock = false;

  if (ctx->isTranslation) {
    auto& t = ctx->currentText;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    // hasLastOrig is set only for in-range originals, so this naturally skips out-of-range pairs.
    if (!t.empty() && ctx->hasLastOrig) {
      // Process pair immediately — don't accumulate all pairs in memory.
      addPairToIndex(ctx->lastOrigText, t, *ctx->index);
      ctx->pairCount++;
      ctx->lastOrigText.clear();
      ctx->hasLastOrig = false;
    }
    ctx->currentText.clear();
  } else {
    seedLiBulletIfEmpty(ctx);  // empty / media-only <li> still counts one bullet paragraph
    flushOriginalParagraph(ctx);
  }
  // Block ended: no <li> context may leak to the next block-open (otherwise a following top-level
  // <table>/<img> would wrongly seed a bullet).
  ctx->currentBlockIsLi = false;
}

static void XMLCALL chOnChar(void* ud, const XML_Char* data, int len) {
  auto* ctx = static_cast<ParseCtx*>(ud);
  if (ctx->skipDepth > 0) return;  // text inside a skipped table cell / image subtree
  if (!ctx->inBlock) return;
  for (int i = 0; i < len; i++) {
    char c = data[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && !ctx->currentText.empty() && ctx->currentText.back() == ' ') continue;
    ctx->currentText += c;
  }
}

// ── Sentence index: map original sentences to translations ───────────────────

static inline bool isAsciiSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

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
    for (int j = 0; j < wlen; j++) {
      if (!isAsciiSpace(w[j])) {
        allSpace = false;
        break;
      }
    }
    if (allSpace) continue;
    // Skip single-char punctuation words (spaced ellipsis ".", stray punctuation)
    if (wlen == 1 && !((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z') || (w[0] >= '0' && w[0] <= '9') ||
                       (uint8_t)w[0] >= 0x80))
      continue;
    // Skip Unicode ellipsis … (U+2026 = E2 80 A6) — standalone or as entire word
    if (wlen == 3 && (uint8_t)w[0] == 0xE2 && (uint8_t)w[1] == 0x80 && (uint8_t)w[2] == 0xA6) continue;
    // Join hyphenated line-break fragments: "over-" + "whelming" → "overwhelming"
    if (wlen > 1 && w[wlen - 1] == '-' && i + 1 < end) {
      if (!key.empty()) key += ' ';
      key.append(w, wlen - 1);   // "over" (strip hyphen)
      key.append(words[i + 1]);  // + "whelming" → "overwhelming"
      i++;                       // skip the continuation word
      count++;
      continue;
    }
    // Strip leading em-space (U+2003 = E2 80 83)
    while (wlen >= 3 && (uint8_t)w[0] == 0xE2 && (uint8_t)w[1] == 0x80 && (uint8_t)w[2] == 0x83) {
      w += 3;
      wlen -= 3;
    }
    // Strip trailing Unicode ellipsis … (U+2026 = E2 80 A6) — e.g., "relief…" → "relief"
    while (wlen >= 3 && (uint8_t)w[wlen - 3] == 0xE2 && (uint8_t)w[wlen - 2] == 0x80 && (uint8_t)w[wlen - 1] == 0xA6) {
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
static void addPairToIndex(const std::string& origText, const std::string& transText, std::vector<SentEntry>& index) {
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
        if (dist < bestDist) {
          bestDist = dist;
          bestTs = ts;
        }
      }
      trans = joinSpan(transWords, transSplits.spans[bestTs]);
    }

    std::string key = sentenceKey(origPtrs.data(), origSplits.spans[os].startWord, origSplits.spans[os].endWord);
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

  // v2: all SD access goes through HalStorage (SdFat is not thread-safe). HalFile serializes every
  // read/seek/close via storageMutex and auto-closes in its destructor (DESTRUCTOR_CLOSES_FILE=1),
  // so no explicit close on any return path.
  HalFile f;
  if (!Storage.openFileForRead("TIP", path, f)) {
    LOG_ERR("TIP", "Cannot open %s", path.c_str());
    return index;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("TIP", "Failed to create expat parser");
    return index;
  }

  ParseCtx ctx;
  ctx.index = &index;
  ctx.wantFirst = wantFirst;
  ctx.wantLast = wantLast;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, chOnStart, chOnEnd);
  XML_SetCharacterDataHandler(parser, chOnChar);

  char buf[1024];
  bool done = false;
  bool stopped = false;
  while (!done && !stopped) {
    int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (n < 0) n = 0;  // read error — treat as end of stream rather than feeding expat a bad length
    done = (n < (int)sizeof(buf));
    if (XML_Parse(parser, buf, n, done) == XML_STATUS_ERROR) {
      // XML_StopParser() (we walked past the page) makes expat return ERROR with code
      // XML_ERROR_ABORTED — that's our success path, not a parse failure.
      if (XML_GetErrorCode(parser) == XML_ERROR_ABORTED) {
        stopped = true;
      } else {
        LOG_ERR("TIP", "XML parse error at line %lu", XML_GetCurrentLineNumber(parser));
      }
      break;
    }
  }
  XML_ParserFree(parser);

  LOG_DBG("TIP", "Index: %d entries from %d pairs in [%d..%d]%s", (int)index.size(), ctx.pairCount, wantFirst, wantLast,
          stopped ? " (early-stop)" : "");
  return index;
}

// ── Translation-unit grouping ─────────────────────────────────────────────────
//
// Collapse consecutive source sentences that display the SAME translation into one
// navigation step. Two mapping paths produce such duplicates:
//   • count mismatch — the engine merged K source sentences into ONE translated
//     sentence, so addPairToIndex maps each of the K source sentences (distinct keys)
//     to the SAME translated span, giving K identical translation strings;
//   • gap-fill — an unmatched sentence inherits a neighbor's translation.
// Either way the user would otherwise see the identical tooltip several times in a row
// (the photo-verified PT_TOOLTIP bug). Grouping steps by byte-identical, non-empty
// translation makes stepping advance per translation and lets the underline span every
// source sentence in the group. Empty (untranslated) sentences are never a step and
// break a run — a page-boundary partial sentence therefore stays out of a group's span.
//
// Pure: depends only on the translation strings, so the grouping rule is unit-testable
// on the host (see test/test_sentence_splitter). Returns the number of steps (<= maxSteps).
static int groupTranslationSteps(const std::vector<std::string>& sentenceTranslations, TooltipStep* out, int maxSteps) {
  const int total = static_cast<int>(sentenceTranslations.size());
  int count = 0;
  int i = 0;
  while (i < total && count < maxSteps) {
    if (sentenceTranslations[i].empty()) {
      i++;  // untranslated sentence: not a step, and it terminates any current run
      continue;
    }
    int j = i;
    while (j + 1 < total && !sentenceTranslations[j + 1].empty() &&
           sentenceTranslations[j + 1] == sentenceTranslations[i]) {
      j++;
    }
    out[count].firstSentence = static_cast<int16_t>(i);
    out[count].lastSentence = static_cast<int16_t>(j);
    count++;
    i = j + 1;
  }
  return count;
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

  // 1. Collect original words from page. v2 flattened TextBlock word storage: iterate by index
  //    (wordCount/wordText), NOT the fork's getWords() container. wordText(i) returns a
  //    NUL-terminated pointer into the block's arena, stable for the block's (and page's) lifetime,
  //    which spans every render() call between page changes.
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (origWordCount < MAX_WORDS) origWordPtrs[origWordCount++] = block->wordText(i);
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
    // Nothing translated on this page (Option C parity with ModalOverlay): the source still
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
// Tooltip text renders one size SMALLER than the reader body (upstream-fork parity) in the reader's
// own family, so the popup translation reads as a secondary annotation below the source. At the
// smallest reader size there is nothing smaller, so fall back to the reader font itself. Adapted to
// v2's font set (NOTOSERIF / NOTOSANS + SD card fonts); the fork's family switch used a different
// built-in font lineup (Bookerly/EdsLab/Caecilia/GPro).

int getTooltipFontId() {
  if (SETTINGS.fontSize <= CrossPointSettings::SMALL) return SETTINGS.getReaderFontId();
  const uint8_t sz = SETTINGS.fontSize - 1;

  // SD card font: resolve the smaller size through the same resolver the reader uses.
  if (SETTINGS.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
    const int id = SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, SETTINGS.sdFontFamilyName, sz);
    if (id != 0) return id;
    // Fall through to a built-in font if the SD font lacks this size.
  }

  switch (SETTINGS.fontFamily) {
    case CrossPointSettings::NOTOSANS:
      switch (sz) {
        case CrossPointSettings::SMALL:
          return NOTOSANS_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
          return NOTOSANS_14_FONT_ID;
        case CrossPointSettings::LARGE:
        default:
          return NOTOSANS_16_FONT_ID;
      }
    case CrossPointSettings::NOTOSERIF:
    default:
      switch (sz) {
        case CrossPointSettings::SMALL:
          return NOTOSERIF_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
          return NOTOSERIF_14_FONT_ID;
        case CrossPointSettings::LARGE:
        default:
          return NOTOSERIF_16_FONT_ID;
      }
  }
}
