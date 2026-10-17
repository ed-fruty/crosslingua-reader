// LinguaLayout::WordLens: the ChapterHtmlSlimParser half of the Word Lens mode.
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/Page.h"
#include "modules/lingua/modes/word_lens/WordLensStrip.h"
#include "modules/lingua/wordlens/LensText.h"

namespace {

using lingua::wordlens::StripGloss;

// Longest run of glued tokens taken as one word ("wh" + "ispered" across an inline tag).
constexpr size_t MAX_GROUP_BYTES = 64;
constexpr uint16_t MAX_GROUP_TOKENS = 4;

struct MeasureCtx {
  const GfxRenderer* renderer;
  int fontId;
};

int measureAnnotation(void* ctx, const char* text) {
  const auto* m = static_cast<const MeasureCtx*>(ctx);
  return m->renderer->getTextAdvanceX(m->fontId, text, EpdFontFamily::REGULAR);
}

// Advance of the first `bytes` bytes of `word` in the body face. A word longer than the scratch
// buffer (far past any glossed word) is measured whole.
int prefixAdvance(const GfxRenderer& renderer, const int fontId, const char* word, const size_t bytes,
                  const EpdFontFamily::Style style, const int8_t tracking) {
  if (bytes == 0) return 0;
  char buf[MAX_GROUP_BYTES + 1];
  if (bytes > MAX_GROUP_BYTES) return renderer.getTextAdvanceX(fontId, word, style, tracking);
  const size_t n = bytes;
  std::memcpy(buf, word, n);
  buf[n] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, style, tracking);
}

// Byte range of the letters of `word`, leaving out the punctuation around them, so a gloss centres
// over "whispered" rather than over "“whispered,".
void letterSpan(const char* word, size_t& first, size_t& end) {
  first = std::strlen(word);
  end = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(word);
  const auto* start = p;
  while (*p) {
    const size_t at = static_cast<size_t>(p - start);
    const uint32_t cp = utf8NextCodepoint(&p);
    if (lingua::wordlens::isWordCodepoint(cp)) {
      if (at < first) first = at;
      end = static_cast<size_t>(p - start);
    }
  }
  if (end <= first) {
    first = 0;
    end = std::strlen(word);
  }
}

}  // namespace

