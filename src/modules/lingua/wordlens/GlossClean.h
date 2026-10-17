#pragma once

#include <string>
#include <string_view>

// Gloss cleaning for Word Lens packs. A C++ mirror of clean_gloss() in scripts/lens_format.py of
// https://github.com/ed-fruty/crosslingua-wordlens: the on-device JSON -> .lens converter must turn
// Readest's raw glosses into the same bytes the released packs store, so any change here is a change
// there too (test/word_lens_format compares the two on fixtures generated there).
//
// The stored gloss is up to GLOSS_MAX_SENSES cleaned senses joined by GLOSS_SENSE_JOINER: the
// annotation strip shows the first one (wordLensDisplayGloss), the gloss popup shows all of them.
namespace lingua::wordlens {

inline constexpr size_t GLOSS_MAX_BYTES = 240;
inline constexpr size_t GLOSS_MAX_SENSES = 3;
inline constexpr size_t GLOSS_DISPLAY_CODEPOINTS = 24;
inline constexpr char GLOSS_SENSE_JOINER[] = "; ";
inline constexpr char GLOSS_ELLIPSIS[] = "\xE2\x80\xA6";

// Converter-only (allocates): returns "" when nothing readable is left, which drops the entry.
std::string cleanGloss(std::string_view raw, bool monolingual);

// The first sense of a stored gloss, capped to GLOSS_DISPLAY_CODEPOINTS codepoints with an
// ellipsis. Writes into `out` (always NUL-terminated) and returns its length. No allocation: runs
// at layout time for every glossed word.
size_t displayGloss(std::string_view stored, char* out, size_t outSize);

// Truncate a UTF-8 string in place to at most `maxCodepoints` codepoints, replacing the cut tail
// with an ellipsis (so the result has maxCodepoints codepoints including it). Returns the length.
size_t truncateCodepoints(char* text, size_t len, size_t maxCodepoints, size_t capacity);

}  // namespace lingua::wordlens
