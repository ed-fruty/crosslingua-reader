#pragma once
#include <Epub/PageFontSet.h>

#include "modules/lingua/modes/common/LinguaMode.h"
class InterlinearMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_INTERLINEAR; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::Interlinear; }
  static constexpr uint8_t annotationInk(CrossPointSettings::LINGUA_MODE mode, uint8_t shade) {
    // Word Lens strips are annotation rows and take the same colour.
    return (mode == CrossPointSettings::LINGUA_INTERLINEAR || mode == CrossPointSettings::LINGUA_WORD_LENS)
               ? shade
               : PageFontSet::INK_INHERIT;
  }
  LinguaLayout layout() const override { return layoutValue(); }
  bool translationVisible() const { return translationVisible_; }
  void toggleTranslation() { translationVisible_ = !translationVisible_; }

 private:
  bool translationVisible_ = true;
};
