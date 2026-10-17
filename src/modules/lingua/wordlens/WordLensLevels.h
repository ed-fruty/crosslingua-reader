#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/lingua/wordlens/LensFormat.h"

// Word Lens reader levels. The level is a FREQUENCY BAND named after the CEFR levels, not a real
// per-word CEFR grading: a word is glossed when its frequency rank (its lemma's, for an inflected
// form) is at or above the cutoff, i.e. rarer than the vocabulary a learner at that level
// typically knows. Same bands as Readest's Word Lens (src/services/wordlens/difficulty.ts).
namespace lingua::wordlens {

enum class Level : uint8_t { A1 = 0, A2 = 1, B1 = 2, B2 = 3, C1 = 4, C2 = 5 };
inline constexpr uint8_t LEVEL_COUNT = 6;
inline constexpr Level DEFAULT_LEVEL = Level::B1;

// The commonest words are never glossed, whatever the level.
inline constexpr uint16_t MIN_GLOSS_RANK = 1000;

inline constexpr uint16_t FREQUENCY_CUTOFFS[LEVEL_COUNT] = {1000, 2000, 4000, 8000, 14000, 24000};
// HSK-ranked packs (zh source; not loaded by the firmware's tokenizer today, kept so a pack's
// rank scale is honoured rather than misread).
inline constexpr uint16_t HSK_CUTOFFS[LEVEL_COUNT] = {6000, 9000, 12000, 15000, 18000, 24000};

inline constexpr const char* LEVEL_LABELS[LEVEL_COUNT] = {"A1", "A2", "B1", "B2", "C1", "C2"};

constexpr uint8_t clampLevel(const uint8_t level) { return level < LEVEL_COUNT ? level : LEVEL_COUNT - 1; }

constexpr uint16_t rankCutoff(const uint8_t level, const uint8_t rankScale = LENS_RANK_SCALE_FREQUENCY) {
  return (rankScale == LENS_RANK_SCALE_HSK ? HSK_CUTOFFS : FREQUENCY_CUTOFFS)[clampLevel(level)];
}

constexpr bool isDifficult(const uint16_t rank, const uint16_t cutoff) {
  return rank >= cutoff && rank >= MIN_GLOSS_RANK;
}

// Section cache key of a pack (its content hash) read at `level`: never 0, which means "no pack".
constexpr uint32_t wordLensCacheKey(const uint32_t contentHash, const uint8_t level) {
  const uint8_t bytes[6] = {static_cast<uint8_t>(contentHash),
                            static_cast<uint8_t>(contentHash >> 8),
                            static_cast<uint8_t>(contentHash >> 16),
                            static_cast<uint8_t>(contentHash >> 24),
                            level,
                            'W'};
  const uint32_t key = fnv1a32(bytes, sizeof(bytes));
  return key != 0 ? key : 1;
}

static_assert(rankCutoff(0) == 1000 && rankCutoff(5) == 24000 && rankCutoff(9) == 24000);
static_assert(!isDifficult(999, 0) && isDifficult(4000, 4000) && !isDifficult(3999, 4000));

}  // namespace lingua::wordlens
