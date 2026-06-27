#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include <string>
#include <vector>

class ModalOverlay {
 public:
  // Public because static XML callbacks in the .cpp need access.
  // Sparse: only paragraphs in the current page's [first..last] range get an entry, and each
  // carries its real paragraph index — so we iterate by paragraphIdx, not by vector position.
  struct ParagraphPair {
    int16_t paragraphIdx = -1;        // actual chapter paragraph index this entry represents
    std::string origText;             // kept ONLY for boundary paragraphs (first/last) — empty otherwise
    std::string translation;          // translated paragraph text
    int16_t origSentenceCount = 0;    // number of sentences in original paragraph
    int16_t transSentenceCount = 0;   // number of sentences in translation
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
};

int getModalFontId();
