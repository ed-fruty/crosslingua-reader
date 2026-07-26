#include "PageTranslationOverlay.h"

#include <CrossPointSettings.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ParagraphBoundary.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cstring>

#include "SentenceSplitter.h"
#include "TextNormalize.h"
#include "TooltipOverlay.h"  // getTooltipFontId() — Page Translation shares the tooltip's font
#include "activities/translator/LanguagePickerActivity.h"
#include "fontIds.h"

// ── State management ─────────────────────────────────────────────────────────

void PageTranslationOverlay::setTranslatedHtmlPath(const std::string& path) { translatedHtmlPath = path; }

void PageTranslationOverlay::open() {
  active = true;
  scrollOffset = 0;
}

void PageTranslationOverlay::onSectionChanged() {
  active = false;
  scrollOffset = 0;
  totalContentHeight = 0;
  pagePrepared = false;
  pageParagraphs.clear();
  translatedCount = 0;
}

void PageTranslationOverlay::onPageChanged() {
  active = false;
  scrollOffset = 0;
  totalContentHeight = 0;
  pagePrepared = false;
  pageParagraphs.clear();
  translatedCount = 0;
}

// Sentence math (countSentences / trimToSentences / trimToLastSentences /
// countSentencesBefore) lives in SentenceSplitter, reimplemented over the shared
// textnorm canonical fold — the same single source of sentence-boundary truth the
// word-span splitter uses. This overlay only wraps those helpers here.

// ── HTML parsing: extract (original, translation) paragraph pairs ─────────────
//
// The paragraph-boundary rule (container block tags + <br/>) is NOT redefined
// here: this reparse and ChapterHtmlSlimParser's layout counter both call the
// shared paraboundary predicate (ParagraphBoundary.h) so their paragraph indices
// cannot diverge by a tag. See that header for the full contract.

// Selective forward SAX parse. Instead of building a pair for EVERY paragraph in the chapter
// (~40 KB peak on a long chapter, which exhausts the heap), we keep a SPARSE vector covering only
// the current page's [wantFirst, wantLast] range:
//   1. Discard paragraph text for idx < wantFirst - 1 (no allocation).
//   2. Capture an entry only when idx ∈ [wantFirst, wantLast].
//   3. Retain origText ONLY for boundary entries (idx == wantFirst or wantLast) — the only
//      paragraphs that can be partially visible and need countSentencesBefore().
//   4. XML_StopParser() once past wantLast so expat bails without scanning the rest of the
//      chapter. Peak heap stays under ~5 KB even for long chapters.
struct PageTranslationParseCtx {
  std::vector<PageTranslationOverlay::ParagraphPair>* entries;
  int wantFirst = 0;
  int wantLast = 0;
  // Tracks the original (non-translation) paragraph index — matches the chapter parser's
  // outermost-block counter. Pre-increment: starts at -1 so the first original becomes idx 0.
  int paragraphCounter = -1;
  bool inBlock = false;
  bool isTranslation = false;
  int blockDepth = 0;
  // >0 while inside a subtree the layout parser SKIPS after emitting a single synthetic
  // paragraph — a <table> ("[Table omitted]") or an undecodable <img> with alt text
  // ("[Image: …]"). Mirrors ChapterHtmlSlimParser::skipUntilDepth: every nested start bumps
  // it, every nested end drops it, and the matching close returns it to 0.
  int skipDepth = 0;
  // The innermost open block is an <li>. ChapterHtmlSlimParser seeds every <li> block with a
  // U+2022 bullet, so an <li> whose only content so far is that implicit bullet still flushes a
  // (bullet-only) paragraph when its first child block opens — we replicate that below.
  bool currentBlockIsLi = false;
  std::string currentText;
  XML_Parser parser = nullptr;  // for XML_StopParser
};

