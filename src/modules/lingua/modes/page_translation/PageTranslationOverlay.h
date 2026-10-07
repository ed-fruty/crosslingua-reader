#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <string>
#include <vector>

class PageTranslationOverlay {
 public:
  // Where the chapter's translated text lives -- the reader's `.translated.html` sidecar, or the
  // chapter's own XHTML for a book whose translation is embedded -- plus the book's primary language,
  // which is what makes a lang-tagged block a translation. Both are handed to the shared walker.
  void setTranslationSource(const std::string& htmlPath, const std::string& cachePath,
                            const std::string& bookPrimaryLang);

  // Opens the overlay externally (e.g. from EpubReaderActivity's longpress handler).
  void open();

  bool handleInput(MappedInputManager& input);

  void render(GfxRenderer& renderer, const Page& page, int fontId, int pageTranslationFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  void onPageChanged();
  void onSectionChanged();

  // Prepare this page's paragraph translations (idempotent with render()'s own preparePage) and append
  // every string the overlay will draw for this page — each visible paragraph's text plus the
  // STR_NO_TRANSLATION marker when any paragraph is a source fallback — into `out`. Lets the caller
  // prewarm the overlay font's glyph cache ONCE per page (see EpubReaderActivity::renderOverlayFrame)
  // instead of taking a per-glyph SD read while scrolling. Text only: no rendering, no SD I/O.
  void collectPageGlyphText(const Page& page, std::string& out);

  bool isActive() const { return active; }

 private:
  // One paragraph to display in the overlay, in reading order. `text` is either a
  // real translation or — when no translation exists for this paragraph — the
  // page's own SOURCE text (Option C fallback), in which case render() appends a
  // short dim STR_NO_TRANSLATION marker so the gap is visible but unobtrusive.
  // No per-paragraph indent VALUE is stored: the overlay's first-line indent is the same for every
  // paragraph that begins here, because it is a pure function of the Extra Paragraph Spacing setting
  // (ParsedText::defaultFirstLineIndent) and the overlay's own font. render() computes it once and
  // gates it on `startsHere`.
  struct DisplayPara {
    std::string text;
    bool translated = true;  // false => source fallback; render() adds the dim marker line
    // Did this paragraph BEGIN on this page? False only for the page's first entry when the layout
    // flowed it in from the previous page, i.e. what the body draws as a continuation line. An
    // indent marks the START of a paragraph, so a continuation must render flush — the body resolves
    // this with isFirstLine=(breakIndex==0) (ParsedText::resolveFirstLineIndent) and the overlay has
    // to reach the same answer. See preparePage() for how it is detected.
    bool startsHere = true;
  };

  bool active = false;
  // Always a LINE TOP in content coordinates (0, or a value published by render() below), so a
  // drawn line can never straddle the top of the viewport.
  int16_t scrollOffset = 0;
  // Scroll targets for the window currently on screen, published by render() from the real line
  // positions; -1 = none (nothing more below / nothing above). Line-top aligned, which is what lets
  // the draw pass demand a fully-visible line box without ever losing a line: a line that straddles
  // the bottom edge is skipped now and becomes the FIRST line of the next window.
  int16_t nextScrollOffset = -1;
  int16_t prevScrollOffset = -1;
  bool pagePrepared = false;

  std::string translationSourcePath;
  std::string pairCachePath;
  std::string bookPrimaryLang;

  // Page-level only: what to show for each VISIBLE source paragraph on the current
  // page. Dense over the page's [first..last] range (one entry per visible
  // paragraph — translation OR source fallback), never sparse. Chapter-level data
  // is NOT cached — parsed on demand per page to save RAM.
  std::vector<DisplayPara> pageParagraphs;
  int16_t translatedCount = 0;  // paragraphs on this page with a REAL translation (gates opening)

  void preparePage(const Page& page);
};

int getPageTranslationFontId();
