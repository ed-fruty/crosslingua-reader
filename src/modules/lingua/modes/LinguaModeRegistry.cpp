#include "modules/lingua/modes/LinguaModeRegistry.h"

LinguaLayout LinguaModeRegistry::layoutFor(const CrossPointSettings::LINGUA_MODE mode) {
  switch (mode) {
    case CrossPointSettings::LINGUA_ORIGINAL_ONLY:
      return OriginalOnlyMode::layoutValue();
    case CrossPointSettings::LINGUA_TRANSLATION_ONLY:
      return TranslationOnlyMode::layoutValue();
    case CrossPointSettings::LINGUA_SIDE_BY_SIDE:
      return SideBySideMode::layoutValue();
    case CrossPointSettings::LINGUA_PAGE_TRANSLATION:
      return PageTranslationMode::layoutValue();
    case CrossPointSettings::LINGUA_TOOLTIP:
      return TooltipMode::layoutValue();
    case CrossPointSettings::LINGUA_INTERLEAVED:
      return InterleavedMode::layoutValue();
    case CrossPointSettings::LINGUA_INTERLINEAR:
      return InterlinearMode::layoutValue();
    case CrossPointSettings::LINGUA_WORD_LENS:
      return WordLensMode::layoutValue();
    case CrossPointSettings::LINGUA_LEGACY_DIMMED:
    case CrossPointSettings::LINGUA_LEGACY_DIMMED_LIGHT:
    case CrossPointSettings::LINGUA_NORMAL:
      return NormalMode::layoutValue();
  }
  return NormalMode::layoutValue();
}

LinguaMode& LinguaModeRegistry::active() {
  switch (static_cast<CrossPointSettings::LINGUA_MODE>(SETTINGS.translationDisplayMode)) {
    case CrossPointSettings::LINGUA_ORIGINAL_ONLY:
      return originalOnly_;
    case CrossPointSettings::LINGUA_TRANSLATION_ONLY:
      return translationOnly_;
    case CrossPointSettings::LINGUA_SIDE_BY_SIDE:
      return sideBySide_;
    case CrossPointSettings::LINGUA_PAGE_TRANSLATION:
      return pageTranslation_;
    case CrossPointSettings::LINGUA_TOOLTIP:
      return tooltip_;
    case CrossPointSettings::LINGUA_INTERLEAVED:
      return interleaved_;
    case CrossPointSettings::LINGUA_INTERLINEAR:
      return interlinear_;
    case CrossPointSettings::LINGUA_WORD_LENS:
      return wordLens_;
    case CrossPointSettings::LINGUA_LEGACY_DIMMED:
    case CrossPointSettings::LINGUA_LEGACY_DIMMED_LIGHT:
    case CrossPointSettings::LINGUA_NORMAL:
      return normal_;
  }
  return normal_;
}
