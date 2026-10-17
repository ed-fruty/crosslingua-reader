#pragma once
#include <Epub/PageFontSet.h>

#include "modules/lingua/modes/common/LinguaMode.h"

// Lingua "Word Lens" display mode (LINGUA_WORD_LENS): short glosses of the page's hard words on a
// strip above each source line, from a language-pair pack (see WordLensService). The strip is an
// annotation row, so its colour and size are Interlinear's.
class WordLensMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_WORD_LENS; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::WordLens; }
  LinguaLayout layout() const override { return layoutValue(); }
};
