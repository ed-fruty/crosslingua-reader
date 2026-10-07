#include "modules/lingua/modes/page_translation/PageTranslationOverlay.h"

#include <CrossPointSettings.h>
#include <Epub/ParsedText.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "fontIds.h"
#include "modules/lingua/activities/LanguagePickerActivity.h"
#include "modules/lingua/parsers/TranslationPairWalker.h"
#include "modules/lingua/utils/SentenceSplitter.h"
#include "modules/lingua/utils/TextNormalize.h"

// ── State management ─────────────────────────────────────────────────────────

void PageTranslationOverlay::setTranslationSource(const std::string& htmlPath, const std::string& cachePath,
                                                  const std::string& bookPrimaryLang) {
  translationSourcePath = htmlPath;
  pairCachePath = cachePath;
  this->bookPrimaryLang = bookPrimaryLang;
}

void PageTranslationOverlay::open() {
  active = true;
  scrollOffset = 0;
  nextScrollOffset = -1;
  prevScrollOffset = -1;
}

void PageTranslationOverlay::onSectionChanged() {
  active = false;
  scrollOffset = 0;
  nextScrollOffset = -1;
  prevScrollOffset = -1;
  pagePrepared = false;
  pageParagraphs.clear();
  translatedCount = 0;
}

void PageTranslationOverlay::onPageChanged() {
  active = false;
  scrollOffset = 0;
  nextScrollOffset = -1;
  prevScrollOffset = -1;
  pagePrepared = false;
  pageParagraphs.clear();
  translatedCount = 0;
}

// Sentence math (countSentences / trimToSentences / trimToLastSentences /
// countSentencesBefore) lives in SentenceSplitter, reimplemented over the shared
// textnorm canonical fold — the same single source of sentence-boundary truth the
// word-span splitter uses. This overlay only wraps those helpers here.

// ── Paragraph data: one shared walker over the chapter HTML ──────────────────
//
// No paragraph counting happens here any more. The overlay asks
// lingua::matchPageParagraphs for the pair behind each paragraph the PAGE shows, matched by text --
// so the parser's PageLine::paragraphIdx stays the only paragraph numbering in the system and there
// is no second counter to drift from it. See TranslationPairWalker.h for why.

// One paragraph's texts as the walker found them. Sentence counts are computed here rather than
// carried, so the sentence rule keeps exactly one implementation (SentenceSplitter).
struct ParagraphEntry {
  std::string origText;     // the FULL original paragraph
  std::string translation;  // the FULL translation
  int16_t origSentenceCount = 0;
  int16_t transSentenceCount = 0;
};

// ── Page preparation: use paragraph indices stored during page building ────────

