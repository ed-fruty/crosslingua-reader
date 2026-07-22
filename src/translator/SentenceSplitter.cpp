#include "SentenceSplitter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ── Word-array sentence splitting (ported from fork's SentenceSplitter.cpp) ──

// Common abbreviations that end with '.' but are not sentence boundaries.
static const char* const ABBREVIATIONS[] = {"Mr.",  "Mrs.", "Ms.",   "Dr.",   "Prof.",   "Sr.",  "Jr.", "St.",
                                            "Inc.", "Ltd.", "Corp.", "Gen.",  "Gov.",    "Sgt.", "vs.", "etc.",
                                            "Fig.", "Vol.", "Dept.", "Univ.", "approx.", nullptr};

// i.e. and e.g. handled specially (two-letter prefix + period)
static const char* const DOT_ABBREVIATIONS[] = {"i.e.", "e.g.", nullptr};

// Check for trailing UTF-8 closing quote at word[0..len-1].
// Returns byte length of the closing quote (1 for ASCII, 2-3 for UTF-8), 0 if none.
// Matches readest tooltip-7 isClosingQuote: " ' ) ] » « " " „ ' '
static int trailingClosingQuote(const char* word, int len) {
  if (len <= 0) return 0;
  auto u = [](char c) -> uint8_t { return (uint8_t)c; };

  // ASCII closing quotes
  char c = word[len - 1];
  if (c == '"' || c == '\'' || c == ')' || c == ']') return 1;

  // 2-byte UTF-8: » U+00BB (0xC2 0xBB), « U+00AB (0xC2 0xAB)
  if (len >= 2) {
    uint8_t b0 = u(word[len - 2]), b1 = u(word[len - 1]);
    if (b0 == 0xC2 && (b1 == 0xBB || b1 == 0xAB)) return 2;
  }

  // 3-byte UTF-8: " U+201D (E2 80 9D), " U+201C (E2 80 9C),
  //   „ U+201E (E2 80 9E), ' U+2019 (E2 80 99), ' U+2018 (E2 80 98)
  if (len >= 3) {
    uint8_t b0 = u(word[len - 3]), b1 = u(word[len - 2]), b2 = u(word[len - 1]);
    if (b0 == 0xE2 && b1 == 0x80 && (b2 == 0x9D || b2 == 0x9C || b2 == 0x9E || b2 == 0x99 || b2 == 0x98)) {
      return 3;
    }
  }
  return 0;
}

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

// Check if the period at position `pos` in `word` is part of an ellipsis.
static bool isEllipsis(const char* word, int pos) {
  const int len = static_cast<int>(strlen(word));
  int dotCount = 0;
  int start = pos;
  while (start > 0 && word[start - 1] == '.') start--;
  while (start + dotCount < len && word[start + dotCount] == '.') dotCount++;
  return dotCount >= 3;
}

// Check if a word ends with a sentence terminator (. ! ?)
// Strips trailing UTF-8 and ASCII closing quotes before checking.
static char getTerminator(const char* word) {
  if (!word || *word == '\0') return '\0';
  int len = static_cast<int>(strlen(word));
  int cq;
  while (len > 0 && (cq = trailingClosingQuote(word, len)) > 0) len -= cq;
  if (len <= 0) return '\0';
  char c = word[len - 1];
  if (c == '.' || c == '!' || c == '?') return c;
  return '\0';
}