// Push currentText as a finished original paragraph entry and advance
// paragraphCounter — same body as the original branch in pageTranslationOnEnd, factored
// out so <br/> handling in pageTranslationOnStart can reuse it. Whitespace-only text is
// dropped (no counter increment), matching ChapterHtmlSlimParser which skips
// empty blocks.
static void flushOriginalParagraph(PageTranslationParseCtx* ctx) {
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
  PageTranslationOverlay::ParagraphPair entry;
  entry.paragraphIdx = static_cast<int16_t>(idx);
  entry.origSentenceCount = static_cast<int16_t>(countSentences(t));
  if (needsOrigText) {
    entry.origText = std::move(t);
  }
  ctx->entries->push_back(std::move(entry));
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
static void seedLiBulletIfEmpty(PageTranslationParseCtx* ctx) {
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

static void XMLCALL pageTranslationOnStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<PageTranslationParseCtx*>(ud);

  // Inside a skipped subtree (table cell / undecodable-image alt content): swallow every nested
  // element so it can never contribute a paragraph, exactly like ChapterHtmlSlimParser's
  // skipUntilDepth. Balanced by the matching drops in pageTranslationOnEnd.
  if (ctx->skipDepth > 0) {
    ctx->skipDepth++;
    return;
  }

  // <br/> — a hard break is an EMPTY, NO-SCOPE element: it never opens a scope, so it must NEVER
  // touch blockDepth (pageTranslationOnEnd skips its self-close too, keeping the count balanced in ALL
  // cases). In an ORIGINAL block it ends the current paragraph in place so the counter advances
  // exactly as ChapterHtmlSlimParser does on <br/>; in a TRANSLATION block it is just an internal
  // line break, so keep accumulating (no flush).
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
  // (like <br/>) it must not touch blockDepth — pageTranslationOnEnd treats <img> as a no-op.
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
    // U+2022 bullet, so its block is non-empty). Do that flush BEFORE (re)starting the block.
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
        // Our translator output marks translated paragraphs with data-translation; embedded
        // Calibre translations use lang/xml:lang. Accept all three.
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

static void XMLCALL pageTranslationOnEnd(void* ud, const XML_Char* name) {
  auto* ctx = static_cast<PageTranslationParseCtx*>(ud);
  // Leaving a skipped subtree (table cell / undecodable-image alt content). The matching close of
  // the <table>/<img> that opened the skip brings skipDepth back to 0. Mirrors skipUntilDepth.
  if (ctx->skipDepth > 0) {
    ctx->skipDepth--;
    return;
  }
  // <br/> is an empty element: expat fires a matching end event for it. It has no
  // scope (pageTranslationOnStart already flushed the paragraph in place and left blockDepth
  // untouched), so ignore it here. Otherwise it would decrement blockDepth, close
  // the enclosing block one tag early, drop post-<br/> text, and drift the counter.
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
    if (!t.empty() && !ctx->entries->empty() && ctx->entries->back().paragraphIdx == ctx->paragraphCounter) {
      ctx->entries->back().translation = std::move(t);
      ctx->entries->back().transSentenceCount = static_cast<int16_t>(countSentences(ctx->entries->back().translation));
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

static void XMLCALL pageTranslationOnText(void* ud, const XML_Char* s, int len) {
  auto* ctx = static_cast<PageTranslationParseCtx*>(ud);
  if (ctx->skipDepth > 0) return;  // text inside a skipped table cell / image subtree
  if (!ctx->inBlock) return;
  // Canonicalize inter-token separators to a single ASCII space so origText and
  // translation reach the SSOT (countSentences / trimToSentences / trimToLastSentences /
  // foldForMatch in SentenceSplitter + TextNormalize) AND the display word-wrapper
  // (splitWords, ASCII-space only) in ONE representation. For U+00A0 / U+202F this is the
  // byte-identical representation the laid-out page words (visibleText) already use
  // (ChapterHtmlSlimParser splits both into a standalone ASCII-space word); for the rarer
  // family members the boundary check reconciles the two sides through foldForMatch, which
  // collapses the whole set.
  //
  // The recognized set is exactly textnorm::whitespaceLenAt — the SSOT for "what
  // separates words / sentences": ASCII space/tab/CR/LF, U+00A0, U+2000..U+200A,
  // U+202F, U+205F. Collapsing them here (rather than only U+00A0, as the original
  // caller-side workaround did) keeps this consumer aligned with the SSOT now that the
  // splitter is NBSP-family-aware:
  //   • A raw multi-byte separator that survived (e.g. U+202F, which Google, French and
  //     Ukrainian typography emit) made the boundary drift check compare an ASCII-spaced
  //     page slice against a raw-separated source paragraph — a false "drift" that forces
  //     source fallback and can drop translatedCount to 0 (the "no translations" toast)
  //     on any build whose foldForMatch does not collapse that byte sequence.
  //   • It also reached splitWords() unsplit, so "word.<U+202F>Next" was ONE token wider
  //     than the line → mis-wrapped / mis-aligned output.
  //   • U+00AD soft hyphen stays dropped (invisible in the page words).
  // NOTE (unchanged from before): a multi-byte separator split across two expat
  // character-data callbacks is not folded (the second half arrives next call); this is
  // rare and identical to the prior behaviour.
  for (int i = 0; i < len; i++) {
    const uint8_t b0 = static_cast<uint8_t>(s[i]);
    if (b0 == 0xC2 && i + 1 < len) {
      const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
      if (b1 == 0xA0) {  // U+00A0 NBSP -> space
        ctx->currentText += ' ';
        i++;
        continue;
      }
      if (b1 == 0xAD) {  // U+00AD soft hyphen — invisible, drop
        i++;
        continue;
      }
    } else if (b0 == 0xE2 && i + 2 < len) {
      const uint8_t b1 = static_cast<uint8_t>(s[i + 1]);
      const uint8_t b2 = static_cast<uint8_t>(s[i + 2]);
      // U+2000..U+200A, U+202F, U+205F (mirror textnorm::whitespaceLenAt) -> space.
      if ((b1 == 0x80 && b2 >= 0x80 && b2 <= 0x8A) || (b1 == 0x80 && b2 == 0xAF) || (b1 == 0x81 && b2 == 0x9F)) {
        ctx->currentText += ' ';
        i += 2;  // the for-loop ++ consumes the third byte
        continue;
      }
    }
    ctx->currentText += s[i];
  }
}

static std::vector<PageTranslationOverlay::ParagraphPair> parseChapterHtml(const std::string& htmlPath, int wantFirst,
                                                                           int wantLast) {
  std::vector<PageTranslationOverlay::ParagraphPair> entries;

  if (htmlPath.empty()) return entries;

  HalFile file;
  if (!Storage.openFileForRead("PGT", htmlPath, file)) {
    LOG_ERR("PGT", "Cannot open %s", htmlPath.c_str());
    return entries;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("PGT", "Failed to create expat parser");
    return entries;
  }

  PageTranslationParseCtx ctx;
  ctx.entries = &entries;
  ctx.wantFirst = wantFirst;
  ctx.wantLast = wantLast;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, pageTranslationOnStart, pageTranslationOnEnd);
  XML_SetCharacterDataHandler(parser, pageTranslationOnText);

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
        LOG_ERR("PGT", "XML parse error at line %lu", XML_GetCurrentLineNumber(parser));
      }
      break;
    }
  }
  XML_ParserFree(parser);
  LOG_DBG("PGT", "Parsed %d pair(s) in [%d..%d] from %s%s", (int)entries.size(), wantFirst, wantLast, htmlPath.c_str(),
          stopped ? " (early-stop)" : "");
  return entries;
}