void ChapterHtmlSlimParser::makePagesWordLens(const bool includeLastLine) {
  if (currentTextBlock->hadDroppedWords()) layoutOom = true;
  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("WLN", "OOM: Page");
      layoutOom = true;
      return;
    }
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  ParsedText& block = *currentTextBlock;
  const BlockStyle blockStyle = block.getBlockStyle();
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  // STEP 1 -- decide the hard words on the PRE-layout token stream, where glued tokens can still be
  // joined back into one word and nothing has been hyphenated yet.
  wordLensPending.clear();
  wordLensGlossText.clear();
  {
    char group[MAX_GROUP_BYTES + 1];
    WordLensGloss gloss;
    const size_t tokens = block.size() < UINT16_MAX ? block.size() : UINT16_MAX;
    std::string_view previous;
    size_t i = 0;
    while (i < tokens) {
      size_t len = 0;
      uint16_t count = 0;
      bool fits = true;
      bool annotated = false;
      do {
        const std::string_view part = block.wordViewAt(i + count);
        if (len + part.size() > MAX_GROUP_BYTES) fits = false;
        if (fits) {
          std::memcpy(group + len, part.data(), part.size());
          len += part.size();
        }
        // A word carrying real <ruby> keeps its own annotation.
        if ((block.getWordStyleAt(i + count) & EpdFontFamily::RUBY_CONTINUE) != 0 ||
            !block.getRubyTextAt(i + count).empty()) {
          annotated = true;
        }
        count++;
      } while (i + count < tokens && block.wordAttachesToPrevious(i + count) && count < MAX_GROUP_TOKENS);
      group[len] = '\0';
      const bool sentenceInitial = previous.empty() ? wordsExtractedInBlock == 0
                                                    : lingua::wordlens::endsSentence(previous.data(), previous.size());
      if (fits && !annotated && wordLensSource->glossFor(group, len, sentenceInitial, gloss)) {
        const size_t textLength = std::strlen(gloss.text);
        if (wordLensPending.empty()) wordLensPending.reserve(16);
        wordLensPending.push_back({static_cast<uint16_t>(i), count, gloss.rank,
                                   static_cast<uint32_t>(wordLensGlossText.size()), static_cast<uint8_t>(textLength)});
        wordLensGlossText.append(gloss.text, textLength);
      }
      previous = block.wordViewAt(i + count - 1);
      i += count;
    }
  }

  // STEP 2 -- lay the block out exactly as makePages() would; the hard words only ride along as
  // tracked words, which report the line and x each one landed on and constrain nothing.
  std::vector<uint16_t> tracked;
  tracked.reserve(wordLensPending.size());
  for (const WordLensPending& p : wordLensPending) tracked.push_back(p.token);
  block.setTrackedWords(std::move(tracked));
  std::vector<ParsedText::TrackedWordPos> positions;
  std::vector<std::unique_ptr<TextBlock>> lines;
  std::vector<uint32_t> offsets;
  lines.reserve(8);
  offsets.reserve(8);
  block.layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [&lines, &offsets](std::unique_ptr<TextBlock> line, const uint32_t offset) {
        lines.push_back(std::move(line));
        offsets.push_back(offset);
      },
      includeLastLine, characterSpacing, wordSpacingPercent, &positions);
  block.setTrackedWords({});
  if (block.hadDroppedWords()) layoutOom = true;
  if (positions.size() != wordLensPending.size()) positions.clear();

  // STEP 3 -- one strip per line, the line's glosses centred over their words.
  const int annotationFont = fontIdForRole(LineFontRole::Annotation);
  const int stripHeight = renderer.getLineHeight(annotationFont, lineCompression);
  const int bodyLineHeight = renderer.getLineHeight(fontId, lineCompression);
  const int bodyAscender = renderer.getFontAscenderSize(fontId);
  const int minGap = renderer.getSpaceWidth(annotationFont, EpdFontFamily::REGULAR);
  MeasureCtx measure{&renderer, annotationFont};
  BlockStyle stripStyle;
  stripStyle.alignment = CssTextAlign::Left;

  std::vector<StripGloss> strip;
  // x of every hard word of the line as laid out, glossed or not: written into the strip as empty
  // marker words, so the reader's word navigation finds exactly the words this layout chose.
  std::vector<int16_t> markers;
  size_t nextPending = 0;
  for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
    std::unique_ptr<TextBlock>& line = lines[lineIndex];
    if (!line) continue;

    // The glosses of this line: positions are in token order, and lines in order, so one cursor
    // walks both. A tracked word whose line was dropped (OOM) or not reached reports NOT_PLACED.
    strip.clear();
    markers.clear();
    while (nextPending < positions.size() && (positions[nextPending].line == ParsedText::TrackedWordPos::NOT_PLACED ||
                                              positions[nextPending].line <= lineIndex)) {
      const ParsedText::TrackedWordPos pos = positions[nextPending];
      const WordLensPending& pending = wordLensPending[nextPending];
      nextPending++;
      if (pos.line != lineIndex || line->wordCount() == 0) continue;
      // The laid-out word the token became: the one starting at the reported x.
      uint16_t w = 0;
      int best = INT32_MAX;
      for (uint16_t k = 0; k < line->wordCount(); k++) {
        const int d = std::abs(line->wordXpos(k) - pos.x);
        if (d < best) {
          best = d;
          w = k;
        }
      }
      const uint16_t last = static_cast<uint16_t>(std::min<int>(w + pending.tokenCount - 1, line->wordCount() - 1));
      const int8_t tracking = line->getBlockStyle().characterSpacing;
      size_t first = 0;
      size_t end = 0;
      letterSpan(line->wordText(w), first, end);
      const int x0 =
          line->wordXpos(w) + prefixAdvance(renderer, fontId, line->wordText(w), first, line->wordStyle(w), tracking);
      letterSpan(line->wordText(last), first, end);
      const int x1 = line->wordXpos(last) +
                     prefixAdvance(renderer, fontId, line->wordText(last), end, line->wordStyle(last), tracking);
      if (strip.empty()) {
        strip.reserve(8);
        markers.reserve(8);
      }
      markers.push_back(line->wordXpos(w));
      strip.emplace_back();
      StripGloss& g = strip.back();
      g.wordX = static_cast<int16_t>(x0);
      g.wordWidth = static_cast<int16_t>(x1 > x0 ? x1 - x0 : 0);
      g.rank = pending.rank;
      const size_t n = std::min<size_t>(pending.textLength, sizeof(g.text) - 1);
      std::memcpy(g.text, wordLensGlossText.data() + pending.textOffset, n);
      g.text[n] = '\0';
    }

    std::unique_ptr<TextBlock> stripBlock;
    if (!strip.empty()) {
      lingua::wordlens::placeStripGlosses(strip.data(), strip.size(), effectiveWidth, minGap, measureAnnotation,
                                          &measure);
      std::vector<std::string> words;
      std::vector<int16_t> xs;
      std::vector<EpdFontFamily::Style> styles;
      words.reserve(strip.size() + markers.size());
      xs.reserve(strip.size() + markers.size());
      styles.reserve(strip.size() + markers.size());
      // Left to right, so the strip reads in order for anything that walks its words.
      for (int16_t x = INT16_MIN;;) {
        const StripGloss* next = nullptr;
        for (const StripGloss& g : strip) {
          if (g.placed && g.x > x && (!next || g.x < next->x)) next = &g;
        }
        if (!next) break;
        words.emplace_back(next->text);
        xs.push_back(next->x);
        styles.push_back(EpdFontFamily::REGULAR);
        x = next->x;
      }
      // Then the markers: empty words, which draw nothing. See WordLensNavigator::collect.
      for (const int16_t markerX : markers) {
        words.emplace_back();
        xs.push_back(markerX);
        styles.push_back(EpdFontFamily::REGULAR);
      }
      if (!words.empty()) {
        stripBlock = makeUniqueNoThrow<TextBlock>(words, xs, styles, std::vector<uint8_t>{}, std::vector<uint16_t>{},
                                                  stripStyle);
        if (stripBlock && !stripBlock->valid()) stripBlock.reset();
        if (!stripBlock) LOG_ERR("WLN", "Strip row lost (OOM)");
      }
    }

    // Top spacing before the block's first line, as makePages() applies it.
    if (wordsExtractedInBlock == 0) {
      if (blockStyle.marginTop > 0) currentPageNextY += blockStyle.marginTop;
      if (blockStyle.paddingTop > 0) currentPageNextY += blockStyle.paddingTop;
    }
    // A line with no hard word keeps its blank strip (even pitch) unless the reader chose more text
    // per page instead.
    const int lineStrip = (markers.empty() && !wordLensSource->keepEmptyStrips()) ? 0 : stripHeight;
    // Strip and line are one group: a strip never ends a page with its line on the next one.
    const int lineHeight = bodyLineHeight + line->getRubyShift(bodyAscender);
    if (!currentPage->elements.empty() && currentPageNextY + lineStrip + lineHeight > viewportHeight) {
      setCurrentPageVisibleOffset(offsets[lineIndex]);
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentPage.reset(new (std::nothrow) Page());
      if (!currentPage) {
        LOG_ERR("WLN", "OOM: Page");
        layoutOom = true;
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }
    if (stripBlock) {
      auto pageLine = makeUniqueNoThrow<PageLine>(std::move(stripBlock), blockStyle.leftInset(),
                                                  static_cast<int16_t>(currentPageNextY));
      if (pageLine) {
        stampLinguaLine(*pageLine, LineFontRole::Annotation);
        currentPage->elements.push_back(std::move(pageLine));
      } else {
        LOG_ERR("WLN", "OOM: strip PageLine");
      }
    }
    currentPageNextY += lineStrip;
    addLineToPage(std::move(line), offsets[lineIndex], LineFontRole::Body);
  }

  // Trailing spacing and footnotes exactly as makePages() finishes a block.
  if (includeLastLine) {
    if (!pendingFootnotes.empty() && currentPage) {
      for (const auto& [idx, fn] : pendingFootnotes) currentPage->addFootnote(fn.number, fn.href);
      pendingFootnotes.clear();
    }
    if (blockStyle.marginBottom > 0) currentPageNextY += blockStyle.marginBottom;
    if (blockStyle.paddingBottom > 0) currentPageNextY += blockStyle.paddingBottom;
    if (extraParagraphSpacing) currentPageNextY += bodyLineHeight / 2;
  }
}
