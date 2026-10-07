#pragma once

#include <Epub/Page.h>
#include <Epub/PageFontSet.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>

#include <cstdint>
#include <optional>
#include <string>

#include "modules/lingua/modes/LinguaModeRegistry.h"

class MappedInputManager;

// Keeps the CrossLingua reader extension out of the upstream reader state machine.
// EpubReaderActivity translates the returned intent into its native page-turn/render operations.
class LinguaReaderIntegration {
 public:
  enum class InputAction : uint8_t { None, Consumed, Render, PageBack, PageForward };

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
  bool fallbackDialogActive() const { return fallbackDialogActive_; }

 private:
  InputAction handleTransientUi();
  InputAction handleFallbackDialog(MappedInputManager& input);
  InputAction handleTooltipInput(MappedInputManager& input, bool hasSection);
  InputAction handlePageTranslationInput(MappedInputManager& input, bool hasSection);
  InputAction handleInterlinearInput(MappedInputManager& input, bool hasSection);
  void clearOverlayPrewarm();

  LinguaModeRegistry modes_;

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
};