// ── Page preparation: use paragraph indices stored during page building ────────

// Decide what translated text to show for one paragraph that is visible on the
// current page. FULL paragraph on page => the whole translation. Partially
// visible boundary paragraph => the sentence window that lines up with the
// visible source slice, every index CLAMPED to the translation's own sentence
// count so we never index past its end. Falls back to the whole translation when
// per-sentence alignment would be unreliable: no detectable translation sentences
// (transSentenceCount == 0 treated as ordinary), or source vs. translation
// sentence totals differ by more than one (large skew => do not mis-slice).
static std::string sliceTranslationForPage(const PageTranslationOverlay::ParagraphPair& pair,
                                           const std::string& visibleText, int visibleSentences, bool isFirst,
                                           bool isLast) {
  const int transTotal = pair.transSentenceCount;
  const int origTotal = pair.origSentenceCount;

  const bool partiallyVisible = (visibleSentences > 0 && visibleSentences < origTotal);
  if (!partiallyVisible) return pair.translation;  // whole paragraph on this page

  int skew = transTotal - origTotal;
  if (skew < 0) skew = -skew;
  if (transTotal <= 0 || skew > 1) return pair.translation;  // unreliable to slice — show whole

  // Sentences of THIS paragraph to reveal: the fully visible ones plus the
  // partial sentence at each broken edge (head +1, tail +1). Only boundary
  // paragraphs retain origText, so the skip is meaningful only when isFirst.
  const int brokenEdges = (isFirst && isLast) ? 2 : 1;
  int fromSentence = isFirst ? countSentencesBefore(pair.origText, visibleText) : 0;
  int toSentence = fromSentence + visibleSentences + brokenEdges;

  // Clamp every index into the translation's own [0, transTotal] range.
  if (toSentence > transTotal) toSentence = transTotal;
  if (fromSentence > transTotal) fromSentence = transTotal;
  if (fromSentence > toSentence) fromSentence = toSentence;
  if (toSentence - fromSentence <= 0) return pair.translation;  // degenerate window — show whole

  std::string trimmed = trimToSentences(pair.translation, toSentence);
  if (fromSentence > 0) trimmed = trimToLastSentences(trimmed, toSentence - fromSentence);
  return trimmed;
}

