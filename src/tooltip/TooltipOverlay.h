#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include <string>
#include <vector>

#include "SentenceSplitter.h"

class TooltipOverlay {
 public:
  void setTranslatedHtmlPath(const std::string& path) { translatedHtmlPath = path; }

  bool handleInput(MappedInputManager& input);

  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  void onPageChanged();

  bool isActive() const { return currentSentenceIndex >= 0; }

  // Set by handleInput when "Page Turn" tooltip behavior reaches a page boundary.
  // EpubReaderActivity checks these after handleInput and triggers the page turn.
  bool pendingPageForward = false;
  bool pendingPageBack = false;
  bool activateOnNextPage = false;   // after page turn, auto-activate tooltip
  bool activateFromEnd = false;      // start from last sentence (back page turn)

 private:
  int8_t currentSentenceIndex = -1;
  int8_t skipDirection = 1;  // 1=forward, -1=backward (for auto-skip)
  bool pagePrepared = false;

  std::string translatedHtmlPath;

  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;

  SentenceSplitResult splits;
  std::vector<std::string> sentenceTranslations;  // one per split sentence

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
