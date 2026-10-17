#pragma once

#include <cstddef>
#include <cstdint>

// Book-token -> pack-key normalization for Word Lens lookups. Allocation-free: it runs for every
// word of every chapter laid out under the Word Lens layout.
namespace lingua::wordlens {

// Longest key a lookup accepts. Pack keys are single words; anything longer is not one of them.
inline constexpr size_t LOOKUP_KEY_MAX_BYTES = 64;

struct TokenInfo {
  // First letter of the token was uppercase (before folding).
  bool capitalized = false;
  // Every letter was uppercase (an acronym, or a heading set in capitals).
  bool allCaps = false;
  // Letters after trimming: 0 means the token holds no word at all.
  uint8_t letters = 0;
};

// Lowercase one codepoint: ASCII, Latin-1, Latin Extended-A, Latin Extended Additional
// (Vietnamese), Greek and Cyrillic. Everything else is returned unchanged.
uint32_t foldCase(uint32_t cp);

// True for codepoints that can be part of a word: letters of any script, plus combining marks.
bool isWordCodepoint(uint32_t cp);

// Turn a laid-out token ("“Whispered,") into a pack key ("whispered") in `out` (NUL-terminated).
// Leading/trailing punctuation is trimmed, typographic apostrophes become ASCII ones, combining
// marks (stress accents in learner editions) are dropped and letters are folded to lowercase.
// Returns the key length, or 0 when the token is not a lookup candidate: no letters, a digit
// inside, or a key longer than LOOKUP_KEY_MAX_BYTES.
size_t normalizeLookupKey(const char* token, size_t len, char* out, size_t outSize, TokenInfo& info);

// True when `token` ends a sentence: its last non-closing-punctuation character is . ! ? … or :.
bool endsSentence(const char* token, size_t len);

}  // namespace lingua::wordlens
