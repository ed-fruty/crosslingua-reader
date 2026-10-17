#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/lingua/modes/word_lens/WordLensGlossSource.h"
#include "modules/lingua/wordlens/GlossClean.h"
#include "modules/lingua/wordlens/LensReader.h"
#include "modules/lingua/wordlens/LensText.h"

namespace lingua::wordlens {

inline constexpr size_t CANDIDATE_BYTES = 40;
inline constexpr size_t MAX_CANDIDATES = 24;

// Gloss decisions for one (pack, level): the rules Readest's planner applies (src/services/
// wordlens/planner.ts), plus a proper-noun rule. Shared by the layout (which places the strip
// glosses) and the reader's word navigation (which must find exactly the same hard words).
class WordLensGlosser final : public WordLensGlossSource {
 public:
  struct Decision {
    uint16_t rank = 0;         // effective rank (after the English derivation rule)
    uint32_t entryOffset = 0;  // the word's own entry: lemma + full gloss
  };

  // `reader` must be open and outlive this object. `level` is a Level value.
  void configure(LensReader* reader, uint8_t level, bool keepEmptyStrips = true);
  bool ready() const { return reader_ != nullptr && reader_->isOpen(); }

  // The full decision for a laid-out token. False when it is not a hard word at this level.
  bool evaluate(const char* word, size_t length, bool sentenceInitial, Decision& out);

  bool glossFor(const char* word, size_t length, bool sentenceInitial, WordLensGloss& out) override;
  bool keepEmptyStrips() const override { return keepEmptyStrips_; }

  // The pack entry of a token, with no hard-word rules: for a word the layout already chose.
  bool lookupEntry(const char* word, size_t length, Decision& out);

  // Lemma and full gloss of a decided word (for the gloss popup and the dictionary fallback).
  bool readEntry(uint32_t entryOffset, char* lemma, size_t lemmaSize, char* gloss, size_t glossSize) {
    return reader_ && reader_->readEntry(entryOffset, lemma, lemmaSize, gloss, glossSize);
  }

 private:
  // English only: a transparent derivation (lazily <= lazy) inherits its base's lower rank when the
  // base is in the pack and the two glosses share meaning, so it drops below the cutoff.
  uint16_t derivedRank(const char* key, size_t keyLength, const LensReader::Hit& hit);

  LensReader* reader_ = nullptr;
  uint16_t cutoff_ = 0;
  bool englishSource_ = false;
  // Proper nouns: German capitalises every noun, so the rule is off for it.
  bool capitalsMarkNames_ = true;
  bool keepEmptyStrips_ = true;
  // Scratch for one decision, kept here rather than on the stack: a decision runs inside the
  // section build on the reader task, and together these are ~1.8 KB.
  char key_[LOOKUP_KEY_MAX_BYTES + 1] = {};
  char candidates_[MAX_CANDIDATES][CANDIDATE_BYTES] = {};
  char ownGloss_[GLOSS_MAX_BYTES + 1] = {};
  char otherGloss_[GLOSS_MAX_BYTES + 1] = {};
};

// Exposed for tests. Over-generates base-form candidates for an English word (Readest's
// baseFormCandidates); the caller validates each against the pack. Writes up to `maxOut`
// NUL-terminated candidates of at most CANDIDATE_BYTES into `out`.
size_t englishBaseCandidates(const char* word, char (*out)[CANDIDATE_BYTES], size_t maxOut);
// Two glosses share core meaning: a common Han character, or a common ASCII word of 3+ letters.
bool glossesShareMeaning(const char* a, const char* b);
// `gloss` names `word` as a whole ASCII word (case-insensitive).
bool glossMentionsWord(const char* gloss, const char* word);

}  // namespace lingua::wordlens
