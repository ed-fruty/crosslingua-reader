#pragma once

#include <cstdint>
#include <string>

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

// Split a sequence of words into sentences based on punctuation boundaries.
// words: array of C-strings (the original words from TextBlock, grayLevel==0 only)
// wordCount: number of words
// Returns SentenceSplitResult with word-index-based spans.
SentenceSplitResult splitSentences(const char* const* words, int wordCount);

// Same, except that a PARAGRAPH boundary always ends a sentence.
//
// splitSentences() breaks on punctuation only. Fed a whole PAGE's words with the paragraph structure
// flattened away, it therefore glues a paragraph that ends WITHOUT a terminator — a chapter heading,
// a stat line, a list row — onto the paragraph that follows it, producing one "sentence" out of two.
// A consumer whose translations are keyed per paragraph (TooltipOverlay's index is built one entry
// per paragraph pair) then has more entries than the page has sentences, and its fuzzy key matcher
// resolves the glued sentence to whichever key it prefix-matches first — the heading — orphaning the
// real first sentence's translation. Splitting per paragraph run restores the invariant that both
// sides are cut at the same places.
//
// `runStarts[r]` is the word index paragraph run r begins at, ascending. Words before runStarts[0]
// (and after the last run) form their own runs, so the table need not be exhaustive; a null table or
// runCount <= 0 degrades to plain splitSentences. The MAX_SENTENCES budget is shared across runs,
// exactly as splitSentences applies it within one.
SentenceSplitResult splitSentencesByParagraph(const char* const* words, int wordCount, const uint16_t* runStarts,
                                              int runCount);

// ── Char-offset sentence helpers (over the shared textnorm canonical fold) ──
//
// Terminator / closing-quote recognition funnels through textnorm's ONE set of
// primitives so "where a sentence ends" stays single-sourced. These scan the
// ORIGINAL (unfolded) text — the trim helpers must return DISPLAYABLE text, and
// the fold is lossy / match-only — while the matching helper
// (countSentencesBefore) folds BOTH sides before comparing.

// Sentence count for a paragraph. Clamped to >=1 for any non-empty text (every
// non-empty paragraph is at least one sentence even without a terminator); 0 for
// empty text.
int countSentences(const std::string& text);

// Trim text to its first N sentences (fewer present => whole text).
std::string trimToSentences(const std::string& text, int maxSentences);

// Trim text to its LAST N sentences (fewer present => whole text).
std::string trimToLastSentences(const std::string& text, int maxSentences);

// Count sentences in origText that end BEFORE where visibleStart begins. Both
// sides run through textnorm::foldForMatch so raw-HTML origText (NBSP, fancy
// quotes, soft hyphens) and laid-out page words compare on equal footing. Honest
// zero when the visible portion starts inside the paragraph's first sentence.
int countSentencesBefore(const std::string& origText, const std::string& visibleStart);
