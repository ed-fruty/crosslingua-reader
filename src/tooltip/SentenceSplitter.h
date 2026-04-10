#pragma once

#include <cstdint>

// Maximum sentences per paragraph — fixed-size to avoid heap allocation.
static constexpr int MAX_SENTENCES = 50;

// Word index range within a paragraph's original words (grayLevel == 0).
struct SentenceSpan {
  uint16_t startWord;  // inclusive
  uint16_t endWord;    // exclusive
};

// Result of sentence splitting: array of spans + count.
struct SentenceSplitResult {
  SentenceSpan spans[MAX_SENTENCES];
  int count = 0;
};

// A mapped sentence: original word range + pointer to translated text.
// The translatedText buffer is owned by the caller (stack-allocated).
struct MappedSentence {
  SentenceSpan original;
  const char* translatedText;  // points into caller-owned buffer
};

struct MappedSentenceResult {
  MappedSentence sentences[MAX_SENTENCES];
  int count = 0;
};

// Split a sequence of words into sentences based on punctuation boundaries.
// words: array of C-strings (the original words from TextBlock, grayLevel==0 only)
// wordCount: number of words
// Returns SentenceSplitResult with word-index-based spans.
SentenceSplitResult splitSentences(const char* const* words, int wordCount);

// Map translated words to original sentences using proportional character-length mapping.
// originalWords/originalCount: original (grayLevel==0) words
// translatedWords/translatedCount: translated (grayLevel>0) words
// splits: result from splitSentences()
// outBuffer: caller-owned buffer where concatenated translation strings are written
// outBufferSize: size of outBuffer
// Returns MappedSentenceResult with original spans + translation text pointers.
MappedSentenceResult mapSentenceTranslations(const char* const* originalWords, int originalCount,
                                             const char* const* translatedWords, int translatedCount,
                                             const SentenceSplitResult& splits, char* outBuffer, int outBufferSize);
