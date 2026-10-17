#pragma once

#include "modules/lingua/modes/interleaved/InterleavedMode.h"
#include "modules/lingua/modes/interlinear/InterlinearMode.h"
#include "modules/lingua/modes/normal/NormalMode.h"
#include "modules/lingua/modes/original_only/OriginalOnlyMode.h"
#include "modules/lingua/modes/page_translation/PageTranslationMode.h"
#include "modules/lingua/modes/side_by_side/SideBySideMode.h"
#include "modules/lingua/modes/tooltip/TooltipMode.h"
#include "modules/lingua/modes/translation_only/TranslationOnlyMode.h"
#include "modules/lingua/modes/word_lens/WordLensMode.h"

class LinguaModeRegistry {
 public:
  static LinguaLayout layoutFor(CrossPointSettings::LINGUA_MODE mode);
  LinguaMode& active();
  TooltipMode& tooltip() { return tooltip_; }
  const TooltipMode& tooltip() const { return tooltip_; }
  PageTranslationMode& pageTranslation() { return pageTranslation_; }
  const PageTranslationMode& pageTranslation() const { return pageTranslation_; }
  InterlinearMode& interlinear() { return interlinear_; }
  const InterlinearMode& interlinear() const { return interlinear_; }

 private:
  NormalMode normal_;
  OriginalOnlyMode originalOnly_;
  TranslationOnlyMode translationOnly_;
  InterleavedMode interleaved_;
  SideBySideMode sideBySide_;
  TooltipMode tooltip_;
  PageTranslationMode pageTranslation_;
  InterlinearMode interlinear_;
  WordLensMode wordLens_;
};
