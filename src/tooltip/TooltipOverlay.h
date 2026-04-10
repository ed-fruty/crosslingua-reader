#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include "SentenceSplitter.h"

class TooltipOverlay {
 public:
  bool handleInput(MappedInputManager& input);

  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  void onPageChanged();

  bool isActive() const { return currentSentenceIndex >= 0; }

 private:
  int8_t currentSentenceIndex = -1;
  bool wrapAround = false;
  bool pagePrepared = false;

  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;
  const char* transWordPtrs[MAX_WORDS];
  int transWordCount = 0;

  SentenceSplitResult splits;

  void preparePage(const Page& page);

  struct SentenceBounds {
    int firstLineY;
    int startX;
    int endX;
  };
  SentenceBounds findSentenceBounds(const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                                    int yOffset) const;

  void drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span, int fontId,
                             int xOffset, int yOffset) const;
};

int getTooltipFontId();