// Decide what translated text to show for one paragraph that is visible on the
// current page. FULL paragraph on page => the whole translation. Partially
// visible boundary paragraph => the sentence window that lines up with the
// visible source slice, every index CLAMPED to the translation's own sentence
// count so we never index past its end. Falls back to the whole translation when
// per-sentence alignment would be unreliable: no detectable translation sentences
// (transSentenceCount == 0 treated as ordinary), or source vs. translation
// sentence totals differ by more than one (large skew => do not mis-slice).
static std::string sliceTranslationForPage(const ParagraphEntry& pair, const std::string& visibleText,
                                           int visibleSentences, bool isFirst, bool isLast) {
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

  // Per-paragraph VISIBLE text from the page's lines, in reading order. This is the page's own
  // truth: the source fallback, the text matched against the chapter, and the sentence window are
  // all derived from it. Lines of one paragraph are contiguous, so one entry per paragraph index.
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
    const auto& block = line->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (!pageParas.back().text.empty()) pageParas.back().text += ' ';
      pageParas.back().text += block->wordText(i);
    }
  }
  if (pageParas.empty()) {
    LOG_DBG("PGT", "Page has no text lines");
    return;
  }

  // Ask the shared walker for the pair behind each of them. Bounded work: it stops as soon as every
  // paragraph on this page has matched, and holds one paragraph at a time while walking.
  std::vector<std::string> visibleTexts;
  visibleTexts.reserve(pageParas.size());
  for (const auto& pp : pageParas) visibleTexts.push_back(pp.text);
  std::vector<lingua::PageMatch> matches(pageParas.size());
  lingua::matchPageParagraphsCached(pairCachePath, translationSourcePath, bookPrimaryLang, visibleTexts.data(),
                                    static_cast<int>(visibleTexts.size()), matches.data());

  for (size_t i = 0; i < pageParas.size(); i++) {
    const std::string& visibleText = pageParas[i].text;
    const lingua::PageMatch& match = matches[i];

    // Does this paragraph BEGIN on this page? Only the page's first entry can fail to -- every later
    // one appears because a new paragraph opened here. The match offset answers it: a paragraph that
    // starts here has its visible slice at offset 0 of the original, while one that flowed in from
    // the previous page is found further in. There is no structural flag to read (PageLine carries no
    // "first line of its paragraph" bit) and the layout's baked indent cannot stand in for one -- it
    // is 0 for a genuine start too whenever the spacing setting is on. Unmatched defaults to TRUE,
    // because a chapter's opening paragraph must indent.
    const bool startsHere = (i > 0) || !match.matched || match.matchOffset == 0;

    if (!match.matched || match.translation.empty()) {
      pageParagraphs.push_back({visibleText, false, startsHere});  // source text + the dim marker
      continue;
    }

    ParagraphEntry entry;
    entry.origText = match.original;
    entry.translation = match.translation;
    entry.origSentenceCount = static_cast<int16_t>(countSentences(entry.origText));
    entry.transSentenceCount = static_cast<int16_t>(countSentences(entry.translation));

    const int visibleSentences = countSentences(visibleText);
    const bool isFirst = (i == 0);
    const bool isLast = (i + 1 == pageParas.size());
    std::string shown = sliceTranslationForPage(entry, visibleText, visibleSentences, isFirst, isLast);
    if (shown.empty()) {
      pageParagraphs.push_back({visibleText, false, startsHere});  // never show a blank translated line
      continue;
    }
    pageParagraphs.push_back({std::move(shown), true, startsHere});
    translatedCount++;
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

  // Next/Back move to a scroll offset render() derived from the REAL line positions (see
  // nextScrollOffset / prevScrollOffset), instead of stepping by a fixed pixel arithmetic that
  // assumed every vertical step was a whole line height. With paragraph gaps of half a line height
  // (what the page layout uses) a fixed step drifts off the line grid; a line-top target keeps the
  // window aligned to real lines, which is what lets the draw pass require a fully-visible line box
  // and still never lose one. EpubReaderActivity re-renders on every accepted press
  // (handleInput -> requestUpdate), so these targets always describe the window on screen.

  // Next button: advance to the first line that is not fully visible, or close if there is none.
  if (input.wasReleased(nextBtn)) {
    if (!active) return false;
    LOG_DBG("PGT", "SCROLL NEXT: offset %d -> %d", scrollOffset, nextScrollOffset);
    if (nextScrollOffset >= 0) {
      scrollOffset = nextScrollOffset;
    } else {
      // Already showing the last content — close the overlay.
      active = false;
      scrollOffset = 0;
    }
    return true;
  }

  // Back button: step back one screenful (line-aligned), or close if already at the top.
  if (input.wasReleased(backBtn)) {
    if (!active) return false;
    if (scrollOffset > 0 && prevScrollOffset >= 0) {
      scrollOffset = prevScrollOffset;
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
  std::string content;  // words joined by single spaces (no trailing hyphen)
  // Pixel indent for THIS line: the paragraph's first-line indent, carried on the paragraph's first
  // line only (0 on every other line, and on the dim marker line). It shifts the line's start and
  // shrinks its usable width, exactly as it does in the page layout (extractLine: xpos starts at
  // firstLineIndent, effectivePageWidth = pageWidth - firstLineIndent).
  int16_t indent = 0;
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
// `firstIndent` is the paragraph's first-line indent: it narrows the FIRST line's usable width
// (and, at draw time, shifts its start) — the same two effects the page layout applies.
std::vector<PageTranslationLine> breakParagraph(const GfxRenderer& r, int fontId, const std::string& text, int maxW,
                                                int firstIndent, int spW, bool hyphenate) {
  std::vector<PageTranslationLine> lines;
  std::vector<std::string> words = splitWords(text);
  if (words.empty()) return lines;

  const int hyphenW = r.getTextWidth(fontId, "-");
  std::string line;
  int lineW = 0;
  bool lineHyphen = false;
  auto pushLine = [&](bool last) {
    lines.push_back({line, static_cast<int16_t>(lines.empty() ? firstIndent : 0), lineHyphen, last, false});
    line.clear();
    lineW = 0;
    lineHyphen = false;
  };

  for (size_t wi = 0; wi < words.size();) {
    // Only the line still being built as the paragraph's first one pays the indent.
    const int lineMaxW = lines.empty() ? maxW - firstIndent : maxW;
    const std::string word = words[wi];
    const int wW = r.getTextWidth(fontId, word.c_str());
    const int need = (line.empty() ? 0 : spW) + wW;

    if (line.empty() || lineW + need <= lineMaxW) {
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
      const int avail = lineMaxW - lineW - spW - hyphenW;  // room for a prefix after a space + hyphen
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

  // Line pitch honors the reader's Line Spacing setting (Tight/Normal/Wide), the same compression the
  // page layout measures every line with (ChapterHtmlSlimParser::addLineToPage). Reading it here is
  // what keeps a paragraph's line rhythm — and the half-line paragraph gap derived from it below —
  // the same in the overlay as on the page under it.
  const int lh = renderer.getLineHeight(pageTranslationFontId, SETTINGS.getReaderLineCompression());
  const int spW = renderer.getSpaceWidth(pageTranslationFontId);
  constexpr int PAD = 10;
  const int maxTextW = viewportWidth - 2 * PAD;
  // Mirror the reader's "Extra Paragraph Spacing" setting: a gap between paragraphs when on,
  // otherwise paragraphs run flush and the first-line indent below marks the boundary instead. A
  // FULL line height — a genuine blank line — in the OVERLAY's own font and line rhythm.
  //
  // Deliberately not the page layout's `lineHeight / 2` (ChapterHtmlSlimParser::makePages). That
  // half-line is a gap between paragraphs that share one line grid, in the body font. This overlay
  // is a full-panel replacement composited over an erased rect (the fillRect above): it is not
  // line-registered with the page underneath, and its `lh` is the overlay font's, which may be a
  // size smaller. Half of a smaller line height reads as a seam rather than a paragraph break, so
  // matching the page's number here does not reproduce the page's appearance.
  const int paraSpacing = SETTINGS.extraParagraphSpacing ? lh : 0;
  // The other half of the same setting, and the ONLY source of the overlay's paragraph indent: the
  // reader's style-free rule (ParsedText::defaultFirstLineIndent — three space widths when the
  // spacing is off, none when it is on), measured in the font the overlay actually draws with.
  //
  // It is deliberately NOT the indent the page layout baked into this paragraph's first line. That
  // number is resolveFirstLineIndent()'s output, which mixes in the SOURCE paragraph's CSS
  // text-indent: on a book that declares `text-indent` (most of them) it reproduces the stylesheet
  // — 1.5em here, 0 there, per class — and with Extra Paragraph Spacing ON it is 0 for every
  // paragraph, so the overlay could never show an indent at all. The text on screen here is a
  // TRANSLATION: it carries no stylesheet, so the setting is the only thing that may decide.
  //
  // One value for the whole page: it depends only on the setting and the overlay font, so every
  // paragraph that BEGINS here is indented identically — the source stylesheet cannot make one
  // paragraph indent and the next one not. The single exception is a paragraph that flowed in from
  // the previous page, which is not a paragraph start at all and renders flush (DisplayPara::
  // startsHere), exactly as the body's continuation lines do.
  int paraIndent =
      ParsedText::defaultFirstLineIndent(renderer, pageTranslationFontId, SETTINGS.extraParagraphSpacing != 0);
  if (paraIndent > maxTextW / 2) paraIndent = maxTextW / 2;  // sanity net; never leaves line 0 unusable

  // Honor the reader's hyphenation toggle + paragraph alignment. The overlay shows the *translated*
  // text, so route hyphenation through the translation's target-language slot (falls back to
  // generic breaks when that language has no trie).
  const bool hyphenate = SETTINGS.hyphenationEnabled != 0;
  if (hyphenate && SETTINGS.translationLanguage != 0xFF &&
      SETTINGS.translationLanguage < LanguagePickerActivity::NUM_LANGUAGES) {
    Hyphenator::setTranslatedLanguage(LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].code);
  }
  const uint8_t align = SETTINGS.paragraphAlignment;

  // Break every paragraph into lines ONCE; reuse for both the scroll metrics and drawing so they
  // can't diverge.
  std::vector<std::vector<PageTranslationLine>> paras;
  paras.reserve(pageParagraphs.size());
  const char* const marker = tr(STR_NO_TRANSLATION);
  for (const auto& para : pageParagraphs) {
    const int firstIndent = para.startsHere ? paraIndent : 0;
    auto lines = breakParagraph(renderer, pageTranslationFontId, para.text, maxTextW, firstIndent, spW, hyphenate);
    if (!para.translated) {
      // Option C: the body already holds the SOURCE text; append one short dim
      // marker line so the untranslated gap is visible but unobtrusive.
      PageTranslationLine mk;
      mk.content = marker;
      mk.lastInParagraph = true;
      mk.dim = true;
      lines.push_back(std::move(mk));
    }
    paras.push_back(std::move(lines));
  }

  if (scrollOffset < 0) scrollOffset = 0;

  // One pass: draw the lines that fit entirely in the viewport and, at the same time, publish the
  // two line-top scroll targets handleInput() steps to.
  //   • nextScrollOffset: the first line NOT fully visible in this window. It becomes the next
  //     window's first line, so the line skipped at the bottom edge below is never lost. -1 => the
  //     rest of the content fits, and the next press closes the overlay.
  //   • prevScrollOffset: the FIRST line whose own screenful would reach the current window's top —
  //     i.e. the furthest-back window that leaves no hole between it and where we are now.
  // scrollOffset is always one of these (or 0), hence always a line top, hence a drawn line can only
  // ever be cut off at the BOTTOM edge — never at the top, and never spilling out of the panel over
  // the status bar.
  const int clipTop = yOffset;
  const int clipBottom = yOffset + viewportHeight;
  int nextOff = -1;
  int prevOff = -1;
  int contentY = 0;
  for (const auto& lines : paras) {
    for (const auto& ln : lines) {
      const int y = yOffset + contentY - scrollOffset;
      if (y >= clipTop && y + lh <= clipBottom) {
        drawPageTranslationLine(renderer, pageTranslationFontId, ln, xOffset + PAD + ln.indent, y, maxTextW - ln.indent,
                                spW, align);
      }
      if (prevOff < 0 && contentY + viewportHeight >= scrollOffset) prevOff = contentY;
      // `contentY > scrollOffset` only matters in the degenerate case of a line taller than the whole
      // viewport: without it the target could be the window's own first line and a press would move
      // nothing. Forward progress is then always at least one line.
      if (nextOff < 0 && contentY > scrollOffset && contentY + lh > scrollOffset + viewportHeight) nextOff = contentY;
      contentY += lh;
    }
    contentY += paraSpacing;
  }
  nextScrollOffset = static_cast<int16_t>(nextOff);
  prevScrollOffset = static_cast<int16_t>(prevOff);
}

// ── Font helper ──────────────────────────────────────────────────────────────
//
// Fork parity: the Page Translation overlay renders in the same reader-derived FAMILY as the tooltip,
// so it honors the reader's Font Family and Font Size settings. It previously returned a fixed
// UI_12_FONT_ID, which ignored both (that predated the Tooltip port that brought getTooltipFontId in).
//
// The SIZE, however, is this mode's own setting (pageTranslationSize), not the tooltip's: the two
// overlays are different reading gestures — a per-sentence popup beside the source line versus a
// full-page replacement — and a user who wants the popup small has said nothing about the full page.
// It was briefly one shared field, which made either row silently retune the other. Both are
// composited at view time, so neither can reach the section cache key.
//
// 0 (Same, or a family with no smaller face) becomes the body font, keeping renderOverlayFrame's
// single-prewarm fast path intact — same contract as getTooltipFontId().

int getPageTranslationFontId() {
  const int translationFontId = SETTINGS.getPageTranslationOverlayFontId();
  return translationFontId != 0 ? translationFontId : SETTINGS.getReaderFontId();
}
