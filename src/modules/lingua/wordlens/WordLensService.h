#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "modules/lingua/modes/word_lens/WordLensGlossSource.h"

// The Word Lens pack the reader is using: which language pair the open book needs, whether that
// pack is on the card, and -- while the Word Lens mode is active -- the open pack itself.
//
// One process-wide instance, because two very different callers need the same answer:
// CrossPointSettings::readerRenderSpec() (the cache key and the parser's gloss source) and the
// reader's word navigation. It holds nothing but a pointer while inactive; the open pack (file
// handle, bucket table, LRU buffers, result cache, decision scratch: ~10-12 KB) is allocated by
// sync() when the mode is entered and released by close() when it is left or the reader exits.
namespace lingua::wordlens {

class WordLensGlosser;

class WordLensService {
 public:
  enum class State : uint8_t {
    Inactive,      // not in the Word Lens mode
    NoLanguage,    // the book declares no language and no source override is set
    NotInstalled,  // no pack for (book language -> hint language) on the card
    OpenFailed,    // the pack is there but unreadable (corrupt or out of memory)
    Ready,
  };

  static WordLensService& instance();

  // Bring the open pack in line with the settings and `bookLanguage` (dc:language, may be empty).
  // Call only where no section build can be running -- the reader does it right before it creates
  // a Section -- because closing a pack invalidates the gloss source a build holds. Returns true
  // when cacheKey() / glossSource() changed.
  bool sync(const std::string& bookLanguage);
  void close();

  // Ready: the pack is installed (and opens with the next section if it is not open yet).
  State state() const;
  // Section cache key: 0 outside the mode, else the open pack's content hash mixed with the level.
  // Computed from the live settings, so a level or language change shows up as a different key at
  // once, before sync() has reopened anything.
  uint32_t cacheKey() const;
  // Null unless the open pack matches the live settings.
  WordLensGlossSource* glossSource();
  WordLensGlosser* glosser();

  // The pair the live settings and the last synced book ask for; empty while unknown. The target
  // is the Word Lens hint language when a pack exists for it (installed or offered by a cached
  // manifest), else -- automatic -- the translator's Target Language, then the UI language, when a
  // pack exists for either, then any installed pack for the source.
  const char* wantedSource() const { return wantedSource_; }
  const char* wantedTarget() const;

  static constexpr size_t MAX_TARGETS = 24;
  using LanguageCode = char[8];
  // Hint languages with a pack for `source`, installed or in a cached manifest; sorted, unique.
  static size_t availableTargets(const char* source, LanguageCode* out, size_t max);
  // English name of a language code ("uk" -> "Ukrainian"), or the code itself.
  static const char* languageName(const char* code);
  // Primary subtag of `bookLanguage`, else of the Source Language override; empty when neither.
  static void sourceLanguage(const std::string& bookLanguage, char* out, size_t outSize);
  // A pack for (source, target) exists on the card.
  static bool isInstalled(const char* source, const char* target);

 private:
  struct OpenPack;
  WordLensService();
  ~WordLensService();
  bool openPairMatches() const;
  bool openMatches() const;
  void resolveTarget(char* out, size_t outSize) const;

  std::unique_ptr<OpenPack> open_;
  char wantedSource_[8] = {};
  // wantedTarget() memo: what it was resolved from, so the SD is only consulted when one changes.
  mutable char wantedTarget_[8] = {};
  mutable char memoSource_[8] = {};
  mutable char memoHint_[8] = {};
  mutable uint8_t memoTranslator_ = 0;
  mutable uint8_t memoUi_ = 0;
  mutable bool memoValid_ = false;
  State lastState_ = State::Inactive;
};

}  // namespace lingua::wordlens
