#include "ModalOverlay.h"

#include <CrossPointSettings.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "TooltipOverlay.h"
#include "activities/translator/LanguagePickerActivity.h"
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

// Raw count of sentence terminators (.!? followed by space/end/closing-quote).
// Unlike countSentences(), does NOT clamp to a minimum of 1 — used by callers
// (e.g. countSentencesBefore) that need an honest zero when the slice contains
// no complete sentences.
static int countSentencesRaw(const std::string& text) {
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
                                          text[next] == ']' || (uint8_t)text[next] >= 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        count++;
      }
    }
  }
  return count;
}

// Sentence count for a paragraph. Clamped to ≥1 because every non-empty
// paragraph is at least one sentence even without an explicit terminator
// (used when sizing show/visible against origSentenceCount).
static int countSentences(const std::string& text) {
  return text.empty() ? 0 : std::max(1, countSentencesRaw(text));
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
                                          text[next] == ']' || (uint8_t)text[next] >= 0x80))
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
                                          text[next] == ']' || (uint8_t)text[next] >= 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        ends.push_back(next);
      }
    }
  }
  if ((int)ends.size() <= maxSentences) return text;
  // ends[K] = position after sentence K. To keep last N of T sentences,
  // we drop first (T-N) sentences, starting from position after sentence (T-N-1).
  int startFrom = ends[ends.size() - maxSentences - 1];
  // Skip leading space
  while (startFrom < (int)text.size() && text[startFrom] == ' ') startFrom++;
  return text.substr(startFrom);
}

