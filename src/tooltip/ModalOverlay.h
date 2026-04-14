#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include <string>
#include <vector>

class ModalOverlay {
 public:
  // Public because static XML callbacks in the .cpp need access.
  struct ParagraphPair {
    std::string original;     // full original paragraph text
    std::string translation;  // full translated paragraph text
  };

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
  int16_t cachedViewportHeight = 0;
  int16_t cachedLineHeight = 0;

  std::string translatedHtmlPath;

  // Page-level only: translations for paragraphs visible on current page.
  // Chapter-level data is NOT cached — parsed on demand per page to save RAM.
  std::vector<std::string> pageTranslations;

  void preparePage(const Page& page);

  static int measureParagraphHeight(GfxRenderer& renderer, int fontId, const char* text, int maxW, int lh, int spW);
  static int drawParagraph(GfxRenderer& renderer, int fontId, const char* text, int x, int y, int maxW, int lh,
                           int spW, int clipTop, int clipBottom);
};

int getModalFontId();
