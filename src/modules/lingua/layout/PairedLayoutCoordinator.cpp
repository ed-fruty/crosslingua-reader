#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <Epub/parsers/ParagraphBoundary.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
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
void ChapterHtmlSlimParser::flushBufferedOriginal() {
  if (!bufferedOriginalBlock) return;

  // Swap the buffered original into currentTextBlock so makePages() lays it out, and point the
  // block-level paragraph state at the buffered original so both the marker guard and the emitted
  // PageLines' paragraphIdx match it. Save/restore the caller's in-flight state around the swap.
  auto savedBlock = std::move(currentTextBlock);
  const int16_t savedParagraphIdx = currentBlockParagraphIdx;
  const bool savedIsTranslated = currentBlockIsTranslated;
  // The footnote ledger travels with the block for the same reason the paragraph index does, and it is
  // load-bearing HERE above all: the in-flight block that TRIGGERED this flush (the next original) has
  // usually already pushed anchors of its own, and its indices restart from base 0 exactly as the
  // buffered block's did. Left in the same ledger, the low-index entries of the block that has not been
  // laid out yet satisfy addLineToPage's `first <= wordsExtractedInBlock` test on one of the buffered
  // block's first lines and were delivered to ITS page, then erased — so the next paragraph's marker
  // appeared one paragraph early, or (once the net below cleared the remainder) not at all.
  FootnoteLedger parkedFootnotes = adoptBufferedFootnoteLedger();

  currentTextBlock = std::move(bufferedOriginalBlock);
  currentBlockParagraphIdx = bufferedOriginalParagraphIdx;
  currentBlockIsTranslated = false;

  appendSideBySideNoTranslationMarkerIfUnpaired();
  makePages();

  releaseFootnoteLedger(parkedFootnotes);
  currentTextBlock = std::move(savedBlock);
  currentBlockParagraphIdx = savedParagraphIdx;
  currentBlockIsTranslated = savedIsTranslated;
}

// Lingua (SideBySide, mode 5): lay a SOURCE block and its paired TRANSLATION block into
// two half-width columns, emitted as lockstep PageLine rows sharing one yPos and advancing one
// lineHeight per row. Both columns stamp the original paragraph's index
// (bufferedOriginalParagraphIdx) so the Page Translation overlay's line->paragraph mapping still
// resolves. RTL is handled per-word inside each half-width line by layoutAndExtractLines.
//
// SOURCE vs LEFT: today the source is the physically left column and the translation the right one,
// but the two are NOT the same thing and the code below never treats them as such. Everything
// keyed to the content — the footnote drain, the block style, the paragraph index — follows the
// SOURCE stream by name; only sourceColX / transColX are physical. A future column-order setting
// then only has to swap those two x values.

void ChapterHtmlSlimParser::flushPendingFootnotesToCurrentPage() {
  if (pendingFootnotes.empty()) return;
  if (currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
  }
  // Cleared either way. An entry left behind here would be drained against the NEXT block's word base,
  // which is precisely the misattribution this net exists to prevent. Every caller has a live
  // currentPage by construction — makePages and both custom emitters create one before laying a single
  // line — so the undelivered case is unreachable in practice.
  pendingFootnotes.clear();
}

ChapterHtmlSlimParser::FootnoteLedger ChapterHtmlSlimParser::adoptBufferedFootnoteLedger() {
  FootnoteLedger parked;
  parked.wordBase = wordsExtractedInBlock;
  // Swaps, never copies: an entry lives in exactly one ledger at every instant, so nothing can be
  // delivered twice or lost in transit. Both swaps are O(1) and recycle the two buffers between the
  // ledgers instead of allocating, so a chapter of footnoted paragraphs allocates once.
  parked.pending.swap(pendingFootnotes);
  pendingFootnotes.swap(bufferedOriginalFootnotes);
  wordsExtractedInBlock = bufferedOriginalWordsExtracted;
  return parked;
}

void ChapterHtmlSlimParser::releaseFootnoteLedger(FootnoteLedger& parked) {
  // Anything still pending belongs to the buffered block and had no line of its own to land on (its
  // layout produced fewer lines than its anchors need, or bailed out before emitting any), so give it
  // the same best-effort page the end-of-block net gives every such entry rather than letting it cross
  // into the in-flight block's ledger. Normally a no-op: makePages and both custom emitters run that
  // net themselves before returning.
  flushPendingFootnotesToCurrentPage();
  bufferedOriginalFootnotes.swap(pendingFootnotes);
  bufferedOriginalWordsExtracted = 0;
  pendingFootnotes.swap(parked.pending);
  wordsExtractedInBlock = parked.wordBase;
}

void ChapterHtmlSlimParser::makePagesForTrailingBlock() {
  // Flush the trailing outermost block through the pairing builder that owns this layout. A trailing
  // original is buffered (not laid out) there, so drain any block still buffered -- the last original
  // of the chapter -- full-width via flushBufferedOriginal (which appends the italic "not translated"
  // marker under SideBySide only; Interlinear just shows the source).
  if (linguaLayout != LinguaLayout::SideBySide && linguaLayout != LinguaLayout::Interlinear) {
    makePages();
    return;
  }
  if (linguaLayout == LinguaLayout::SideBySide) {
    makePagesTableMode();
  } else {
    makePagesInterlinearMode();
  }
  if (bufferedOriginalBlock) {
    flushBufferedOriginal();
  }
}
