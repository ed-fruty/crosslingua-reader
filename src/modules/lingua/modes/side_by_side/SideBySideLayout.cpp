#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <Epub/parsers/ParagraphBoundary.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "fontIds.h"
#include "modules/lingua/services/TranslatedContentDetector.h"
void ChapterHtmlSlimParser::makePagesTableMode() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  if (currentBlockIsTranslated) {
    // Translation paragraph: pair it beside the buffered original if one is waiting, otherwise
    // fall back to a full-width layout (a translation with no preceding original — unusual).
    if (bufferedOriginalBlock) {
      // renderSideBySide drains against the SOURCE column, i.e. the buffered original, so that block's
      // ledger is the one that must be installed while it runs. A sidecar translation cannot itself
      // carry an anchor (see the drain comment there), but a book whose OWN markup marks a paragraph
      // with a differing lang= can, and its indices restart from a base of their own — parking them
      // keeps them out of the source column's drain, and the ledger swap also pins the word BASE, which
      // is otherwise whatever the translation block left behind if it was large enough to soft-flush.
      FootnoteLedger parkedFootnotes = adoptBufferedFootnoteLedger();
      renderSideBySide(std::move(bufferedOriginalBlock), std::move(currentTextBlock));
      releaseFootnoteLedger(parkedFootnotes);
    } else {
      makePages();
    }
  } else {
    // Original paragraph: if a previous original is still buffered it never got a translation,
    // so lay it out full-width (with the italic marker) before buffering this one for pairing.
    if (bufferedOriginalBlock) {
      flushBufferedOriginal();
    }
    bufferedOriginalParagraphIdx = currentBlockParagraphIdx;
    // The block's footnote ledger goes into the buffer WITH it: its anchor indices are relative to its
    // own words, and startNewTextBlock is about to zero that base for the next block. The buffered
    // ledger is empty here by construction — flushBufferedOriginal above, and every other consumer of
    // the buffer, leaves it so — hence a swap: it hands this block's ledger over AND leaves the
    // in-flight one clean for the next block, with the two buffers recycled rather than reallocated.
    bufferedOriginalFootnotes.swap(pendingFootnotes);
    bufferedOriginalWordsExtracted = wordsExtractedInBlock;
    bufferedOriginalBlock = std::move(currentTextBlock);
  }
}