void PageTranslationOverlay::collectPageGlyphText(const Page& page, std::string& out) {
  preparePage(page);  // idempotent (pagePrepared guard); render() will find it already done
  size_t need = 0;
  bool anyUntranslated = false;
  for (const auto& p : pageParagraphs) {
    need += p.text.size() + 1;
    if (!p.translated) anyUntranslated = true;
  }
  const char* const marker = tr(STR_NO_TRANSLATION);
  if (anyUntranslated) need += std::strlen(marker) + 1;
  out.reserve(out.size() + need);
  for (const auto& p : pageParagraphs) {
    out += p.text;
    out += ' ';
  }
  if (anyUntranslated) out += marker;  // dim marker line drawn for each source-fallback paragraph
}

void PageTranslationOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  pageParagraphs.clear();
  translatedCount = 0;
  totalContentHeight = 0;

  // The page knows exactly which paragraph indices it contains (set by the parser).
  LOG_DBG("PGT", "Page paragraph indices: first=%d last=%d", page.firstParagraphIdx, page.lastParagraphIdx);

  if (page.firstParagraphIdx < 0 || page.lastParagraphIdx < 0) {
    LOG_DBG("PGT", "Page has no paragraph indices (old cache?) — clear cache and retry");
    return;
  }

  // Sparse translation entries for this page's [first..last] range. Temporary —
  // freed after this function; only boundary paragraphs retain origText, bounding
  // peak RAM to ~5 KB. May be EMPTY (untranslated page) — we still source-fill
  // every visible paragraph below and let the render() guard decide whether to
  // open the overlay at all.
  auto pairs = parseChapterHtml(translatedHtmlPath, page.firstParagraphIdx, page.lastParagraphIdx);

  // Per-paragraph VISIBLE text from the page's lines, keyed by paragraphIdx. This
  // is the source-of-truth for both the source fallback and the boundary
  // safety-check. Lines of one paragraph are contiguous, so one entry per idx.
  struct ParaText {
    int16_t idx;
    std::string text;
  };
  std::vector<ParaText> pageParas;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const int16_t pIdx = line->paragraphIdx;
    if (pIdx < 0) continue;
    if (pageParas.empty() || pageParas.back().idx != pIdx) pageParas.push_back({pIdx, ""});
    // v2 flattened TextBlock word storage: iterate by index (wordCount/wordText),
    // NOT the fork's getWords() container.
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (!pageParas.back().text.empty()) pageParas.back().text += ' ';
      pageParas.back().text += block->wordText(i);
    }
  }

  const int first = page.firstParagraphIdx;
  const int last = page.lastParagraphIdx;
  bool boundaryErrLogged = false;

  // INVARIANT: walk the page's paragraph index RANGE [first..last] (not the sparse
  // pairs list) and emit exactly one entry per VISIBLE source paragraph. A
  // translation attaches to a paragraph ONLY by validated index equality — never
  // by document order or content search. A paragraph with no/empty translation,
  // or one that fails the boundary safety-check, is source-filled and marked
  // untranslated (Option C: source text + dim marker).
  for (int idx = first; idx <= last; idx++) {
    const std::string* visiblePtr = nullptr;
    for (const auto& pp : pageParas) {
      if (pp.idx == idx) {
        visiblePtr = &pp.text;
        break;
      }
    }
    if (visiblePtr == nullptr) continue;  // image/gap index with no visible text line — nothing to show
    const std::string& visibleText = *visiblePtr;

    const PageTranslationOverlay::ParagraphPair* pair = nullptr;
    for (const auto& p : pairs) {
      if (p.paragraphIdx == idx) {
        pair = &p;
        break;
      }
    }

    const bool isFirst = (idx == first);
    const bool isLast = (idx == last);

    // Boundary safety-check (a VALIDATOR, not the mapping): the two boundary
    // paragraphs retain origText. If a discriminating prefix of the visible slice
    // cannot be located inside the mapped source paragraph (both folded), the
    // index mapping has drifted — log once and force source fallback rather than
    // show a neighbor's translation. Array-free (substring compare, no std::set).
    //
    // The needle is bounded and truncated BEFORE the first line-break hyphenation
    // artifact: layout splits a word at a line end into "prefix-" + "remainder"
    // tokens, so the joined visible text carries a "…prefix- remainder…" that the
    // unsplit source lacks. That artifact is a '-' glued to the preceding char and
    // followed by a space (a real spaced dash has a space on BOTH sides), so we
    // stop the needle there. The kept prefix always covers at least the first word
    // (a line-end split never lands on the first token), which is enough to catch
    // gross drift while tolerating benign in-text hyphenation.
    bool forceSource = false;
    if (pair != nullptr && (isFirst || isLast) && !pair->origText.empty() && !visibleText.empty()) {
      // ChapterHtmlSlimParser prepends a U+2022 bullet as the first word of every <li> (and, when
      // Extra Paragraph Spacing is OFF, a U+2003 em-space indent before it), so a list item's
      // visibleText reads "• <text>" / " • <text>". The reparsed origText has neither, and
      // foldForMatch folds neither, so the needle would never be found and a correctly-translated
      // <li> at a page boundary would be wrongly source-filled. Strip a leading em-space indent
      // and/or bullet (+ following spaces) here -- LOCAL to the boundary needle only, so the
      // general fold is untouched and origText is unchanged. The em-space strip also fixes the
      // same-cause artifact on non-list indented boundary paragraphs.
      std::string vis = visibleText;
      size_t bo = 0;
      if (vis.compare(0, 3, "\xe2\x80\x83") == 0) bo += 3;  // U+2003 em-space paragraph indent
      if (vis.compare(bo, 3, "\xe2\x80\xa2") == 0) {        // U+2022 <li> bullet
        bo += 3;
        while (bo < vis.size() && vis[bo] == ' ') bo++;
      }
      if (bo) vis.erase(0, bo);
      std::string needle = textnorm::foldForMatch(vis, 48);
      for (size_t k = 1; k + 1 < needle.size(); k++) {
        if (needle[k] == '-' && needle[k + 1] == ' ' && needle[k - 1] != ' ') {
          needle.resize(k);
          break;
        }
      }
      if (needle.size() >= 3) {
        const std::string foldedOrig = textnorm::foldForMatch(pair->origText);
        if (foldedOrig.find(needle) == std::string::npos) {
          if (!boundaryErrLogged) {
            LOG_ERR("PGT", "boundary drift at idx %d: visible text not in source paragraph — source fallback", idx);
            boundaryErrLogged = true;
          }
          forceSource = true;
        }
      }
    }

    if (pair == nullptr || pair->translation.empty() || forceSource) {
      pageParagraphs.push_back({visibleText, false});  // Option C source fallback
      continue;
    }

    const int visibleSentences = countSentences(visibleText);
    std::string shown = sliceTranslationForPage(*pair, visibleText, visibleSentences, isFirst, isLast);
    if (shown.empty()) {
      pageParagraphs.push_back({visibleText, false});  // defensive: never show a blank translated line
      continue;
    }
    pageParagraphs.push_back({std::move(shown), true});
    translatedCount++;
  }

  // Match the reader's paragraph presentation: when "Extra Paragraph Spacing" is
  // OFF, the reader indents each paragraph's first line with an em-space (U+2003)
  // instead of adding a gap. Mirror that here for the paragraph body (translation
  // or source). Applied once (preparePage is guarded by pagePrepared). The dim
  // marker line added in render() is never indented. The gap is handled in render().
  if (!SETTINGS.extraParagraphSpacing) {
    for (auto& p : pageParagraphs) {
      if (!p.text.empty()) p.text.insert(0, "\xe2\x80\x83");
    }
  }

  LOG_DBG("PGT", "Result: %d paragraph(s), %d translated", (int)pageParagraphs.size(), (int)translatedCount);
}

