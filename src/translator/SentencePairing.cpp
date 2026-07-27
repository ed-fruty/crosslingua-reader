#include "SentencePairing.h"

#include <cmath>
#include <cstring>

namespace {

bool isAsciiSpace(const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// Cumulative character length of a word array, counting one separator per word — the same measure
// both sides of the mapping use, so the two fractions are comparable.
int charLen(const char* w) { return static_cast<int>(std::strlen(w)) + 1; }

// THE grouping rule, once. Parameterized on "is sentence i untranslated?" and "do sentences a and b
// carry the same translation?" so the string form and the span form cannot drift apart. Both
// instantiations live in this TU and are a few dozen instructions each.
template <typename IsEmptyFn, typename SameFn>
int groupSteps(const int total, IsEmptyFn isEmpty, SameFn same, SentenceStep* out, const int maxSteps) {
  int count = 0;
  int i = 0;
  while (i < total && count < maxSteps) {
    if (isEmpty(i)) {
      i++;  // untranslated sentence: not a step, and it terminates any current run
      continue;
    }
    int j = i;
    while (j + 1 < total && !isEmpty(j + 1) && same(j + 1, i)) {
      j++;
    }
    out[count].firstSentence = static_cast<int16_t>(i);
    out[count].lastSentence = static_cast<int16_t>(j);
    count++;
    i = j + 1;
  }
  return count;
}

}  // namespace

bool splitSentencePair(const char* const* origWords, const int origWordCount, const char* const* transWords,
                       const int transWordCount, SentencePairScratch& scratch) {
  scratch.origWordCount = origWordCount;
  scratch.transWordCount = transWordCount;
  scratch.origSplits.count = 0;
  scratch.transSplits.count = 0;
  if (origWordCount <= 0 || transWordCount <= 0) return false;
  scratch.origSplits = splitSentences(origWords, origWordCount);
  scratch.transSplits = splitSentences(transWords, transWordCount);
  return scratch.origSplits.count > 0 && scratch.transSplits.count > 0;
}

void mergeJunkSentences(SentenceSplitResult& splits, const char* const* words, const uint16_t* runStarts,
                        const int runCount) {
  const auto beginsAParagraph = [&](const uint16_t word) {
    if (runStarts == nullptr) return false;
    for (int r = 0; r < runCount; r++) {
      if (runStarts[r] == word) return true;
    }
    return false;
  };
  for (int i = splits.count - 1; i > 0; i--) {
    if (beginsAParagraph(splits.spans[i].startWord)) continue;  // never fold across a paragraph edge
    const std::string key = sentenceKey(words, splits.spans[i].startWord, splits.spans[i].endWord);
    if (key.empty() || (key.size() <= 2 && splits.spans[i].endWord - splits.spans[i].startWord <= 3)) {
      // Merge into the previous sentence: extend its endWord, then close the hole.
      splits.spans[i - 1].endWord = splits.spans[i].endWord;
      for (int j = i; j < splits.count - 1; j++) splits.spans[j] = splits.spans[j + 1];
      splits.count--;
    }
  }
}

void mapSentenceSpans(const char* const* origWords, const char* const* transWords, SentencePairScratch& scratch) {
  const SentenceSplitResult& origSplits = scratch.origSplits;
  const SentenceSplitResult& transSplits = scratch.transSplits;

  int origTotalChars = 0;
  for (int w = 0; w < scratch.origWordCount; w++) origTotalChars += charLen(origWords[w]);
  int transTotalChars = 0;
  for (int w = 0; w < scratch.transWordCount; w++) transTotalChars += charLen(transWords[w]);

  // Translation sentence midpoints, as a fraction of the translated paragraph.
  {
    int tCum = 0;
    for (int ts = 0; ts < transSplits.count; ts++) {
      const int tStart = tCum;
      for (int w = transSplits.spans[ts].startWord; w < transSplits.spans[ts].endWord; w++)
        tCum += charLen(transWords[w]);
      scratch.transMidFrac[ts] =
          transTotalChars > 0 ? static_cast<float>(tStart + tCum) / 2.0f / transTotalChars : 0.0f;
    }
  }

  int oCum = 0;
  for (int os = 0; os < origSplits.count; os++) {
    const int oStart = oCum;
    for (int w = origSplits.spans[os].startWord; w < origSplits.spans[os].endWord; w++) oCum += charLen(origWords[w]);
    const float origStartFrac = origTotalChars > 0 ? static_cast<float>(oStart) / origTotalChars : 0.0f;
    const float origEndFrac = origTotalChars > 0 ? static_cast<float>(oCum) / origTotalChars : 1.0f;

    // Every translation sentence whose midpoint falls in [origStart, origEnd). The midpoints are
    // monotonic and splitSentences' spans are contiguous, so the matches form one run: taking the
    // first match's startWord and the last match's endWord is exactly the text the previous
    // string-joining implementation produced.
    int firstTs = -1;
    int lastTs = -1;
    for (int ts = 0; ts < transSplits.count; ts++) {
      if (scratch.transMidFrac[ts] >= origStartFrac && scratch.transMidFrac[ts] < origEndFrac) {
        if (firstTs < 0) firstTs = ts;
        lastTs = ts;
      }
    }
    // Fallback: the closest midpoint when none fell in range.
    if (firstTs < 0) {
      const float origMid = (origStartFrac + origEndFrac) / 2.0f;
      int bestTs = 0;
      float bestDist = 999.0f;
      for (int ts = 0; ts < transSplits.count; ts++) {
        const float dist = std::fabs(scratch.transMidFrac[ts] - origMid);
        if (dist < bestDist) {
          bestDist = dist;
          bestTs = ts;
        }
      }
      firstTs = bestTs;
      lastTs = bestTs;
    }
    scratch.transFor[os].startWord = transSplits.spans[firstTs].startWord;
    scratch.transFor[os].endWord = transSplits.spans[lastTs].endWord;
  }
}

int groupTranslationSteps(const std::vector<std::string>& sentenceTranslations, SentenceStep* out, const int maxSteps) {
  return groupSteps(
      static_cast<int>(sentenceTranslations.size()), [&](const int i) { return sentenceTranslations[i].empty(); },
      [&](const int a, const int b) { return sentenceTranslations[a] == sentenceTranslations[b]; }, out, maxSteps);
}

int groupTranslationSpanSteps(const SentenceSpan* transFor, const int count, SentenceStep* out, const int maxSteps) {
  return groupSteps(
      count, [&](const int i) { return transFor[i].startWord >= transFor[i].endWord; },
      [&](const int a, const int b) {
        return transFor[a].startWord == transFor[b].startWord && transFor[a].endWord == transFor[b].endWord;
      },
      out, maxSteps);
}

std::string sentenceKey(const char* const* words, const int start, const int end, const int n) {
  std::string key;
  int count = 0;
  for (int i = start; i < end && count < n; i++) {
    const char* w = words[i];
    int wlen = static_cast<int>(std::strlen(w));
    if (wlen == 0) continue;
    // Skip whitespace-only words
    bool allSpace = true;
    for (int j = 0; j < wlen; j++) {
      if (!isAsciiSpace(w[j])) {
        allSpace = false;
        break;
      }
    }
    if (allSpace) continue;
    // Skip single-char punctuation words (spaced ellipsis ".", stray punctuation)
    if (wlen == 1 && !((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z') || (w[0] >= '0' && w[0] <= '9') ||
                       static_cast<uint8_t>(w[0]) >= 0x80))
      continue;
    // Skip Unicode ellipsis … (U+2026 = E2 80 A6) — standalone or as entire word
    if (wlen == 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
        static_cast<uint8_t>(w[2]) == 0xA6)
      continue;
    // Join hyphenated line-break fragments: "over-" + "whelming" → "overwhelming"
    if (wlen > 1 && w[wlen - 1] == '-' && i + 1 < end) {
      if (!key.empty()) key += ' ';
      key.append(w, wlen - 1);   // "over" (strip hyphen)
      key.append(words[i + 1]);  // + "whelming" → "overwhelming"
      i++;                       // skip the continuation word
      count++;
      continue;
    }
    // Strip leading em-space (U+2003 = E2 80 83)
    while (wlen >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
           static_cast<uint8_t>(w[2]) == 0x83) {
      w += 3;
      wlen -= 3;
    }
    // Strip trailing Unicode ellipsis … (U+2026 = E2 80 A6) — e.g., "relief…" → "relief"
    while (wlen >= 3 && static_cast<uint8_t>(w[wlen - 3]) == 0xE2 && static_cast<uint8_t>(w[wlen - 2]) == 0x80 &&
           static_cast<uint8_t>(w[wlen - 1]) == 0xA6) {
      wlen -= 3;
    }
    // Strip trailing ASCII dots (e.g., "word..." → "word")
    while (wlen > 0 && w[wlen - 1] == '.') wlen--;
    if (wlen == 0) continue;
    if (!key.empty()) key += ' ';
    key.append(w, wlen);
    count++;
  }
  return key;
}

std::string joinSpan(const std::vector<std::string>& words, const SentenceSpan& span) {
  std::string result;
  for (int i = span.startWord; i < span.endWord && i < static_cast<int>(words.size()); i++) {
    if (!result.empty()) result += ' ';
    result += words[i];
  }
  return result;
}
