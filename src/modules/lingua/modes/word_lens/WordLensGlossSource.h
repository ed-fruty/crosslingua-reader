#pragma once
#include <cstddef>
#include <cstdint>

// LinguaLayout::WordLens: the boundary between the LAYOUT (lib/Epub, which only places strings) and
// the gloss DECISION (the app: pack lookup, level cutoff, proper-noun and derivation rules). Same
// shape as the Interlinear pairing contract (InterlinearAnnotation.h): lib/Epub sees a plain
// interface and never includes an app header.
struct WordLensGloss {
  // The strip text: the gloss's first sense, at most 24 codepoints.
  static constexpr size_t TEXT_BYTES = 100;
  uint16_t rank = 0;
  char text[TEXT_BYTES] = {};
};

class WordLensGlossSource {
 public:
  virtual ~WordLensGlossSource() = default;
  // Whether `word` -- one laid-out token, punctuation included, not necessarily NUL-terminated --
  // is hard at the configured level, and if so its strip gloss. `sentenceInitial` tells the
  // proper-noun rule a capital letter may just open the sentence.
  virtual bool glossFor(const char* word, size_t length, bool sentenceInitial, WordLensGloss& out) = 0;
};
