#include "modules/lingua/reader/LinguaReaderIntegration.h"

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/WrappedPopup.h"
#include "fontIds.h"
#include "modules/lingua/parsers/TranslationPairWalker.h"

namespace {
// Interlinear annotation visibility as the reader last drew it (pageFontSet()). The upstream
// screens stacked over the reader have no LinguaReaderIntegration of their own, so the resolver
// below reads this mirror; it returns to the default when the reader exits.
bool readerAnnotationsVisible = true;

// Upstream screens (clip selection, dictionary, footnotes) render pages with the plain reader font
// id. Map that id to the same role set the reader draws with; any other id (a dictionary
// definition's own font) gets a uniform set, which is all its body-only pages need.
PageFontSet resolveUpstreamPageFonts(const int bodyFontId) {
  const PageFontSet fonts = LinguaReaderIntegration::resolvePageFontSet(readerAnnotationsVisible);
  return bodyFontId == fonts.body ? fonts : PageFontSet(bodyFontId);
}
[[maybe_unused]] const bool upstreamPageFontsRegistered = [] {
  PageFontSet::resolver = &resolveUpstreamPageFonts;
  return true;
}();
}  // namespace

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleInput(MappedInputManager& input,
                                                                          const bool hasSection) {
  InputAction action = handleTransientUi();
  if (action != InputAction::None) return action;

  action = handleFallbackDialog(input);
  if (action != InputAction::None) return action;

  action = handleTooltipInput(input, hasSection);
  if (action != InputAction::None) return action;

  action = handlePageTranslationInput(input, hasSection);
  if (action != InputAction::None) return action;

  return handleInterlinearInput(input, hasSection);
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleTransientUi() {
  if (showNoTranslationsToast_ && millis() - noTranslationsToastTime_ >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showNoTranslationsToast_ = false;
    return InputAction::Render;
  }
  return InputAction::None;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleFallbackDialog(MappedInputManager& input) {
  if (!fallbackDialogActive_) return InputAction::None;
  if (fallbackDialogDrawn_ &&
      (input.wasReleased(MappedInputManager::Button::Confirm) || input.wasReleased(MappedInputManager::Button::Back))) {
    fallbackDialogActive_ = false;
    fallbackDialogDrawn_ = false;
    return InputAction::Render;
  }
  return InputAction::Consumed;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleTooltipInput(MappedInputManager& input,
                                                                                 const bool hasSection) {
  if (!hasSection || SETTINGS.translationDisplayMode != CrossPointSettings::LINGUA_TOOLTIP) {
    return InputAction::None;
  }

  if (modes_.tooltip().handleInput(input)) {
    if (modes_.tooltip().hasPendingPageTurn()) {
      const bool forward = modes_.tooltip().takePendingPageTurn();
      return forward ? InputAction::PageForward : InputAction::PageBack;
    }
    return InputAction::Render;
  }

  const bool front = SETTINGS.tooltipButtons == CrossPointSettings::OVERLAY_BUTTONS_FRONT;
  const bool side = SETTINGS.tooltipButtons == CrossPointSettings::OVERLAY_BUTTONS_SIDE;
  const bool held = (front && (input.isPressed(MappedInputManager::Button::Left) ||
                               input.isPressed(MappedInputManager::Button::Right))) ||
                    (side && (input.isPressed(MappedInputManager::Button::PageBack) ||
                              input.isPressed(MappedInputManager::Button::PageForward)));
  return held ? InputAction::Consumed : InputAction::None;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handlePageTranslationInput(MappedInputManager& input,
                                                                                         const bool hasSection) {
  if (modes_.pageTranslation().handleInput(input)) {
    return InputAction::Render;
  }

  if (modes_.pageTranslation().active()) return InputAction::Consumed;

  if (hasSection && SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_PAGE_TRANSLATION) {
    const bool forward = input.wasReleased(MappedInputManager::Button::PageForward);
    const bool back = input.wasReleased(MappedInputManager::Button::PageBack);
    if (forward || back) {
      if (input.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
        modes_.pageTranslation().open();
        clearOverlayPrewarm();
        return InputAction::Render;
      }
      return forward ? InputAction::PageForward : InputAction::PageBack;
    }
    if (input.isPressed(MappedInputManager::Button::PageForward) ||
        input.isPressed(MappedInputManager::Button::PageBack)) {
      return InputAction::Consumed;
    }
  }
  return InputAction::None;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleInterlinearInput(MappedInputManager& input,
                                                                                     const bool hasSection) {
  if (hasSection && SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_INTERLINEAR &&
      SETTINGS.interlinearToggleByLongPress) {
    const bool front = SETTINGS.interlinearToggleButtons == CrossPointSettings::OVERLAY_BUTTONS_FRONT;
    const auto backButton = front ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;
    const auto forwardButton = front ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
    const bool back = input.wasReleased(backButton);
    const bool forward = input.wasReleased(forwardButton);
    if (back || forward) {
      if (input.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
        modes_.interlinear().toggleTranslation();
        readerAnnotationsVisible = modes_.interlinear().translationVisible();
        return InputAction::Render;
      }
      return forward ? InputAction::PageForward : InputAction::PageBack;
    }
    if (input.isPressed(backButton) || input.isPressed(forwardButton)) return InputAction::Consumed;
  }

  return InputAction::None;
}

PageFontSet LinguaReaderIntegration::pageFontSet() const {
  readerAnnotationsVisible = modes_.interlinear().translationVisible();
  return resolvePageFontSet(readerAnnotationsVisible);
}

void LinguaReaderIntegration::configureRenderer(GfxRenderer& renderer) {
  const auto mode = static_cast<CrossPointSettings::LINGUA_MODE>(SETTINGS.translationDisplayMode);
  renderer.setTranslationGrayLevel(InterleavedMode::translatedWordInk(mode, SETTINGS.translationShade));
}

PageFontSet LinguaReaderIntegration::resolvePageFontSet(const bool annotationVisible) {
  const auto mode = static_cast<CrossPointSettings::LINGUA_MODE>(SETTINGS.translationDisplayMode);
  PageFontSet fonts(SETTINGS.getReaderFontId(), SETTINGS.getInterleavedTranslationFontId(),
                    SETTINGS.getInterlinearAnnotationFontId());
  fonts.annotationInk = InterlinearMode::annotationInk(mode, SETTINGS.interlinearAnnotationShade);
  fonts.translationInk = SideBySideMode::translationInk(mode, SETTINGS.sideBySideTranslationShade);
  fonts.annotationVisible = annotationVisible;
  return fonts;
}

void LinguaReaderIntegration::renderOverlay(GfxRenderer& renderer, const Page& page, const int xOffset,
                                            const int yOffset, const int viewportWidth, const int viewportHeight) {
  const int bodyFont = SETTINGS.getReaderFontId();
  // Each overlay draws on its OWN active flag, so it has to be gated on the mode it belongs to as
  // well. Tooltip and Page Translation share one LinguaLayout, so switching between them is a cache
  // hit with no re-layout -- without this gate the overlay left over from the previous mode keeps
  // painting itself over the new one's page.
  const auto displayMode = static_cast<CrossPointSettings::LINGUA_MODE>(SETTINGS.translationDisplayMode);
  if (displayMode == CrossPointSettings::LINGUA_PAGE_TRANSLATION && modes_.pageTranslation().active()) {
    modes_.pageTranslation().render(renderer, page, bodyFont, getPageTranslationFontId(), xOffset, yOffset,
                                    viewportWidth, viewportHeight);
    if (!modes_.pageTranslation().active()) {
      showNoTranslationsToast_ = true;
      noTranslationsToastTime_ = millis();
    }
  }
  if (displayMode == CrossPointSettings::LINGUA_TOOLTIP && modes_.tooltip().active()) {
    modes_.tooltip().render(renderer, page, bodyFont, getTooltipFontId(), xOffset, yOffset, viewportWidth,
                            viewportHeight);
  }
}

void LinguaReaderIntegration::onDisplayModeChanged() {
  // The display mode changed without the section being rebuilt (the two overlay modes share a
  // layout, so their pages are byte-identical and the cache is a HIT). Nothing else clears the
  // overlays, so an open tooltip or translation panel would survive into the new mode.
  modes_.pageTranslation().onPageChanged();
  modes_.tooltip().onPageChanged();
  clearOverlayPrewarm();
}

bool LinguaReaderIntegration::prepareOverlayFonts(GfxRenderer& renderer, const Page& page, const PageFontSet& fonts,
                                                  const int spineIndex, const int pageIndex, const int xOffset,
                                                  const int yOffset) {
  if (!modes_.pageTranslation().active() && !modes_.tooltip().active()) {
    clearOverlayPrewarm();
    return false;
  }

  auto* cache = renderer.getFontCacheManager();
  const int overlayFont = modes_.pageTranslation().active() ? getPageTranslationFontId() : getTooltipFontId();
  const bool stale = !overlayPrewarm_ || overlayPrewarmSpine_ != spineIndex || overlayPrewarmPage_ != pageIndex ||
                     overlayPrewarmFontId_ != fonts.body || overlayPrewarmOverlayFontId_ != overlayFont ||
                     overlayPrewarmGeneration_ != cache->cacheGeneration();
  if (!stale) return true;

  std::string overlayText;
  if (modes_.pageTranslation().active()) {
    modes_.pageTranslation().collectGlyphText(page, overlayText);
  } else {
    modes_.tooltip().collectGlyphText(page, overlayText);
  }

  overlayPrewarm_.emplace(*cache);
  page.render(renderer, fonts, xOffset, yOffset);
  if (overlayFont == fonts.body && !overlayText.empty()) {
    cache->recordText(overlayText.c_str(), fonts.body, EpdFontFamily::REGULAR);
  }
  overlayPrewarm_->endScanAndPrewarm();
  if (overlayFont != fonts.body && !overlayText.empty()) {
    cache->prewarmCache(overlayFont, overlayText.c_str(), 0x01);
  }

  overlayPrewarmSpine_ = spineIndex;
  overlayPrewarmPage_ = pageIndex;
  overlayPrewarmFontId_ = fonts.body;
  overlayPrewarmOverlayFontId_ = overlayFont;
  overlayPrewarmGeneration_ = cache->cacheGeneration();
  return true;
}

void LinguaReaderIntegration::drawTransientUi(GfxRenderer& renderer) const {
  if (showNoTranslationsToast_) {
    drawWrappedPopup(renderer, tr(STR_NO_TRANSLATIONS_FOR_PAGE));
  }
}

bool LinguaReaderIntegration::prepareSection(Section& section) {
  if (SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_NORMAL) return false;
  if (!section.isTranslationPresenceKnown()) section.resolveTranslationPresence();
  if (section.hasTranslation() || section.isTextless()) return false;

  SETTINGS.translationDisplayMode = CrossPointSettings::LINGUA_NORMAL;
  SETTINGS.saveToFile();
  fallbackDialogActive_ = true;
  fallbackDialogDrawn_ = false;
  return true;
}

void LinguaReaderIntegration::drawFallbackDialog(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_LINGUA));

  const int maxTextWidth = std::max(1, screen.width - 2 * (metrics.popupMarginX + metrics.popupFrameThickness));
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const auto lines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_NO_TRANSLATION_SWITCH_NORMAL), maxTextWidth, 4);
  int textY = screen.y + (screen.height - static_cast<int>(lines.size()) * lineHeight) / 2;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_12_FONT_ID, textY, line.c_str(), true);
    textY += lineHeight;
  }

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  fallbackDialogDrawn_ = true;
}

