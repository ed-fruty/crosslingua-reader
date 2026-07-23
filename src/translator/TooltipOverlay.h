#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <string>
#include <vector>

#include "SentenceSplitter.h"

// Pre-Translation "Tooltip" display mode (PT_TOOLTIP). The reader lays the page out as
// original-only text (the ChapterHtmlSlimParser drops translated words in this mode, exactly like
// PT_MODAL / PT_ORIGINAL_ONLY); this overlay then surfaces the translation of ONE sentence at a
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

  bool isActive() const { return currentSentenceIndex >= 0; }

  // Set by handleInput when "Page Turn" tooltip behavior reaches a page boundary.
  // EpubReaderActivity checks these after handleInput and triggers the page turn.
  bool pendingPageForward = false;
  bool pendingPageBack = false;
  bool activateOnNextPage = false;  // after page turn, auto-activate tooltip
  bool activateFromEnd = false;     // start from last sentence (back page turn)

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

  void drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                             int yOffset) const;
};

int getTooltipFontId();
