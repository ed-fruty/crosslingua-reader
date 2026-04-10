#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include "SentenceSplitter.h"

class TooltipOverlay {
 public:
  // Returns true if input was consumed (caller should skip normal page-turn handling).
  bool handleInput(MappedInputManager& input);

  // Render the tooltip overlay on top of the already-drawn page.
  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  // Reset tooltip state when page changes.
  void onPageChanged();

  // Check if tooltip is currently visible.
  bool isActive() const { return currentSentenceIndex >= 0; }

 private:
  int8_t currentSentenceIndex = -1;
  bool wrapAround = false;
  bool pagePrepared = false;

  // Cached sentence data for the current page.
  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;
  const char* transWordPtrs[MAX_WORDS];
  int transWordCount = 0;

  // Sentence split result for the current page.
  SentenceSplitResult splits;

  // Prepare sentence data from the page's TextBlocks (called once per page).
  void preparePage(const Page& page);

  // Find the screen Y coordinate and X bounds for a sentence span.
  struct SentenceBounds {
    int firstLineY;
    int startX;
    int endX;
  };
  SentenceBounds findSentenceBounds(const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                                    int yOffset) const;

  // Draw underline under all lines of the active sentence.
  void drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span, int fontId,
                             int xOffset, int yOffset) const;
};

// Get the font ID one size smaller than the current reader font.
int getTooltipFontId();
