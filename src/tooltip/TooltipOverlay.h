#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include <string>

#include "SentenceSplitter.h"

class TooltipOverlay {
 public:
  // Set the path to the .translated.html file for the current chapter.
  void setTranslatedHtmlPath(const std::string& path) { translatedHtmlPath = path; }

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

  std::string translatedHtmlPath;

  // Cached sentence data for the current page.
  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;

  // Translated paragraph texts extracted from .translated.html (kept alive for pointer stability).
  static constexpr int MAX_PARAGRAPHS = 60;
  std::string translatedParagraphs[MAX_PARAGRAPHS];
  int translatedParagraphCount = 0;

  // Original paragraph texts extracted from the page (for matching to translations).
  std::string originalParagraphs[MAX_PARAGRAPHS];
  int originalParagraphCount = 0;

  // Translated text split into words for sentence mapping.
  static constexpr int MAX_TRANS_WORDS = 500;
  std::string transWordStorage[MAX_TRANS_WORDS];
  const char* transWordPtrs[MAX_TRANS_WORDS];
  int transWordCount = 0;

  // Sentence split result for the current page.
  SentenceSplitResult splits;

  // Prepare sentence data from the page + translated HTML (called once per page).
  void preparePage(const Page& page);

  // Extract translated paragraphs from the .translated.html file.
  void loadTranslationsFromHtml();

  // Match page paragraphs to chapter translations and build translated word arrays.
  void matchPageTranslations(const Page& page);

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

// Get the font ID one size smaller than the current reader font.
int getTooltipFontId();