// Lingua (SideBySide, mode 5): lay out a buffered original that never received a paired
// translation. It renders full-width (via makePages) with the italic "not translated" marker inline.
void ChapterHtmlSlimParser::renderSideBySide(std::unique_ptr<ParsedText> sourceBlock,
                                             std::unique_ptr<ParsedText> transBlock) {
  if (!sourceBlock || !transBlock) return;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  // Both columns are laid out AND drawn in the body FONT: the pairing is what distinguishes the two
  // languages here, and shrinking one column would break the lockstep row geometry this loop relies
  // on (one shared yPos and one shared advance per row). The translation column is nonetheless
  // tagged LineFontRole::Translation below -- the role is what carries the COLOUR sub-setting, and
  // it costs nothing in fonts: LinguaReaderIntegration's translation slot is 0 under this mode
  // (getInterleavedTranslationFontId is gated to Interleaved), which PageFontSet maps back to the
  // body font. currentLineRole() still answers Body under SideBySide, and must: it serves the main
  // flow, where a translated block only ever lands via the soft-flush escape, not via this pairing.
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  const uint16_t gapWidth = static_cast<uint16_t>(viewportWidth * 0.04f);
  // Equal columns, measured identically -- which is exactly why the source/translation order is a
  // matter of x only and can never move a line break.
  const uint16_t colWidth = static_cast<uint16_t>((viewportWidth - gapWidth) / 2);
  // The two column origins, named by CONTENT and not by side. Today the source is the near column
  // and the translation the far one; a future column-order setting swaps exactly these two values
  // and nothing else in this function has to change.
  const int16_t sourceColX = 0;
  const int16_t transColX = static_cast<int16_t>(colWidth + gapWidth);

  // Lay each column out at half width into its own line vector. Paragraphs are short (one block
  // each), so a small reserve avoids the first few reallocs without over-committing DRAM.
  std::vector<std::unique_ptr<TextBlock>> sourceLines;
  std::vector<std::unique_ptr<TextBlock>> transLines;
  sourceLines.reserve(8);
  transLines.reserve(8);
  // Each source line's own offset into the chapter's visible text, kept 1:1 with sourceLines. This
  // is what a page is stamped with, so a reposition after a mode switch lands on the row the reader
  // was actually looking at. 4 bytes per line of ONE paragraph, released with the vector below --
  // a fixed stack array cannot serve it, since a paragraph's line count is unbounded.
  std::vector<uint32_t> sourceOffsets;
  sourceOffsets.reserve(8);
  // Words dropped to an arena OOM while either block was buffered: the parser's own latch only sees
  // currentTextBlock, so the paired blocks are checked here, before and after their layout.
  if (sourceBlock->hadDroppedWords() || transBlock->hadDroppedWords()) layoutOom = true;
  sourceBlock->layoutAndExtractLines(
      renderer, fontId, colWidth,
      [&sourceLines, &sourceOffsets](std::unique_ptr<TextBlock> line, const uint32_t offset) {
        sourceLines.push_back(std::move(line));
        sourceOffsets.push_back(offset);
      },
      /*includeLastLine=*/true, characterSpacing, wordSpacingPercent);
  transBlock->layoutAndExtractLines(
      renderer, fontId, colWidth,
      [&transLines](std::unique_ptr<TextBlock> line) { transLines.push_back(std::move(line)); },
      /*includeLastLine=*/true, characterSpacing, wordSpacingPercent);
  if (sourceBlock->hadDroppedWords() || transBlock->hadDroppedWords()) layoutOom = true;

  // Top spacing comes from the SOURCE block (vertical, so order-invariant).
  const BlockStyle& bs = sourceBlock->getBlockStyle();
  if (bs.marginTop > 0) currentPageNextY += bs.marginTop;
  if (bs.paddingTop > 0) currentPageNextY += bs.paddingTop;

  const size_t maxLines = std::max(sourceLines.size(), transLines.size());
  // Where the ROW sits in the chapter's visible text. NOT the parser's running visibleTextOffset:
  // this pairing runs at endElement, by which time that has advanced past the whole paragraph, so
  // using it stamped every page of a paired chapter with an offset from further down the text. A
  // translation column longer than its source contributes no text of its own, so its extra rows
  // keep the last source row's offset.
  uint32_t rowOffset = sourceOffsets.empty() ? visibleTextOffset : sourceOffsets.front();
  for (size_t i = 0; i < maxLines; i++) {
    if (i < sourceOffsets.size()) rowOffset = sourceOffsets[i];
    // Page-break check: v2 uses the 3-arg completePageFn + explicit page counter.
    if (currentPageNextY + lineHeight > viewportHeight) {
      setCurrentPageVisibleOffset(rowOffset);
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
      // Without this the flag stayed latched for the rest of the chapter, so every later page
      // reported the FIRST page's offset and a reposition jumped back to the chapter's start.
      currentPageVisibleOffsetSet = false;
    }
    setCurrentPageVisibleOffset(rowOffset);

    // FOOTNOTES, attributed to the page carrying the anchor exactly as addLineToPage (:1828-1834) does
    // it. SideBySide needs its own copy because a PAIRED paragraph never reaches addLineToPage, and
    // under this layout every translated paragraph pairs: without this, pendingFootnotes accumulated
    // for the whole chapter and was either dumped wholesale onto whatever page happened to be current at
    // the next unpaired paragraph (the end-of-block net, flushPendingFootnotesToCurrentPage) or never
    // delivered at all, since finishParse does not drain it.
    //
    // SOURCE column only -- semantically the source, not "the left one". The translation column is
    // written by the sidecar as a fresh block element holding nothing but the ESCAPED plain text of
    // the translation (TranslationHtmlRewriter's write-out loop, via appendEscaped — '<' becomes
    // "&lt;"), so it carries no <a epub:type="noteref"> and can never push a pending footnote, which
    // is why one counter over the SOURCE column's lines is the whole story here.
    //
    // The pending indices and this counter share the SOURCE block's base because the caller installed
    // that block's ledger (adoptBufferedFootnoteLedger) before calling: both the entries and the base
    // below are the buffered original's own, and the in-flight block's are parked out of reach. Relying
    // instead on "startNewTextBlock zeroed the counter and nothing advances it" was not sound — the soft
    // flush (:1452) advances it for any block over 750 words (320 with embedded CSS), and the entries of
    // an as-yet-unlaid block sat in the same list. The pre-layout anchor index vs post-layout
    // wordCount() mismatch is addLineToPage's own approximation, kept identical here.
    if (i < sourceLines.size()) {
      wordsExtractedInBlock += sourceLines[i]->wordCount();
      auto footnoteIt = pendingFootnotes.begin();
      while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
        currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
        ++footnoteIt;
      }
      pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

      // Moving the line into the page releases it from sourceLines, so a pair long enough to span
      // several pages keeps the makePages peak -- one page's worth of TextBlocks, freed as
      // onPageComplete serializes each page -- instead of pinning every line of both columns.
      auto sourceLine = makeUniqueNoThrow<PageLine>(std::move(sourceLines[i]), sourceColX, currentPageNextY);
      if (!sourceLine) {
        LOG_ERR("EHP", "OOM: PageLine");
      } else {
        sourceLine->paragraphIdx = bufferedOriginalParagraphIdx;
        // fontRole stays Body: this is the book's own text, and it must never take the translation
        // colour.
        currentPage->elements.push_back(std::move(sourceLine));
      }
    }
    if (i < transLines.size()) {
      auto transLine = makeUniqueNoThrow<PageLine>(std::move(transLines[i]), transColX, currentPageNextY);
      if (!transLine) {
        LOG_ERR("EHP", "OOM: PageLine");
      } else {
        transLine->paragraphIdx = bufferedOriginalParagraphIdx;
        // THE colour hook for this mode. The role resolves to the body font (see above), so this is
        // purely "which stream is this line", read back at draw time by PageFontSet::inkForRole to
        // colour the translation column and nothing else.
        transLine->fontRole = LineFontRole::Translation;
        currentPage->elements.push_back(std::move(transLine));
      }
    }

    // Both columns belong to the same original paragraph; keep the page's paragraph range current
    // (also fixes up firstParagraphIdx on a page freshly reset by the break above).
    if (bufferedOriginalParagraphIdx >= 0) {
      if (currentPage->firstParagraphIdx < 0) {
        currentPage->firstParagraphIdx = bufferedOriginalParagraphIdx;
      }
      currentPage->lastParagraphIdx = bufferedOriginalParagraphIdx;
    }

    currentPageNextY += lineHeight;
  }

  // Same end-of-block net makePages keeps: every entry in the installed ledger belongs to the SOURCE
  // column just emitted (the caller parked the in-flight block's ledger before this call), so flushing
  // it to the current page can never touch another paragraph's entry. The per-line drain above already
  // covers the normal case — an anchor index can never exceed the block's pre-layout word count, and
  // hyphenation only ADDS post-layout words — so this fires only when the source column produced fewer
  // lines than the anchors need (a line dropped to a TextBlock arena OOM).
  flushPendingFootnotesToCurrentPage();

  // Bottom spacing + the usual half-line paragraph gap after the pair.
  if (bs.marginBottom > 0) currentPageNextY += bs.marginBottom;
  if (bs.paddingBottom > 0) currentPageNextY += bs.paddingBottom;
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::appendSideBySideNoTranslationMarkerIfUnpaired() {
  // Only LinguaLayout::SideBySide surfaces the inline marker; every other layout either drops or
  // pairs content elsewhere.
  if (linguaLayout != LinguaLayout::SideBySide) return;
  // currentBlockIsTranslated reflects the most-recently-opened outermost block. If it was a
  // translation, the preceding original is already paired — no marker. If it was an original,
  // the caller has determined nothing will pair with it (next outermost block is also an
  // original, or we reached EOF), so it is unpaired.
  if (currentBlockIsTranslated) return;
  // No outermost block has opened yet (nothing to mark), or the block is empty/whitespace-only
  // (which the layout parser never counts as a paragraph — see ParagraphBoundary.h).
  if (currentBlockParagraphIdx < 0) return;
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  // Option C (unpaired original): append a short "not translated" marker inline after the source
  // text so the gap is visible but unobtrusive. RAM-cheap: a few extra addWord() calls on the block
  // already being flushed, no new buffers.
  //
  // ITALICS, not a gray level. The fork dimmed this marker and v2 first copied that by tagging it
  // EpdFontFamily::TRANSLATED, the per-word bit the renderer maps through
  // InterleavedMode::translatedWordInk(). That does
  // not work here, in either direction:
  //   * the bit is a MODE-wide switch. Turning it on for Side by Side also greys the source column
  //     of any book with inline lang= runs, every unpaired translation paragraph, the soft-flush
  //     escape, and the translation column of every chapter cached before it had a line role — see
  //     the leak list on InterleavedMode::translatedWordInk(). The marker cannot be singled out.
  //   * a LINE role, the vehicle the two Translation Colour sub-settings use, cannot reach it
  //     either: the marker deliberately shares its line with the source text it annotates.
  // Italic is per-word, is set nowhere else on this path, needs no grayscale pass (so it survives
  // Text Anti-Aliasing being off, which grey does not), and reads as editorial furniture in every
  // colour setting. EpdFontFamily::getFont() falls back to the regular face when a family ships no
  // italic, which is exactly the old appearance rather than a wrong one.
  const char* marker = tr(STR_NO_TRANSLATION);
  std::string markerWord;
  for (const char* p = marker;; ++p) {
    if (*p == ' ' || *p == '\0') {
      if (!markerWord.empty()) {
        currentTextBlock->addWord(markerWord, EpdFontFamily::ITALIC);
        markerWord.clear();
      }
      if (*p == '\0') break;
    } else {
      markerWord.push_back(*p);
    }
  }
}

// ── Lingua (LinguaLayout::Interlinear) ───────────────────────────────────
//
// Route one flushed outermost block to the interlinear builder. Identical buffering shape to
// makePagesTableMode (see there for why currentBlockIsTranslated / currentBlockParagraphIdx are
// still the FLUSHED block's state at this point).
