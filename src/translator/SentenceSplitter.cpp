#include "SentenceSplitter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TextNormalize.h"

// ── Word-array sentence splitting ────────────────────────────────────────────

// Common abbreviations that end with '.' but are not sentence boundaries.
static const char* const ABBREVIATIONS[] = {"Mr.",  "Mrs.", "Ms.",   "Dr.",   "Prof.",   "Sr.",  "Jr.", "St.",
                                            "Inc.", "Ltd.", "Corp.", "Gen.",  "Gov.",    "Sgt.", "vs.", "etc.",
                                            "Fig.", "Vol.", "Dept.", "Univ.", "approx.", nullptr};

// i.e. and e.g. handled specially (two-letter prefix + period)
static const char* const DOT_ABBREVIATIONS[] = {"i.e.", "e.g.", nullptr};

// Check if the word ending at the current position is an abbreviation.
static bool isAbbreviation(const char* lastWord) {
  if (!lastWord || *lastWord == '\0') return false;
  for (int i = 0; ABBREVIATIONS[i]; i++) {
    const size_t abbrLen = strlen(ABBREVIATIONS[i]);
    const size_t wordLen = strlen(lastWord);
    if (wordLen >= abbrLen && strcmp(lastWord + wordLen - abbrLen, ABBREVIATIONS[i]) == 0) {
      return true;
    }
  }
  for (int i = 0; DOT_ABBREVIATIONS[i]; i++) {
    const size_t abbrLen = strlen(DOT_ABBREVIATIONS[i]);
    const size_t wordLen = strlen(lastWord);
    if (wordLen >= abbrLen && strcmp(lastWord + wordLen - abbrLen, DOT_ABBREVIATIONS[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Classify how `word` terminates a sentence, funneling recognition through the
// shared textnorm primitives so every quote / dash / ellipsis form is canonical
// first (see TextNormalize.h). The word is folded, trailing closing quotes are
// skipped via textnorm::closingQuoteLenAt (post-fold they are ASCII " ' ) ],
// 1 byte each), then the final unit is tested with textnorm::terminatorLenAt.
// Returns:
//   0 = not a terminator
//   1 = HARD terminator (! ? , the ellipsis sentinel that "..."/"…" fold to, or
//       the CJK 。！？) — always a sentence boundary. Ellipsis now terminates.
//   2 = a single ASCII '.' — the caller applies abbreviation / spaced-run
//       exceptions before deciding to break.
static int classifyTerminator(const char* word) {
  if (!word || *word == '\0') return 0;
  std::string f = textnorm::foldForMatch(word);
  while (!f.empty() && textnorm::closingQuoteLenAt(f, f.size() - 1) == 1) f.pop_back();
  if (f.empty()) return 0;
  // CJK terminators survive the fold as 3 raw bytes at the end.
  if (f.size() >= 3 && textnorm::terminatorLenAt(f, f.size() - 3) == 3) return 1;
  if (textnorm::terminatorLenAt(f, f.size() - 1) == 1) {
    return (static_cast<uint8_t>(f.back()) == '.') ? 2 : 1;
  }
  return 0;
}

SentenceSplitResult splitSentences(const char* const* words, int wordCount) {
  SentenceSplitResult result;
  if (wordCount == 0) return result;

  int sentenceStart = 0;

  for (int i = 0; i < wordCount; i++) {
    const char* word = words[i];
    const int kind = classifyTerminator(word);

    if (kind == 0) continue;

    if (kind == 2) {  // single ASCII '.'
      if (isAbbreviation(word)) continue;

      // Spaced ellipsis handling: for a standalone "." word, check if more
      // "." words follow. If yes, skip this dot (middle of run). If no,
      // this is the LAST dot — let it create ONE sentence break for the run.
      // Result: ". . ." creates exactly one break (at the last dot), not three.
      if (strlen(word) == 1) {
        bool moreDots = (i + 1 < wordCount && strlen(words[i + 1]) == 1 && words[i + 1][0] == '.');
        if (moreDots) continue;  // middle of run — skip
        // Last dot in run: fall through to create sentence break.
      }
    }

    if (result.count < MAX_SENTENCES) {
      result.spans[result.count].startWord = sentenceStart;
      result.spans[result.count].endWord = i + 1;
      result.count++;
    }
    sentenceStart = i + 1;
  }

  if (sentenceStart < wordCount && result.count < MAX_SENTENCES) {
    result.spans[result.count].startWord = sentenceStart;
    result.spans[result.count].endWord = wordCount;
    result.count++;
  }

  if (result.count == 0 && wordCount > 0) {
    result.spans[0].startWord = 0;
    result.spans[0].endWord = wordCount;
    result.count = 1;
  }

  return result;
}

// ── Char-offset sentence helpers (over the shared textnorm fold) ──────────────

// UTF-8 byte length of the code point whose lead byte is b (1..4; 1 if invalid).
static int utf8CodePointLen(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}

// Byte length of a sentence terminator starting at raw text[o] (0 if none),
// matching the canonical set textnorm folds to. Wraps textnorm::terminatorLenAt
// (ASCII . ! ? and CJK 。！？ pass through the fold unchanged, so it works on raw)
// and adds the two forms the fold collapses that raw text still spells out: a run
// of >=2 ASCII dots (one ellipsis terminator) and U+2026 (E2 80 A6).
static int terminatorLenRaw(const std::string& t, size_t o) {
  const int n = textnorm::terminatorLenAt(t, o);
  if (n == 1 && t[o] == '.') {
    size_t run = 1;
    while (o + run < t.size() && t[o + run] == '.') run++;
    return static_cast<int>(run);  // "." => 1; ".."/"..." collapse to one terminator
  }
  if (n == 0 && o + 3 <= t.size() && static_cast<uint8_t>(t[o]) == 0xE2 && static_cast<uint8_t>(t[o + 1]) == 0x80 &&
      static_cast<uint8_t>(t[o + 2]) == 0xA6) {
    return 3;  // U+2026 … (raw terminatorLenAt only recognizes the folded sentinel)
  }
  return n;
}

// Byte length of a closing quote/bracket at raw text[o] (0 if none). Wraps
// textnorm::closingQuoteLenAt (ASCII " ' ) ]) and adds any fancy quote the fold
// maps to " or ' by folding the single code point at o.
static int closingQuoteLenRaw(const std::string& t, size_t o) {
  const int n = textnorm::closingQuoteLenAt(t, o);
  if (n) return n;
  const int cp = utf8CodePointLen(static_cast<uint8_t>(t[o]));
  if (cp > 1 && o + static_cast<size_t>(cp) <= t.size()) {
    const std::string folded = textnorm::foldForMatch(t.substr(o, cp));
    if (folded == "\"" || folded == "'") return cp;
  }
  return 0;
}

// Byte offsets in `text` immediately past each COMPLETE sentence: a terminator
// (plus any trailing closing quotes) that is followed by whitespace or the end of
// the string. Offsets index `text` itself so callers may slice the original for
// display. This is the ONE char-offset scanner the trim/count helpers wrap.
static std::vector<size_t> sentenceEndOffsets(const std::string& text) {
  std::vector<size_t> ends;
  size_t i = 0;
  while (i < text.size()) {
    const int tl = terminatorLenRaw(text, i);
    if (tl == 0) {
      i++;
      continue;
    }
    size_t next = i + static_cast<size_t>(tl);
    while (next < text.size()) {
      const int ql = closingQuoteLenRaw(text, next);
      if (ql == 0) break;
      next += static_cast<size_t>(ql);
    }
    // A complete sentence: terminator (+ trailing quotes) at end-of-string or
    // followed by any inter-token whitespace. textnorm::whitespaceLenAt is the
    // SSOT for "what separates" — it spans ASCII space AND Unicode NBSP-style
    // separators (U+00A0, U+2000..200A, U+202F, U+205F), which translation
    // services and EPUBs emit. Accepting only ASCII space here previously made
    // an NBSP-separated boundary invisible, undercounting the sentence and
    // shifting every downstream tooltip/modal mapping by one.
    if (next >= text.size() || textnorm::whitespaceLenAt(text, next) > 0) {
      ends.push_back(next);
    }
    i += static_cast<size_t>(tl);
  }
  return ends;
}

// Raw sentence count (honest zero when no complete sentence is present). Used by
// callers that need a true zero (e.g. countSentencesBefore).
static int countSentencesRaw(const std::string& text) {
  return text.empty() ? 0 : static_cast<int>(sentenceEndOffsets(text).size());
}

int countSentences(const std::string& text) { return text.empty() ? 0 : std::max(1, countSentencesRaw(text)); }

std::string trimToSentences(const std::string& text, int maxSentences) {
  if (maxSentences <= 0) return "";
  const std::vector<size_t> ends = sentenceEndOffsets(text);
  if (static_cast<int>(ends.size()) < maxSentences) return text;
  return text.substr(0, ends[maxSentences - 1]);
}

std::string trimToLastSentences(const std::string& text, int maxSentences) {
  if (maxSentences <= 0) return "";
  const std::vector<size_t> ends = sentenceEndOffsets(text);
  if (static_cast<int>(ends.size()) <= maxSentences) return text;
  // ends[K] = position after sentence K. Keep the last N of T sentences by
  // starting just after sentence (T-N-1), then skipping the separating
  // whitespace (ASCII or Unicode NBSP-style) so the result has no leading
  // separator byte.
  size_t startFrom = ends[ends.size() - static_cast<size_t>(maxSentences) - 1];
  while (startFrom < text.size()) {
    const int wl = textnorm::whitespaceLenAt(text, startFrom);
    if (wl == 0) break;
    startFrom += static_cast<size_t>(wl);
  }
  return text.substr(startFrom);
}

int countSentencesBefore(const std::string& origText, const std::string& visibleStart) {
  if (visibleStart.empty() || origText.empty()) return 0;
  const std::string needle = textnorm::foldForMatch(visibleStart, 40);
  if (needle.size() < 3) return 0;
  const std::string foldedOrig = textnorm::foldForMatch(origText);
  const auto pos = foldedOrig.find(needle);
  if (pos == std::string::npos) return 0;
  return countSentencesRaw(foldedOrig.substr(0, pos));
}