// ── Button handling ──────────────────────────────────────────────────────────
//
// Which pair scrolls/closes the OPEN overlay is configurable (SETTINGS.pageTranslationButtons):
// SIDE (default) uses PageBack/PageForward, FRONT uses Left/Right. The "longpress-opens-overlay"
// gesture is NOT handled here: it is detected by EpubReaderActivity (always on the side pair),
// which calls PageTranslationOverlay::open() externally. Back always dismisses.

bool PageTranslationOverlay::handleInput(MappedInputManager& input) {
  const bool useFrontButtons = (SETTINGS.pageTranslationButtons == CrossPointSettings::OVERLAY_BUTTONS_FRONT);
  const auto nextBtn = useFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
  const auto backBtn = useFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;

  // Next button: scroll down one screenful, or close if we're at the end.
  if (input.wasReleased(nextBtn)) {
    if (!active) return false;
    const int lh = cachedLineHeight > 0 ? cachedLineHeight : 20;
    const int vpH = cachedViewportHeight > 0 ? cachedViewportHeight : 700;
    const int screenScroll = (vpH / lh) * lh;
    const int maxScroll = std::max(0, (int)totalContentHeight - vpH);
    LOG_DBG("PGT", "SCROLL NEXT: offset %d -> %d (step=%d, vpH=%d, lh=%d, totalH=%d, maxScroll=%d)", scrollOffset,
            scrollOffset + screenScroll, screenScroll, vpH, lh, totalContentHeight, maxScroll);
    if (scrollOffset + vpH < totalContentHeight) {
      // More content below — scroll down.
      scrollOffset += screenScroll;
    } else {
      // Already showing last content — close the overlay.
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

  // ESC/Back button: dismiss the overlay if active.
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
struct PageTranslationLine {
  std::string content;           // words joined by single spaces (no trailing hyphen)
  bool hyphen = false;           // append '-' when drawing (word was hyphenated here)
  bool lastInParagraph = false;  // last line of its paragraph — never justified
  bool dim = false;              // dim/greyed marker line (STR_NO_TRANSLATION) — left-aligned, never justified
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
std::vector<PageTranslationLine> breakParagraph(const GfxRenderer& r, int fontId, const std::string& text, int maxW,
                                                int spW, bool hyphenate) {
  std::vector<PageTranslationLine> lines;
  std::vector<std::string> words = splitWords(text);
  if (words.empty()) return lines;

  const int hyphenW = r.getTextWidth(fontId, "-");
  std::string line;
  int lineW = 0;
  bool lineHyphen = false;
  auto pushLine = [&](bool last) {
    lines.push_back({line, lineHyphen, last, false});
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
        // prefix/suffix even when no language rule matches. translated=true selects the
        // translated-language hyphenator slot (v2's per-block routing) — the overlay shows the
        // translation, so it must break in the target language, not the book's source language.
        for (const auto& b : Hyphenator::breakOffsets(word, true, true)) {
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
void drawPageTranslationLine(const GfxRenderer& r, int fontId, const PageTranslationLine& ln, int x, int y, int maxW,
                             int spW, uint8_t align) {
  if (ln.dim) {
    // Missing-translation marker (Option C): left-aligned, never justified. The fork rendered it
    // through drawText's grayLevel path (grayLevel=1) so it dimmed on grayscale panels; v2's
    // drawText has no grayLevel parameter and the target panel is monochrome, so it renders as
    // ordinary black text (the fork's grayLevel=1 also fell back to black on pure-BW panels).
    r.drawText(fontId, x, y, ln.content.c_str());
    return;
  }

  const int hyphenW = ln.hyphen ? r.getTextWidth(fontId, "-") : 0;
  const int naturalW = r.getTextWidth(fontId, ln.content.c_str()) + hyphenW;

  const bool wantJustify =
      (align == CrossPointSettings::JUSTIFIED || align == CrossPointSettings::BOOK_STYLE) && !ln.lastInParagraph;

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

void PageTranslationOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int pageTranslationFontId,
                                    int xOffset, int yOffset, int viewportWidth, int viewportHeight) {
  (void)fontId;
  if (!active) return;

  preparePage(page);

  // Per-page open guard: refuse to open the overlay on a page where NOTHING is
  // translated (fully-untranslated page => fall through to the normal reader,
  // which shows the "switch to Normal" toast). A partly-translated page still
  // opens and source-fills the gaps.
  if (translatedCount == 0) {
    active = false;
    return;
  }

  renderer.fillRect(xOffset, yOffset, viewportWidth, viewportHeight, false);

  const int lh = renderer.getLineHeight(pageTranslationFontId);
  const int spW = renderer.getSpaceWidth(pageTranslationFontId);
  constexpr int PAD = 10;
  const int maxTextW = viewportWidth - 2 * PAD;
  // Mirror the reader's "Extra Paragraph Spacing" setting: a blank-line gap between paragraphs
  // when on, otherwise paragraphs run flush (the first-line indent applied in preparePage marks
  // the boundary instead). Kept a multiple of lh so screen-by-screen scrolling stays aligned.
  const int paraSpacing = SETTINGS.extraParagraphSpacing ? lh : 0;

  cachedViewportHeight = static_cast<int16_t>(viewportHeight);
  cachedLineHeight = static_cast<int16_t>(lh);

  // Honor the reader's hyphenation toggle + paragraph alignment. The overlay shows the *translated*
  // text, so route hyphenation through the translation's target-language slot (falls back to
  // generic breaks when that language has no trie).
  const bool hyphenate = SETTINGS.hyphenationEnabled != 0;
  if (hyphenate && SETTINGS.translationLanguage != 0xFF &&
      SETTINGS.translationLanguage < LanguagePickerActivity::NUM_LANGUAGES) {
    Hyphenator::setTranslatedLanguage(LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].code);
  }
  const uint8_t align = SETTINGS.paragraphAlignment;

  // Break every paragraph into lines ONCE; reuse for both height and drawing so they can't diverge.
  std::vector<std::vector<PageTranslationLine>> paras;
  paras.reserve(pageParagraphs.size());
  const char* const marker = tr(STR_NO_TRANSLATION);
  int contentH = 0;
  for (const auto& para : pageParagraphs) {
    auto lines = breakParagraph(renderer, pageTranslationFontId, para.text, maxTextW, spW, hyphenate);
    if (!para.translated) {
      // Option C: the body already holds the SOURCE text; append one short dim
      // marker line so the untranslated gap is visible but unobtrusive.
      PageTranslationLine mk;
      mk.content = marker;
      mk.lastInParagraph = true;
      mk.dim = true;
      lines.push_back(std::move(mk));
    }
    contentH += static_cast<int>(lines.size()) * lh + paraSpacing;
    paras.push_back(std::move(lines));
  }
  if (!paras.empty()) contentH -= paraSpacing;

  // totalContentHeight = full content height (NOT minus viewport).
  // scrollOffset steps through it in viewport-sized chunks.
  totalContentHeight = static_cast<int16_t>(contentH);

  if (scrollOffset < 0) scrollOffset = 0;

  const int clipTop = yOffset;
  const int clipBottom = yOffset + viewportHeight;
  int curY = yOffset - scrollOffset;
  for (const auto& lines : paras) {
    for (const auto& ln : lines) {
      if (curY + lh > clipTop && curY + lh <= clipBottom) {
        drawPageTranslationLine(renderer, pageTranslationFontId, ln, xOffset + PAD, curY, maxTextW, spW, align);
      }
      curY += lh;
    }
    curY += paraSpacing;
  }
}

// ── Font helper ──────────────────────────────────────────────────────────────
//
// Fork parity: the Page Translation overlay renders in the SAME reader-derived font as the tooltip
// (getTooltipFontId — the reader's family, at the size the Lingua submenu's Translation Size row
// selects), so it honors the reader's Font Family and Font Size settings as well as that row. It
// previously returned a fixed UI_12_FONT_ID, which ignored all three (that predated the Tooltip port
// that brought getTooltipFontId in).

int getPageTranslationFontId() { return getTooltipFontId(); }
