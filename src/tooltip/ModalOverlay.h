#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include <string>
#include <vector>

class ModalOverlay {
 public:
  // Public because static XML callbacks in the .cpp need access.
  struct ParagraphEntry {
    std::string key;          // first N words of original paragraph, normalized
    std::string translation;  // full translated paragraph text
  };
  static std::string paragraphKey(const char* const* words, int count);

  void setTranslatedHtmlPath(const std::string& path);

  bool handleInput(MappedInputManager& input);

  void render(GfxRenderer& renderer, const Page& page, int fontId, int modalFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  void onPageChanged();
  void onSectionChanged();

  bool isActive() const { return active; }

 private:
  bool active = false;
  int16_t scrollOffset = 0;
  int16_t totalContentHeight = 0;
  bool pagePrepared = false;
  bool sectionParsed = false;
  int16_t cachedViewportHeight = 0;
  int16_t cachedLineHeight = 0;

  std::string translatedHtmlPath;

  // Chapter-level: all (original_key, translation) pairs from HTML.
  std::vector<ParagraphEntry> chapterParagraphs;

  // Page-level: translations for paragraphs visible on current page.
  std::vector<std::string> pageTranslations;

  void parseChapterHtml();
  void preparePage(const Page& page);

  static int measureParagraphHeight(GfxRenderer& renderer, int fontId, const char* text, int maxW, int lh, int spW);
  static int drawParagraph(GfxRenderer& renderer, int fontId, const char* text, int x, int y, int maxW, int lh,
                           int spW, int clipTop, int clipBottom);
};

int getModalFontId();
