#pragma once

#include <string>
#include <vector>

#include "SentenceSplitter.h"

// Shared sentence-level source <-> translation alignment.
//
// Two consumers need exactly the same answer to "which translated sentence belongs to this source
// sentence?", so the rule lives here once instead of in each of them:
//   * TooltipOverlay  — builds a key -> translation index at view time and shows one unit per press;
//   * PtLayout::Interlinear — emits an annotation row above the source line each sentence starts on.
// It used to live only in TooltipOverlay.cpp, whose TU pulls GfxRenderer / Page / HalStorage and
// therefore cannot link on the native host; the host test kept a hand-copied MIRROR of the grouping
// rule in sync by comment. This TU is pure text logic (SentenceSplitter + std::string), is listed in
// the `native` env's build_src_filter, and is tested directly.
//
// Everything here is deterministic and allocation-light: the only heap it touches is the caller's
// output strings. All fixed-size scratch lives in SentencePairScratch, which the CALLER owns — the
// three arrays total ~800 bytes, well past the <256 B stack-local budget, so it must not be a local.

// Per-source-sentence alignment state for ONE paragraph pair. Owned by the caller (heap, via
// makeUniqueNoThrow) and reused across pairs, so the mapping costs no per-paragraph allocation.
// The split results are outputs too: callers read origSplits to get each source sentence's word
// span.
struct SentencePairScratch {
  SentenceSplitResult origSplits;   // source sentence spans (word indices into the source array)
  SentenceSplitResult transSplits;  // translation sentence spans
  // Per SOURCE sentence os in [0, origSplits.count): the translation word span mapped to it.
  // startWord >= endWord means "no translation" (never emitted / never a step).
  SentenceSpan transFor[MAX_SENTENCES] = {};
  int origWordCount = 0;
  int transWordCount = 0;
  // Internal: translation sentence midpoints as a fraction of the paragraph's character length.
  float transMidFrac[MAX_SENTENCES] = {};
};

// Split both sides of a paragraph pair into sentences. Returns false (and leaves transFor untouched)
// when either side has no words or no sentence, which is the caller's "nothing to align" signal.
bool splitSentencePair(const char* const* origWords, int origWordCount, const char* const* transWords,
                       int transWordCount, SentencePairScratch& scratch);

// Merge "junk" sentences — a stray "." from a spaced ellipsis, a short fragment left by a page
// boundary — into the preceding sentence, so they never become a navigation step or an annotation
// row of their own. A sentence is junk when its match key is empty, or is <= 2 bytes over <= 3
// words. Walks backwards so a run of junk collapses in one pass. No-op for a single sentence.
void mergeJunkSentences(SentenceSplitResult& splits, const char* const* words);

// Map every source sentence in scratch.origSplits to a translation word span in scratch.transFor.
//
// THE RULE (unchanged from the original TooltipOverlay implementation): both sides are measured in
// cumulative characters; each translation sentence gets a midpoint expressed as a fraction of the
// translated paragraph; a source sentence takes every translation sentence whose midpoint falls in
// its own [startFrac, endFrac); if none does, it takes the single closest midpoint. Because the
// midpoints are monotonic and splitSentences' spans are contiguous, the matching translation
// sentences are always a contiguous run — which is why one span can express what used to be a
// joined string, at no loss (equal span <=> byte-identical text).
//
// Call splitSentencePair (and optionally mergeJunkSentences) first.
void mapSentenceSpans(const char* const* origWords, const char* const* transWords, SentencePairScratch& scratch);

// One navigation / annotation unit: the run of consecutive source sentences that share a
// translation. firstSentence..lastSentence are indices into the source sentence array.
struct SentenceStep {
  int16_t firstSentence;
  int16_t lastSentence;
};

// Collapse consecutive source sentences that resolve to the SAME translation into one step.
//
// Two mapping paths produce such duplicates: the engine merged K source sentences into ONE
// translated sentence (so all K map to the same translated span), or an unmatched sentence
// inherited a neighbour's translation via gap-fill. Without this the user sees the identical
// tooltip several presses in a row, and Interlinear would print the identical annotation row K
// times — the photo-verified PT_TOOLTIP bug. Empty (untranslated) sentences are never a step and
// terminate a run, so a page-boundary partial sentence stays out of a group's span.
//
// Two entry points, ONE rule (see the shared implementation in the .cpp):
//   * the string form is what the tooltip has after key matching + gap-fill;
//   * the span form is what mapSentenceSpans produces directly, and lets the layout path emit
//     translated words with no intermediate string at all.
// Returns the number of steps written (<= maxSteps).
int groupTranslationSteps(const std::vector<std::string>& sentenceTranslations, SentenceStep* out, int maxSteps);
int groupTranslationSpanSteps(const SentenceSpan* transFor, int count, SentenceStep* out, int maxSteps);

// Match key for a sentence: its first `n` meaningful words, normalized. Skips whitespace-only and
// single-character-punctuation words, strips leading em-space and trailing dots/ellipsis, and joins
// a hyphenated line-break fragment with the word that follows it ("over-" + "whelming").
std::string sentenceKey(const char* const* words, int start, int end, int n = 6);

// Join a span of words with single spaces. The inverse of the span form above, for callers that
// still need the text (the tooltip index).
std::string joinSpan(const std::vector<std::string>& words, const SentenceSpan& span);
