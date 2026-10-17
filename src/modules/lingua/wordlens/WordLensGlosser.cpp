#include "modules/lingua/wordlens/WordLensGlosser.h"

#include <cstring>

#include "modules/lingua/wordlens/WordLensLevels.h"

namespace lingua::wordlens {
namespace {

bool endsWith(const char* w, const size_t n, const char* suffix) {
  const size_t s = std::strlen(suffix);
  return n >= s && std::memcmp(w + n - s, suffix, s) == 0;
}

bool startsWith(const char* w, const char* prefix) { return std::strncmp(w, prefix, std::strlen(prefix)) == 0; }

struct CandidateSet {
  char (*out)[CANDIDATE_BYTES];
  size_t max;
  size_t count = 0;
  // Readest adds a candidate only when it is at least 2 characters long, and keeps insertion order.
  void add(const char* base, const size_t baseLen, const char* tail = "") {
    const size_t tailLen = std::strlen(tail);
    if (baseLen + tailLen < 2 || baseLen + tailLen >= CANDIDATE_BYTES || count >= max) return;
    char tmp[CANDIDATE_BYTES];
    std::memcpy(tmp, base, baseLen);
    std::memcpy(tmp + baseLen, tail, tailLen);
    tmp[baseLen + tailLen] = '\0';
    for (size_t i = 0; i < count; i++) {
      if (std::strcmp(out[i], tmp) == 0) return;
    }
    std::memcpy(out[count++], tmp, baseLen + tailLen + 1);
  }
};

bool isHan(const uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x2FDF) || cp == 0x3005 || cp == 0x3007 || (cp >= 0x3021 && cp <= 0x3029) ||
         (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x323AF);
}

uint32_t nextCp(const char*& p) {
  const auto c = static_cast<uint8_t>(*p);
  if (c < 0x80) {
    p += 1;
    return c;
  }
  const int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
  uint32_t cp = c & (n == 4 ? 0x07 : n == 3 ? 0x0F : 0x1F);
  for (int i = 1; i < n; i++) {
    if ((static_cast<uint8_t>(p[i]) & 0xC0) != 0x80) {
      p += i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (static_cast<uint8_t>(p[i]) & 0x3F);
  }
  p += n;
  return cp;
}

bool isParticle(const uint32_t cp) {
  static constexpr uint32_t PARTICLES[] = {0x7684, 0x5730, 0x5F97, 0x4E86, 0x7740, 0x4E4B, 0x5417, 0x5462, 0x554A,
                                           0xFF0C, ',',    0xFF1B, ';',    0x3001, 0x3002, '.',    '/'};
  if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) return true;
  for (const uint32_t p : PARTICLES) {
    if (cp == p) return true;
  }
  return false;
}

bool containsCp(const char* text, const uint32_t wanted) {
  const char* p = text;
  while (*p) {
    if (nextCp(p) == wanted) return true;
  }
  return false;
}

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 0x20) : c; }
bool isAsciiAlpha(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiWord(const char c) { return isAsciiAlpha(c) || (c >= '0' && c <= '9') || c == '_'; }

}  // namespace

size_t englishBaseCandidates(const char* word, char (*out)[CANDIDATE_BYTES], const size_t maxOut) {
  const size_t n = std::strlen(word);
  for (size_t i = 0; i < n; i++) {
    if (!(word[i] >= 'a' && word[i] <= 'z')) return 0;  // lowercase ASCII keys only
  }
  CandidateSet set{out, maxOut};
  if (endsWith(word, n, "ily") && n >= 5) set.add(word, n - 3, "y");
  if (endsWith(word, n, "ly") && n >= 4) {
    set.add(word, n - 2);
    set.add(word, n - 2, "e");
  }
  if (endsWith(word, n, "ful") && n >= 5) {
    set.add(word, n - 3);
    set.add(word, n - 4, "y");
  }
  if (endsWith(word, n, "wards") && n >= 6) {
    set.add(word, n - 1);
    set.add(word, n - 5);
  } else if (endsWith(word, n, "ward") && n >= 5) {
    set.add(word, n - 4);
  }
  if (endsWith(word, n, "ness") && n >= 5) {
    set.add(word, n - 4);
    set.add(word, n - 5, "y");
  }
  if (endsWith(word, n, "less") && n >= 5) set.add(word, n - 4);
  if ((endsWith(word, n, "able") || endsWith(word, n, "ible")) && n >= 6) {
    set.add(word, n - 4);
    set.add(word, n - 4, "e");
  }
  // Negative prefixes, peeled off the word and off every suffix candidate found so far.
  static constexpr const char* PREFIXES[] = {"un", "in", "im", "ir", "il"};
  const size_t suffixCount = set.count;
  for (size_t s = 0; s <= suffixCount; s++) {
    char stem[CANDIDATE_BYTES];
    const char* src = s == 0 ? word : out[s - 1];
    const size_t len = std::strlen(src);
    if (len >= CANDIDATE_BYTES) continue;
    std::memcpy(stem, src, len + 1);
    for (const char* p : PREFIXES) {
      if (startsWith(stem, p) && len >= 2 + 3) set.add(stem + 2, len - 2);
    }
  }
  // The word itself is never its own base.
  size_t kept = 0;
  for (size_t i = 0; i < set.count; i++) {
    if (std::strcmp(out[i], word) == 0) continue;
    if (kept != i) std::memcpy(out[kept], out[i], CANDIDATE_BYTES);
    kept++;
  }
  return kept;
}

