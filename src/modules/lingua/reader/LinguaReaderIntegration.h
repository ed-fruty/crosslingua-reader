#pragma once

#include <Epub/Page.h>
#include <Epub/PageFontSet.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <I18n.h>

#include <cstdint>
#include <optional>
#include <string>

#include "modules/lingua/modes/LinguaModeRegistry.h"
#include "modules/lingua/modes/word_lens/WordLensReader.h"

class MappedInputManager;

// Keeps the CrossLingua reader extension out of the upstream reader state machine.
// EpubReaderActivity translates the returned intent into its native page-turn/render operations.
class LinguaReaderIntegration {
 public:
  // OpenDefinition: Word Lens looked a word up; takeWordLensDefinition() has the result.
  enum class InputAction : uint8_t { None, Consumed, Render, PageBack, PageForward, OpenDefinition };

  InputAction handleInput(MappedInputManager& input, bool hasSection);
  static void configureRenderer(GfxRenderer& renderer);
  static PageFontSet resolvePageFontSet(bool annotationVisible = true);
  PageFontSet pageFontSet() const;
  void renderOverlay(GfxRenderer& renderer, const Page& page, int xOffset, int yOffset, int viewportWidth,
                     int viewportHeight);
  bool prepareOverlayFonts(GfxRenderer& renderer, const Page& page, const PageFontSet& fonts, int spineIndex,
                           int pageIndex, int xOffset, int yOffset);
  void drawTransientUi(GfxRenderer& renderer) const;
  bool prepareSection(Section& section);
  void drawFallbackDialog(GfxRenderer& renderer, const MappedInputManager& input);
  void onPageChanged();
  // Call when the user picked a different Lingua display mode. Required even when the page layout is
  // unchanged: the previous mode's overlay stays active otherwise.
  void onDisplayModeChanged();
  void onSectionChanged(const std::string& translationSourcePath, const std::string& pairCachePath,
                        const std::string& bookPrimaryLang);
  // Builds this chapter's pair cache if it has none. Called from the reader's background tick, never
  // from a render: a page whose cache is missing simply walks the HTML instead.
  void buildPairCacheIfNeeded();
  void onReaderExit();

  bool pageTranslationActive() const { return modes_.pageTranslation().active(); }
  WordLensReader::Definition takeWordLensDefinition() { return wordLens_.takeDefinition(); }
  bool fallbackDialogActive() const { return fallbackDialogActive_; }

 private:
  InputAction handleTransientUi();
  InputAction handleFallbackDialog(MappedInputManager& input);
  InputAction handleTooltipInput(MappedInputManager& input, bool hasSection);
  InputAction handlePageTranslationInput(MappedInputManager& input, bool hasSection);
  InputAction handleInterlinearInput(MappedInputManager& input, bool hasSection);
  InputAction handleWordLensInput(MappedInputManager& input, bool hasSection);
  // Whether the active mode's annotation rows are shown (its long-press hide toggle).
  bool annotationRowsVisible() const;
  void clearOverlayPrewarm();

  LinguaModeRegistry modes_;
  WordLensReader wordLens_;

  std::optional<FontCacheManager::PrewarmScope> overlayPrewarm_;
  int overlayPrewarmSpine_ = -1;
  int overlayPrewarmPage_ = -1;
  int overlayPrewarmFontId_ = -1;
  int overlayPrewarmOverlayFontId_ = -1;
  uint32_t overlayPrewarmGeneration_ = 0;
  std::string translationSourcePath_;
  std::string pairCachePath_;
  std::string bookPrimaryLang_;
  bool pairCacheAttempted_ = false;
  bool fallbackDialogActive_ = false;
  bool fallbackDialogDrawn_ = false;
  bool showNoTranslationsToast_ = false;
  unsigned long noTranslationsToastTime_ = 0;
  // Which message the toast above shows: Page Translation's "no translations for this page", or
  // Word Lens's "no gloss pack for this book" (shown once per book).
  StrId toastMessage_ = StrId::STR_NO_TRANSLATIONS_FOR_PAGE;
  bool wordLensNoPackNotified_ = false;
};