SentenceSplitResult splitSentences(const char* const* words, int wordCount) {
  SentenceSplitResult result;
  if (wordCount == 0) return result;

  int sentenceStart = 0;

  for (int i = 0; i < wordCount; i++) {
    const char* word = words[i];
    char terminator = getTerminator(word);

    if (terminator == '\0') continue;

    if (terminator == '.') {
      int len = static_cast<int>(strlen(word));
      int pos = len - 1;
      int cq;
      while (pos > 0 && (cq = trailingClosingQuote(word, pos + 1)) > 0) pos -= cq;
      if (isEllipsis(word, pos)) continue;
      if (isAbbreviation(word)) continue;

      // Spaced ellipsis handling: for a standalone "." word, check if more
      // "." words follow. If yes, skip this dot (middle of run). If no,
      // this is the LAST dot — let it create ONE sentence break for the run.
      // Result: ". . ." creates exactly one break (at the last dot), not three.
      if (len == 1) {
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

static int totalCharsInRange(const char* const* words, int start, int end) {
  int total = 0;
  for (int i = start; i < end; i++) {
    total += static_cast<int>(strlen(words[i]));
    if (i < end - 1) total++;
  }
  return total;
}

MappedSentenceResult mapSentenceTranslations(const char* const* originalWords, int originalCount,
                                             const char* const* translatedWords, int translatedCount,
                                             const SentenceSplitResult& splits, char* outBuffer, int outBufferSize) {
  MappedSentenceResult result;
  if (splits.count == 0 || translatedCount == 0) return result;

  SentenceSplitResult transSplits = splitSentences(translatedWords, translatedCount);

  const int origTotalChars = totalCharsInRange(originalWords, 0, originalCount);
  const int transTotalChars = totalCharsInRange(translatedWords, 0, translatedCount);

  int bufPos = 0;
  int transWordIdx = 0;
  int transCharAccum = 0;
  int origCharAccum = 0;

  for (int i = 0; i < splits.count && i < MAX_SENTENCES; i++) {
    result.sentences[i].original = splits.spans[i];

    origCharAccum += totalCharsInRange(originalWords, splits.spans[i].startWord, splits.spans[i].endWord);
    const float targetFraction = origTotalChars > 0 ? static_cast<float>(origCharAccum) / origTotalChars : 1.0f;

    const int transStart = transWordIdx;
    if (transSplits.count == splits.count) {
      transWordIdx = (i < transSplits.count) ? transSplits.spans[i].endWord : translatedCount;
    } else {
      while (transWordIdx < translatedCount) {
        transCharAccum += static_cast<int>(strlen(translatedWords[transWordIdx])) + 1;
        transWordIdx++;
        const float transFraction = transTotalChars > 0 ? static_cast<float>(transCharAccum) / transTotalChars : 1.0f;
        if (transFraction >= targetFraction - 0.01f || transWordIdx >= translatedCount) break;
      }
      if (i == splits.count - 1) {
        transWordIdx = translatedCount;
      }
    }

    const int textStart = bufPos;
    for (int j = transStart; j < transWordIdx && bufPos < outBufferSize - 1; j++) {
      if (j > transStart && bufPos < outBufferSize - 1) {
        outBuffer[bufPos++] = ' ';
      }
      const int wlen = static_cast<int>(strlen(translatedWords[j]));
      const int copyLen = (bufPos + wlen < outBufferSize - 1) ? wlen : (outBufferSize - 1 - bufPos);
      memcpy(outBuffer + bufPos, translatedWords[j], copyLen);
      bufPos += copyLen;
    }
    outBuffer[bufPos++] = '\0';

    result.sentences[i].translatedText = outBuffer + textStart;
    result.count++;
  }

  return result;
}

// ── Free-function sentence helpers (ported from fork's ModalOverlay.cpp lines 36-149) ──

// Count sentences in text. A sentence ends with . ! ? followed by space/end.
int countSentences(const std::string& text) {
  if (text.empty()) return 0;
  int count = 0;
  for (int i = 0; i < (int)text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      // Skip ellipsis (... or . . .)
      if (c == '.' && i + 1 < (int)text.size() && text[i + 1] == '.') continue;
      // Check followed by space, end, or closing quote
      int next = i + 1;
      // Skip closing quotes/brackets
      while (next < (int)text.size() && (text[next] == '"' || text[next] == '\'' || text[next] == ')' ||
                                         text[next] == ']' || (uint8_t)text[next] > 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        count++;
      }
    }
  }
  return std::max(1, count);  // At least 1 sentence per non-empty text
}

// Trim text to first N sentences. Returns the trimmed text.
std::string trimToSentences(const std::string& text, int maxSentences) {
  if (maxSentences <= 0) return "";
  int count = 0;
  for (int i = 0; i < (int)text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      if (c == '.' && i + 1 < (int)text.size() && text[i + 1] == '.') continue;
      int next = i + 1;
      while (next < (int)text.size() && (text[next] == '"' || text[next] == '\'' || text[next] == ')' ||
                                         text[next] == ']' || (uint8_t)text[next] > 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        count++;
        if (count >= maxSentences) {
          return text.substr(0, next);
        }
      }
    }
  }
  return text;  // Fewer sentences than max — return all
}

// Trim text to LAST N sentences.
std::string trimToLastSentences(const std::string& text, int maxSentences) {
  if (maxSentences <= 0) return "";
  // Find all sentence end positions
  std::vector<int> ends;
  for (int i = 0; i < (int)text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      if (c == '.' && i + 1 < (int)text.size() && text[i + 1] == '.') continue;
      int next = i + 1;
      while (next < (int)text.size() && (text[next] == '"' || text[next] == '\'' || text[next] == ')' ||
                                         text[next] == ']' || (uint8_t)text[next] > 0x80))
        next++;
      if (next >= (int)text.size() || text[next] == ' ' || text[next] == '\n') {
        ends.push_back(next);
      }
    }
  }
  if ((int)ends.size() <= maxSentences) return text;
  // ends[K] = position after sentence K. To keep last N of T sentences,
  // we drop first (T-N) sentences, starting from position after sentence (T-N-1).
  int startFrom = ends[ends.size() - maxSentences - 1];
  // Skip leading space
  while (startFrom < (int)text.size() && text[startFrom] == ' ') startFrom++;
  return text.substr(startFrom);
}

// Count sentences in origText that end BEFORE the position where visibleText starts.
// Used to determine how many sentences to skip in translation for tail paragraphs.
int countSentencesBefore(const std::string& origText, const std::string& visibleStart) {
  if (visibleStart.empty() || origText.empty()) return 0;

  // Normalize visibleStart: collapse whitespace, take first 40 chars
  std::string needle;
  bool lastSp = true;
  for (char c : visibleStart) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!lastSp) needle += ' ';
      lastSp = true;
    } else {
      needle += c;
      lastSp = false;
    }
    if ((int)needle.size() >= 40) break;
  }
  while (!needle.empty() && needle.back() == ' ') needle.pop_back();
  if (needle.size() < 3) return 0;

  // Normalize origText too
  std::string normOrig;
  lastSp = true;
  for (char c : origText) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!lastSp) normOrig += ' ';
      lastSp = true;
    } else {
      normOrig += c;
      lastSp = false;
    }
  }

  // Find where the visible text starts in the original
  auto pos = normOrig.find(needle);
  if (pos == std::string::npos) return 0;

  // Count sentences that end before this position
  return countSentences(normOrig.substr(0, pos));
}
