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
              int viewportWidth, int viewportHeight, int pageIndex, int pageCount);

  void onPageChanged();

  bool isActive() const { return currentSentenceIndex >= 0; }

 private:
  int8_t currentSentenceIndex = -1;
  bool wrapAround = false;
  bool pagePrepared = false;

  std::string translatedHtmlPath;

  // Original words from the page (CT_NO_RENDER = all words are original).
  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;

  // Translated words extracted from HTML for this page's slice.
  std::vector<std::string> transWordStorage;
  const char* transWordPtrs[MAX_WORDS];
  int transWordCount = 0;

  SentenceSplitResult splits;

  void preparePage(const Page& page, int pageIndex, int pageCount);

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