// Collapse runs of whitespace into a single ASCII space, treating UTF-8 NBSP
// (U+00A0 = 0xC2 0xA0) as whitespace too. ChapterHtmlSlimParser swaps NBSP for
// ASCII space in page words, so visible text and origText (from expat, raw
// NBSP) would otherwise diverge byte-for-byte and `find` would miss. Same for
// soft hyphen (U+00AD = 0xC2 0xAD) — invisible in rendered words but raw in
// HTML. `limit < 0` means no cap (used for origText).
static std::string normalizeForMatch(const std::string& text, int limit) {
  std::string out;
  bool lastSp = true;
  for (size_t i = 0; i < text.size(); i++) {
    const uint8_t b0 = static_cast<uint8_t>(text[i]);
    const bool isAsciiWs = (b0 == ' ' || b0 == '\n' || b0 == '\r' || b0 == '\t');
    const bool isNbsp = (b0 == 0xC2 && i + 1 < text.size() && static_cast<uint8_t>(text[i + 1]) == 0xA0);
    const bool isShy = (b0 == 0xC2 && i + 1 < text.size() && static_cast<uint8_t>(text[i + 1]) == 0xAD);
    if (isShy) {
      i++;  // drop soft-hyphen entirely (invisible in page words)
      continue;
    }
    if (isAsciiWs || isNbsp) {
      if (!lastSp) out += ' ';
      lastSp = true;
      if (isNbsp) i++;  // skip 2nd UTF-8 byte
    } else {
      out += text[i];
      lastSp = false;
    }
    if (limit >= 0 && (int)out.size() >= limit) break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Count sentences in origText that end BEFORE the position where visibleText starts.
// Used to determine how many sentences to skip in translation for tail paragraphs.
static int countSentencesBefore(const std::string& origText, const std::string& visibleStart) {
  if (visibleStart.empty() || origText.empty()) return 0;

  const std::string needle = normalizeForMatch(visibleStart, 40);
  if (needle.size() < 3) return 0;

  const std::string normOrig = normalizeForMatch(origText, -1);

  // Find where the visible text starts in the original
  auto pos = normOrig.find(needle);
  if (pos == std::string::npos) return 0;

  // Honest zero when no sentence has ended yet (visible portion starts inside
  // the paragraph's first sentence — common when a long S₁ spans pages).
  // countSentences() would round up to 1 here and skip T₁ on the next page.
  return countSentencesRaw(normOrig.substr(0, pos));
}

// ── HTML parsing: extract (original, translation) paragraph pairs ─────────────

static const char* BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "div", nullptr};

static bool isBlockTag(const char* name) {
  for (int i = 0; BLOCK_TAGS[i]; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

// Selective forward SAX parse (ported from reader-cross-point-1.3). Instead of building a pair
// for EVERY paragraph in the chapter (~40 KB peak on a long chapter, which exhausts the heap),
// we keep a SPARSE vector covering only the current page's [wantFirst, wantLast] range:
//   1. Discard paragraph text for idx < wantFirst - 1 (no allocation).
//   2. Capture an entry only when idx ∈ [wantFirst, wantLast].
//   3. Retain origText ONLY for boundary entries (idx == wantFirst or wantLast) — the only
//      paragraphs that can be partially visible and need countSentencesBefore().
//   4. XML_StopParser() once past wantLast so expat bails without scanning the rest of the
//      chapter. Peak heap stays under ~5 KB even for long chapters.
struct ModalParseCtx {
  std::vector<ModalOverlay::ParagraphPair>* entries;
  int wantFirst = 0;
  int wantLast = 0;
  // Tracks the original (non-translation) paragraph index — matches the chapter parser's
  // outermost-block counter. Pre-increment: starts at -1 so the first original becomes idx 0.
  int paragraphCounter = -1;
  bool inBlock = false;
  bool isTranslation = false;
  int blockDepth = 0;
  std::string currentText;
  XML_Parser parser = nullptr;  // for XML_StopParser
};

// Push currentText as a finished original paragraph entry and advance
// paragraphCounter — same body as the original branch in modalOnEnd, factored
// out so <br/> handling in modalOnStart can reuse it. Whitespace-only text is
// dropped (no counter increment), matching ChapterHtmlSlimParser which skips
// empty blocks.
static void flushOriginalParagraph(ModalParseCtx* ctx) {
  auto& t = ctx->currentText;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
  if (t.empty()) {
    ctx->currentText.clear();
    return;
  }
  const int idx = ++ctx->paragraphCounter;
  if (idx < ctx->wantFirst - 1) {
    ctx->currentText.clear();
    return;
  }
  if (idx > ctx->wantLast) {
    if (ctx->parser) XML_StopParser(ctx->parser, XML_FALSE);
    ctx->currentText.clear();
    return;
  }
  const bool needsOrigText = (idx == ctx->wantFirst || idx == ctx->wantLast);
  ModalOverlay::ParagraphPair entry;
  entry.paragraphIdx = static_cast<int16_t>(idx);
  entry.origSentenceCount = static_cast<int16_t>(countSentences(t));
  if (needsOrigText) {
    entry.origText = std::move(t);
  }
  ctx->entries->push_back(std::move(entry));
  ctx->currentText.clear();
}

static void XMLCALL modalOnStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);

  // <br/> inside a non-translation block — mirror ChapterHtmlSlimParser, which
  // treats <br/> as a paragraph boundary (its paragraphCounter advances on
  // <br/> when the current block has content). Without this, our pair indices
  // drift behind the chapter parser's PageLine.paragraphIdx, and preparePage
  // ends up looking up the WRONG paragraphs — exactly what produces "modal
  // shows translation for paragraphs that aren't on the page" + "missing
  // translation for paragraphs that ARE on the page" together (because every
  // <br/> in the chapter creates an extra index in chapter's count that our
  // pairs vector skips over).
  if (ctx->inBlock && !ctx->isTranslation && strcmp(name, "br") == 0) {
    flushOriginalParagraph(ctx);  // only counts if currentText non-empty
    return;                        // stay in outer block — its </> handler still owns blockDepth
  }

  if (isBlockTag(name)) {
    ctx->inBlock = true;
    ctx->blockDepth = 1;
    ctx->isTranslation = false;
    ctx->currentText.clear();
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        // Our translator output marks translated paragraphs with data-translation; embedded
        // Calibre translations use lang/xml:lang. Accept all three.
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
  (void)name;
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (!ctx->inBlock) return;
  ctx->blockDepth--;
  if (ctx->blockDepth > 0) return;
  ctx->inBlock = false;

  if (ctx->isTranslation) {
    auto& t = ctx->currentText;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.erase(0, 1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    if (!t.empty() && !ctx->entries->empty() &&
        ctx->entries->back().paragraphIdx == ctx->paragraphCounter) {
      ctx->entries->back().translation = std::move(t);
      ctx->entries->back().transSentenceCount = static_cast<int16_t>(countSentences(ctx->entries->back().translation));
    }
    ctx->currentText.clear();
  } else {
    flushOriginalParagraph(ctx);
  }
}

static void XMLCALL modalOnText(void* ud, const XML_Char* s, int len) {
  auto* ctx = static_cast<ModalParseCtx*>(ud);
  if (!ctx->inBlock) return;
  // Mirror ChapterHtmlSlimParser's text normalization so origText (raw expat
  // output) matches the visible page text byte-for-byte:
  //   • U+00A0 (NBSP, 0xC2 0xA0)        → ASCII space (chapter parser does the same)
  //   • U+00AD (soft hyphen, 0xC2 0xAD) → dropped (chapter parser ignores it)
  // Without this, `find(needle)` in countSentencesBefore misses on any
  // NBSP/SHY in the first 40 chars, and trimToSentences fails to recognize
  // sentence boundaries that have NBSP-style separators (common in books
  // using French/Russian typography or "Mr. Smith" style names).
  for (int i = 0; i < len; i++) {
    const uint8_t b0 = static_cast<uint8_t>(s[i]);
    if (b0 == 0xC2 && i + 1 < len) {
      const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
      if (b1 == 0xA0) {                     // NBSP
        ctx->currentText += ' ';
        i++;
        continue;
      }
      if (b1 == 0xAD) {                     // soft hyphen — invisible, drop
        i++;
        continue;
      }
    }
    ctx->currentText += s[i];
  }
}

static std::vector<ModalOverlay::ParagraphPair> parseChapterHtml(const std::string& htmlPath, int wantFirst,
                                                                 int wantLast) {
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
    if (len < 0) len = 0;  // read error — treat as end of stream rather than feeding expat a bad length
    done = (len < (int)sizeof(buf));
    if (XML_Parse(parser, buf, len, done) == XML_STATUS_ERROR) {
      // XML_StopParser() makes expat return ERROR with code XML_ERROR_ABORTED — that's our
      // success path (we walked past the page and bailed early).
      if (XML_GetErrorCode(parser) == XML_ERROR_ABORTED) {
        stopped = true;
      } else {
        LOG_ERR("MOD", "XML parse error at line %lu", XML_GetCurrentLineNumber(parser));
      }
      break;
    }
  }
  XML_ParserFree(parser);
  file.close();
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

  // Parse HTML to get translation entries. Temporary — freed after this function.
  // Only paragraphs on this page retain their text, bounding peak RAM to one page's worth.
  auto pairs = parseChapterHtml(translatedHtmlPath, page.firstParagraphIdx, page.lastParagraphIdx);
  if (pairs.empty()) {
    LOG_DBG("MOD", "No pairs parsed from HTML");
    return;
  }

  LOG_DBG("MOD", "Parsed %d pair(s), page wants indices [%d..%d]", (int)pairs.size(), page.firstParagraphIdx,
          page.lastParagraphIdx);

  // Log pairs for debugging (sparse — each entry carries its own index).
  for (size_t i = 0; i < pairs.size(); i++) {
    LOG_DBG("MOD", "  pair[%d] idx=%d origS=%d transS=%d trans=%s(len=%d)", (int)i, pairs[i].paragraphIdx,
            pairs[i].origSentenceCount, pairs[i].transSentenceCount, pairs[i].translation.empty() ? "EMPTY" : "OK",
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

  // Match the reader's paragraph presentation: when "Extra Paragraph Spacing" is OFF, the
  // reader indents each paragraph's first line with an em-space (U+2003) instead of adding a
  // gap. Mirror that here. Applied once (preparePage is guarded by pagePrepared) so it never
  // accumulates across render frames. The complementary gap is handled in render().
  if (!SETTINGS.extraParagraphSpacing) {
    for (auto& t : pageTranslations) {
      if (!t.empty()) t.insert(0, "\xe2\x80\x83");
    }
  }

  LOG_DBG("MOD", "Result: %d translation(s)", (int)pageTranslations.size());
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

// ── Line breaking + alignment ─────────────────────────────────────────────────

namespace {

// One wrapped line of a paragraph. Produced by breakParagraph() and consumed by BOTH the height
// measurement and the draw pass — a single source of truth so the scroll height can never drift
// from what's actually drawn.
struct ModalLine {
  std::string content;          // words joined by single spaces (no trailing hyphen)
  bool hyphen = false;          // append '-' when drawing (word was hyphenated here)
  bool lastInParagraph = false; // last line of its paragraph — never justified
};

// Split a string into space-separated words.
std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> words;
  for (size_t i = 0; i < s.size();) {
    while (i < s.size() && s[i] == ' ') i++;
    const size_t start = i;
    while (i < s.size() && s[i] != ' ') i++;
    if (i > start) words.push_back(s.substr(start, i - start));
  }
  return words;
}

// Greedy word-wrap with optional Liang hyphenation (mirrors the reader): when a word overflows the
// remaining space and hyphenation is on, break it at the longest language-valid point that fits.
std::vector<ModalLine> breakParagraph(const GfxRenderer& r, int fontId, const std::string& text, int maxW, int spW,
                                      bool hyphenate) {
  std::vector<ModalLine> lines;
  std::vector<std::string> words = splitWords(text);
  if (words.empty()) return lines;

  const int hyphenW = r.getTextWidth(fontId, "-");
  std::string line;
  int lineW = 0;
  bool lineHyphen = false;
  auto pushLine = [&](bool last) {
    lines.push_back({line, lineHyphen, last});
    line.clear();
    lineW = 0;
    lineHyphen = false;
  };

  for (size_t wi = 0; wi < words.size();) {
    const std::string word = words[wi];
    const int wW = r.getTextWidth(fontId, word.c_str());
    const int need = (line.empty() ? 0 : spW) + wW;

    if (line.empty() || lineW + need <= maxW) {
      if (!line.empty()) {
        line += ' ';
        lineW += spW;
      }
      line += word;
      lineW += wW;
      wi++;
      continue;
    }

    // Word doesn't fit on the current (non-empty) line.
    if (hyphenate) {
      const int avail = maxW - lineW - spW - hyphenW;  // room for a prefix after a space + hyphen
      int bestOff = 0;
      bool bestNeedsHyphen = true;
      if (avail > 0) {
        // includeFallback=true (matches the reader): return break positions obeying the min
        // prefix/suffix even when no language rule matches — otherwise an unsupported/unset
        // translation language yields zero breaks and nothing ever hyphenates.
        for (const auto& b : Hyphenator::breakOffsets(word, true)) {
          if (b.byteOffset == 0 || b.byteOffset >= word.size()) continue;
          if (r.getTextWidth(fontId, word.substr(0, b.byteOffset).c_str()) <= avail) {
            bestOff = static_cast<int>(b.byteOffset);
            bestNeedsHyphen = b.requiresInsertedHyphen;
          }
        }
      }
      if (bestOff > 0) {
        line += ' ';
        line += word.substr(0, bestOff);
        lineHyphen = bestNeedsHyphen;
        pushLine(false);
        words[wi] = word.substr(bestOff);  // re-process the remainder on the next line
        continue;
      }
    }
    // No hyphenation possible/enabled — move the whole word to a fresh line.
    pushLine(false);
  }
  if (!line.empty()) {
    pushLine(true);
  } else if (!lines.empty()) {
    lines.back().lastInParagraph = true;
  }
  return lines;
}

// Draw one wrapped line honoring the reader's paragraph-alignment setting. Justified spreads the
// slack evenly across inter-word gaps on every line except the paragraph's last.
void drawModalLine(const GfxRenderer& r, int fontId, const ModalLine& ln, int x, int y, int maxW, int spW,
                   uint8_t align) {
  const int hyphenW = ln.hyphen ? r.getTextWidth(fontId, "-") : 0;
  const int naturalW = r.getTextWidth(fontId, ln.content.c_str()) + hyphenW;

  const bool wantJustify = (align == CrossPointSettings::JUSTIFIED || align == CrossPointSettings::BOOK_STYLE) &&
                           !ln.lastInParagraph;

  if (wantJustify && naturalW < maxW) {
    const std::vector<std::string> words = splitWords(ln.content);
    if (words.size() >= 2) {
      const int gaps = static_cast<int>(words.size()) - 1;
      const int slack = maxW - naturalW;
      const int base = slack / gaps;
      const int extra = slack % gaps;
      int cx = x;
      for (size_t i = 0; i < words.size(); i++) {
        r.drawText(fontId, cx, y, words[i].c_str());
        cx += r.getTextWidth(fontId, words[i].c_str());
        if (i + 1 < words.size()) cx += spW + base + (static_cast<int>(i) < extra ? 1 : 0);
      }
      if (ln.hyphen) r.drawText(fontId, cx, y, "-");
      return;
    }
  }

  int startX = x;
  if (align == CrossPointSettings::CENTER_ALIGN) {
    startX = x + (maxW - naturalW) / 2;
  } else if (align == CrossPointSettings::RIGHT_ALIGN) {
    startX = x + (maxW - naturalW);
  }
  std::string s = ln.content;
  if (ln.hyphen) s += "-";
  r.drawText(fontId, startX, y, s.c_str());
}

}  // namespace

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
  // Mirror the reader's "Extra Paragraph Spacing" setting: a blank-line gap between paragraphs
  // when on, otherwise paragraphs run flush (the first-line indent applied in preparePage marks
  // the boundary instead). Kept a multiple of lh so screen-by-screen scrolling stays aligned.
  const int paraSpacing = SETTINGS.extraParagraphSpacing ? lh : 0;

  cachedViewportHeight = viewportHeight;
  cachedLineHeight = lh;

  // Honor the reader's hyphenation toggle + paragraph alignment. The modal shows the *translated*
  // text, so hyphenate using the translation's target language (falls back to generic breaks).
  const bool hyphenate = SETTINGS.hyphenationEnabled != 0;
  if (hyphenate && SETTINGS.translationLanguage != 0xFF &&
      SETTINGS.translationLanguage < LanguagePickerActivity::NUM_LANGUAGES) {
    Hyphenator::setPreferredLanguage(LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].code);
  }
  const uint8_t align = SETTINGS.paragraphAlignment;

  // Break every paragraph into lines ONCE; reuse for both height and drawing so they can't diverge.
  std::vector<std::vector<ModalLine>> paras;
  paras.reserve(pageTranslations.size());
  int contentH = 0;
  for (const auto& trans : pageTranslations) {
    auto lines = breakParagraph(renderer, modalFontId, trans, maxTextW, spW, hyphenate);
    contentH += static_cast<int>(lines.size()) * lh + paraSpacing;
    paras.push_back(std::move(lines));
  }
  if (!paras.empty()) contentH -= paraSpacing;

  // totalContentHeight = full content height (NOT minus viewport).
  // scrollOffset steps through it in viewport-sized chunks.
  totalContentHeight = contentH;

  if (scrollOffset < 0) scrollOffset = 0;

  const int clipTop = yOffset;
  const int clipBottom = yOffset + viewportHeight;
  int curY = yOffset - scrollOffset;
  for (const auto& lines : paras) {
    for (const auto& ln : lines) {
      if (curY + lh > clipTop && curY + lh <= clipBottom) {
        drawModalLine(renderer, modalFontId, ln, xOffset + PAD, curY, maxTextW, spW, align);
      }
      curY += lh;
    }
    curY += paraSpacing;
  }
}

// ── Font helper ──────────────────────────────────────────────────────────────

int getModalFontId() { return getTooltipFontId(); }
