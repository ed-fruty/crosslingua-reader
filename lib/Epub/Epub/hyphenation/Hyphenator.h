#pragma once

#include <cstddef>
#include <string>
#include <vector>

class LanguageHyphenator;

class Hyphenator {
 public:
  struct BreakInfo {
    size_t byteOffset;            // Byte position inside the UTF-8 word where a break may occur.
    bool requiresInsertedHyphen;  // true = a visible '-' must be rendered at the break (pattern/fallback breaks).
                                  // false = break occurs at an existing visible separator boundary
                                  //         (explicit '-' or eligible apostrophe contraction boundary).
  };

  // Returns byte offsets where the word may be hyphenated.
  //
  // Break sources (in priority order):
  //   1. Explicit hyphens already present in the word (e.g. '-' or soft-hyphen U+00AD).
  //      When found, language patterns are additionally run on each alphabetic segment
  //      between separators so compound words can break within their parts.
  //      Example: "US-Satellitensystems" yields breaks after "US-" (no inserted hyphen)
  //               plus pattern breaks inside "Satellitensystems" (Sa|tel|li|ten|sys|tems).
  //   2. Apostrophe contractions between letters (e.g. all'improvviso).
  //      Liang patterns are run per alphabetic segment around apostrophes.
  //      A direct break at the apostrophe boundary is allowed only when the left
  //      segment has at least 3 letters and the right segment has at least 3 letters,
  //      avoiding short clitics (e.g. l', d') and contraction tails (e.g. 've, 're, 'll).
  //   3. Language-specific Liang patterns (e.g. German de_patterns).
  //      Example: "Quadratkilometer" -> Qua|drat|ki|lo|me|ter.
  //   4. Fallback every-N-chars splitting (only when includeFallback is true AND no
  //      pattern breaks were found). Used as a last resort to prevent a single oversized
  //      word from overflowing the page width.
  //
  // When `translated` is true the translated-language hyphenator (see setTranslatedLanguage)
  // is used instead of the primary one. This lets a bilingual (e.g. en->uk translated) book
  // hyphenate each block in its own script: a single book-wide hyphenator built for the
  // original language rejects every word in the other script, so those words never break.
  // Falls back to the primary hyphenator when no translated language has been set.
  static std::vector<BreakInfo> breakOffsets(const std::string& word, bool includeFallback, bool translated = false);

  // Provide a publication-level language hint (e.g. "en", "en-US", "ru") used to select hyphenation rules.
  static void setPreferredLanguage(const std::string& lang);

  // Provide the language of translated blocks (words carrying EpdFontFamily::TRANSLATED), used to
  // select hyphenation rules for those words. Pass an empty string to clear the slot (e.g. at the
  // start of each section build so it never leaks across books). Independent of setPreferredLanguage.
  static void setTranslatedLanguage(const std::string& lang);

 private:
  static const LanguageHyphenator* cachedHyphenator_;
  static const LanguageHyphenator* cachedTranslated_;
};