void LinguaReaderIntegration::onPageChanged() {
  modes_.pageTranslation().onPageChanged();
  modes_.tooltip().onPageChanged();
  clearOverlayPrewarm();
}

void LinguaReaderIntegration::onSectionChanged(const std::string& translationSourcePath,
                                               const std::string& pairCachePath, const std::string& bookPrimaryLang) {
  if (translationSourcePath_ == translationSourcePath) return;
  translationSourcePath_ = translationSourcePath;
  pairCachePath_ = pairCachePath;
  bookPrimaryLang_ = bookPrimaryLang;
  pairCacheAttempted_ = false;
  modes_.pageTranslation().setTranslationSource(translationSourcePath, pairCachePath, bookPrimaryLang);
  modes_.pageTranslation().onSectionChanged();
  modes_.tooltip().setTranslationSource(translationSourcePath, pairCachePath, bookPrimaryLang);
  modes_.tooltip().onPageChanged();
  clearOverlayPrewarm();
}

void LinguaReaderIntegration::buildPairCacheIfNeeded() {
  // Pure optimisation, and deliberately off the render path: every page already works by walking the
  // HTML, this only makes the ones after it cheaper. Attempted at most once per chapter -- a failure
  // must not turn into a retry on every tick.
  if (pairCacheAttempted_ || translationSourcePath_.empty() || pairCachePath_.empty()) return;
  const auto mode = static_cast<CrossPointSettings::LINGUA_MODE>(SETTINGS.translationDisplayMode);
  if (mode != CrossPointSettings::LINGUA_PAGE_TRANSLATION && mode != CrossPointSettings::LINGUA_TOOLTIP) return;

  pairCacheAttempted_ = true;
  if (lingua::hasTranslationPairCache(pairCachePath_)) return;
  const uint32_t started = millis();
  if (lingua::writeTranslationPairCache(translationSourcePath_, bookPrimaryLang_, pairCachePath_)) {
    LOG_DBG("LNG", "Pair cache built in %lums", millis() - started);
  }
}

void LinguaReaderIntegration::onReaderExit() {
  readerAnnotationsVisible = true;
  clearOverlayPrewarm();
  modes_.pageTranslation().onSectionChanged();
  modes_.tooltip().onPageChanged();
  translationSourcePath_.clear();
  pairCachePath_.clear();
  pairCacheAttempted_ = false;
}

void LinguaReaderIntegration::clearOverlayPrewarm() {
  overlayPrewarm_.reset();
  overlayPrewarmSpine_ = -1;
  overlayPrewarmPage_ = -1;
  overlayPrewarmFontId_ = -1;
  overlayPrewarmOverlayFontId_ = -1;
  overlayPrewarmGeneration_ = 0;
}