bool glossesShareMeaning(const char* a, const char* b) {
  bool aHasContent = false;
  bool bHasContent = false;
  bool aHasHan = false;
  for (const char* p = a; *p;) {
    const uint32_t cp = nextCp(p);
    if (isParticle(cp)) continue;
    aHasContent = true;
    aHasHan = aHasHan || isHan(cp);
  }
  for (const char* p = b; *p;) {
    if (!isParticle(nextCp(p))) bHasContent = true;
  }
  if (!aHasContent || !bHasContent) return false;
  if (aHasHan) {
    for (const char* p = a; *p;) {
      const uint32_t cp = nextCp(p);
      if (isHan(cp) && !isParticle(cp) && containsCp(b, cp)) return true;
    }
    return false;
  }
  // ASCII words of 3+ letters, compared case-insensitively.
  for (const char* p = a; *p;) {
    if (!isAsciiAlpha(*p)) {
      p++;
      continue;
    }
    const char* start = p;
    while (isAsciiAlpha(*p)) p++;
    const size_t len = static_cast<size_t>(p - start);
    if (len < 3) continue;
    for (const char* q = b; *q;) {
      if (!isAsciiAlpha(*q)) {
        q++;
        continue;
      }
      const char* qs = q;
      while (isAsciiAlpha(*q)) q++;
      if (static_cast<size_t>(q - qs) != len) continue;
      bool same = true;
      for (size_t i = 0; i < len && same; i++) same = asciiLower(start[i]) == asciiLower(qs[i]);
      if (same) return true;
    }
  }
  return false;
}

bool glossMentionsWord(const char* gloss, const char* word) {
  const size_t wl = std::strlen(word);
  if (wl < 3) return false;
  const size_t gl = std::strlen(gloss);
  for (size_t i = 0; i + wl <= gl; i++) {
    bool same = true;
    for (size_t k = 0; k < wl && same; k++) same = asciiLower(gloss[i + k]) == asciiLower(word[k]);
    if (!same) continue;
    const bool leftOk = i == 0 || !isAsciiWord(gloss[i - 1]);
    const bool rightOk = i + wl == gl || !isAsciiWord(gloss[i + wl]);
    if (leftOk && rightOk) return true;
  }
  return false;
}

void WordLensGlosser::configure(LensReader* reader, const uint8_t level, const bool keepEmptyStrips) {
  reader_ = reader;
  keepEmptyStrips_ = keepEmptyStrips;
  cutoff_ = 0;
  englishSource_ = false;
  capitalsMarkNames_ = true;
  if (!reader || !reader->isOpen()) return;
  const LensHeader& h = reader->header();
  cutoff_ = rankCutoff(level, h.rankScale);
  englishSource_ = std::strcmp(h.source, "en") == 0;
  capitalsMarkNames_ = std::strcmp(h.source, "de") != 0;
}

uint16_t WordLensGlosser::derivedRank(const char* key, const size_t keyLength, const LensReader::Hit& hit) {
  (void)keyLength;
  const size_t count = englishBaseCandidates(key, candidates_, MAX_CANDIDATES);
  if (count == 0) return hit.rank;
  bool ownLoaded = false;
  uint16_t rank = hit.rank;
  for (size_t i = 0; i < count; i++) {
    LensReader::Hit base;
    if (!reader_->lookup(candidates_[i], std::strlen(candidates_[i]), base) || base.rank >= rank) continue;
    if (!ownLoaded) {
      if (!reader_->readEntry(hit.entryOffset, nullptr, 0, ownGloss_, sizeof(ownGloss_))) return rank;
      ownLoaded = true;
    }
    if (!reader_->readEntry(base.entryOffset, nullptr, 0, otherGloss_, sizeof(otherGloss_))) continue;
    if (glossesShareMeaning(ownGloss_, otherGloss_) || glossMentionsWord(ownGloss_, candidates_[i])) {
      rank = base.rank;
    }
  }
  return rank;
}

bool WordLensGlosser::evaluate(const char* word, const size_t length, const bool sentenceInitial, Decision& out) {
  if (!ready()) return false;
  TokenInfo info;
  const size_t keyLength = normalizeLookupKey(word, length, key_, sizeof(key_), info);
  if (keyLength == 0) return false;
  // A capitalised word in mid-sentence is taken for a name ("Paris", "Hope"). Words set wholly in
  // capitals are left alone: that is a heading or emphasis, not a name.
  if (capitalsMarkNames_ && info.capitalized && !info.allCaps && !sentenceInitial) return false;
  LensReader::Hit hit;
  if (!reader_->lookup(key_, keyLength, hit) || !isDifficult(hit.rank, cutoff_)) return false;
  uint16_t rank = hit.rank;
  if (englishSource_) {
    rank = derivedRank(key_, keyLength, hit);
    if (!isDifficult(rank, cutoff_)) return false;
  }
  out.rank = rank;
  out.entryOffset = hit.entryOffset;
  return true;
}

bool WordLensGlosser::lookupEntry(const char* word, const size_t length, Decision& out) {
  if (!ready()) return false;
  TokenInfo info;
  const size_t keyLength = normalizeLookupKey(word, length, key_, sizeof(key_), info);
  LensReader::Hit hit;
  if (keyLength == 0 || !reader_->lookup(key_, keyLength, hit)) return false;
  out.rank = hit.rank;
  out.entryOffset = hit.entryOffset;
  return true;
}

bool WordLensGlosser::glossFor(const char* word, const size_t length, const bool sentenceInitial, WordLensGloss& out) {
  Decision decision;
  if (!evaluate(word, length, sentenceInitial, decision)) return false;
  if (!reader_->readEntry(decision.entryOffset, nullptr, 0, ownGloss_, sizeof(ownGloss_))) return false;
  if (displayGloss(ownGloss_, out.text, sizeof(out.text)) == 0) return false;
  out.rank = decision.rank;
  return true;
}

}  // namespace lingua::wordlens
