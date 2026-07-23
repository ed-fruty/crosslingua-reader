#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <string>
#include <vector>

class ModalOverlay {
 public:
  // Public because static XML callbacks in the .cpp need access.
  // Sparse: only paragraphs in the current page's [first..last] range get an entry, and each
  // carries its real paragraph index — so we iterate by paragraphIdx, not by vector position.
  struct ParagraphPair {
    int16_t paragraphIdx = -1;       // actual chapter paragraph index this entry represents
    std::string origText;            // kept ONLY for boundary paragraphs (first/last) — empty otherwise
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
  // One paragraph to display in the modal, in reading order. `text` is either a
  // real translation or — when no translation exists for this paragraph — the
  // page's own SOURCE text (Option C fallback), in which case render() appends a
  // short dim STR_NO_TRANSLATION marker so the gap is visible but unobtrusive.
  struct DisplayPara {
    std::string text;
    bool translated = true;  // false => source fallback; render() adds the dim marker line
  };

  bool active = false;
  int16_t scrollOffset = 0;
  int16_t totalContentHeight = 0;
  bool pagePrepared = false;
  int16_t cachedViewportHeight = 0;
  int16_t cachedLineHeight = 0;

  std::string translatedHtmlPath;

  // Page-level only: what to show for each VISIBLE source paragraph on the current
  // page. Dense over the page's [first..last] range (one entry per visible
  // paragraph — translation OR source fallback), never sparse. Chapter-level data
  // is NOT cached — parsed on demand per page to save RAM.
  std::vector<DisplayPara> pageParagraphs;
  int16_t translatedCount = 0;  // paragraphs on this page with a REAL translation (gates opening)

  void preparePage(const Page& page);
};

int getModalFontId();
