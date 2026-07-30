#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <string>
#include <vector>

#include "SentencePairing.h"
#include "SentenceSplitter.h"

// One TOOLTIP STEP == one translation unit. When the translation engine merges K
// consecutive source sentences into a SINGLE translated sentence, those K source
// sentences all resolve to the SAME translation string; grouping them into one step
// (groupTranslationSteps, SentencePairing.h) makes stepping advance per translation —
// never showing the identical tooltip K times — and lets the underline span all K
// source sentences at once. [firstSentence..lastSentence] are inclusive indices into
// the page's sentence spans (SentenceSplitResult::spans).
//
// The step type itself is shared: LinguaLayout::Interlinear groups sentences by the same rule to decide
// how many annotation rows a paragraph gets, so both consumers name one type (SentencePairing.h).
using TooltipStep = SentenceStep;

// Lingua "Tooltip" display mode (LINGUA_TOOLTIP). The reader lays the page out as
// original-only text (the ChapterHtmlSlimParser drops translated words in this mode, exactly like
// LINGUA_PAGE_TRANSLATION / LINGUA_ORIGINAL_ONLY); this overlay then surfaces the translation of ONE sentence at a
// time as a small popup near the selected sentence, which is underlined on the page.
//
// Translation data is NOT read from the section.bin cache — it is re-parsed on demand from the
// translated chapter HTML (setTranslatedHtmlPath). Only the current page's paragraph range is
// parsed so peak RAM stays bounded regardless of chapter length.
//
// Ported from the upstream fork (src/tooltip/TooltipOverlay), adapted to v2: HalStorage/HalFile
// (never raw FsFile), the flattened TextBlock accessors (wordCount/wordText/wordXpos/wordStyle
// instead of the fork's getWords()/getWordXpos()/getWordStyles() containers), and v2's drawText
// signature (no grayLevel parameter).
class TooltipOverlay {
 public:
  void setTranslatedHtmlPath(const std::string& path) { translatedHtmlPath = path; }

  bool handleInput(MappedInputManager& input);

  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  void onPageChanged();

  // Prepare this page's per-sentence translations (idempotent with render()'s own preparePage) and
  // append every string the tooltip will draw for this page — all sentence translations plus the
  // STR_NO_TRANSLATION marker — into `out`. Lets the caller prewarm the tooltip font's glyph cache
  // ONCE per page (see EpubReaderActivity::renderOverlayFrame) instead of taking a per-glyph SD read
  // on every sentence step. Text only: does no rendering and no SD I/O of its own.
  void collectPageGlyphText(const Page& page, std::string& out);

  bool isActive() const { return currentStepIndex >= 0; }

  // Set by handleInput when "Page Turn" tooltip behavior reaches a page boundary.
  // EpubReaderActivity checks these after handleInput and triggers the page turn.
  bool pendingPageForward = false;
  bool pendingPageBack = false;
  bool activateOnNextPage = false;  // after page turn, auto-activate tooltip
  bool activateFromEnd = false;     // start from last sentence (back page turn)

 private:
  // Index of the current TOOLTIP STEP (translation unit) in steps[], NOT a raw source
  // sentence index. Stepping moves per translation, so a merged group of source
  // sentences is one stop. -1 = inactive.
  int8_t currentStepIndex = -1;
  int8_t skipDirection = 1;  // 1=forward, -1=backward (picks the start step after a page turn)
  bool pagePrepared = false;

  std::string translatedHtmlPath;

  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;

  SentenceSplitResult splits;
  // One entry per split source sentence. A merged group holds the SAME string in every
  // member slot; grouping (below) collapses those into a single navigable step.
  std::vector<std::string> sentenceTranslations;

  // Navigation steps: each is a maximal run of consecutive source sentences that display
  // the SAME translation. stepCount <= splits.count <= MAX_SENTENCES, so this is O(page)
  // and never heap-allocates.
  TooltipStep steps[MAX_SENTENCES];
  int stepCount = 0;

  void preparePage(const Page& page);

  struct SentenceBounds {
    int firstLineY;
    int startX;
    int endX;
  };
  SentenceBounds findSentenceBounds(const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                                    int yOffset) const;

  void drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                             int yOffset) const;
};

int getTooltipFontId();
