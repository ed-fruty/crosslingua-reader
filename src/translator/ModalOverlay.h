#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <string>
#include <vector>

class ModalOverlay {
 public:
  // Public because static XML callbacks in the .cpp need access.
  // NOTE (port): `paragraphIdx` is NEW vs the fork's struct. The selective SAX parse
  // returns a *sparse* vector whose entries each carry their actual paragraph index —
  // entries for paragraphs outside `[wantFirst, wantLast]` are skipped entirely, so
  // we can no longer index by position.
  struct ParagraphPair {
    int16_t paragraphIdx = -1;       // sparse: matches the actual page range only
    std::string origText;            // empty unless this is a boundary entry (wantFirst or wantLast)
    std::string translation;         // translated paragraph text
    int16_t origSentenceCount = 0;   // number of sentences in original paragraph
    int16_t transSentenceCount = 0;  // number of sentences in translation
  };

  void setTranslatedHtmlPath(const std::string& path);

  // Opens the overlay externally (e.g. from EpubReaderActivity's longpress handler).
  void open();

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
  static int drawParagraph(GfxRenderer& renderer, int fontId, const char* text, int x, int y, int maxW, int lh, int spW,
                           int clipTop, int clipBottom);
};

int getModalFontId();
