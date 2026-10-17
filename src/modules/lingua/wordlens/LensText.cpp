#include "modules/lingua/wordlens/LensText.h"

namespace lingua::wordlens {
namespace {

uint32_t decodeAt(const uint8_t* s, const size_t len, size_t& i) {
  const uint8_t c = s[i];
  if (c < 0x80) {
    i += 1;
    return c;
  }
  if (c >= 0xF0) {
    if (i + 3 >= len) {
      i = len;
      return 0xFFFD;
    }
    const uint32_t cp =
        ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
    i += 4;
    return cp;
  }
  if (c >= 0xE0) {
    if (i + 2 >= len) {
      i = len;
      return 0xFFFD;
    }
    const uint32_t cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
    i += 3;
    return cp;
  }
  if (c >= 0xC0) {
    if (i + 1 >= len) {
      i = len;
      return 0xFFFD;
    }
    const uint32_t cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
    i += 2;
    return cp;
  }
  i += 1;  // stray continuation byte
  return 0xFFFD;
}

size_t encodeInto(const uint32_t cp, char* out, const size_t pos, const size_t outSize) {
  const size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
  if (pos + n >= outSize) return 0;
  if (n == 1) {
    out[pos] = static_cast<char>(cp);
  } else if (n == 2) {
    out[pos] = static_cast<char>(0xC0 | (cp >> 6));
    out[pos + 1] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (n == 3) {
    out[pos] = static_cast<char>(0xE0 | (cp >> 12));
    out[pos + 1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[pos + 2] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out[pos] = static_cast<char>(0xF0 | (cp >> 18));
    out[pos + 1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[pos + 2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[pos + 3] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return n;
}

bool isCombining(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
         (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

bool isApostrophe(const uint32_t cp) { return cp == '\'' || cp == 0x2019 || cp == 0x02BC; }

bool isLetter(const uint32_t cp) { return isWordCodepoint(cp) && !isCombining(cp); }

}  // namespace

uint32_t foldCase(const uint32_t cp) {
  if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 0x20 : cp;
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
  if (cp >= 0x0100 && cp <= 0x017F) {
    if (cp == 0x0130) return 'i';
    if (cp == 0x0178) return 0xFF;
    if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E)) return (cp & 1) ? cp + 1 : cp;
    if (cp == 0x0138 || cp == 0x0149 || cp == 0x017F) return cp;
    return (cp & 1) ? cp : cp + 1;
  }
  if (cp >= 0x0391 && cp <= 0x03AB && cp != 0x03A2) return cp + 0x20;
  if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
  if (cp >= 0x0460 && cp <= 0x04BF) return (cp & 1) ? cp : cp + 1;
  if (cp >= 0x04D0 && cp <= 0x052F) return (cp & 1) ? cp : cp + 1;
  if (cp >= 0x1E00 && cp <= 0x1EFF) return (cp & 1) ? cp : cp + 1;
  return cp;
}

bool isWordCodepoint(const uint32_t cp) {
  if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
  if (cp < 0xC0) return cp == 0xAA || cp == 0xB5 || cp == 0xBA;
  if (cp == 0xD7 || cp == 0xF7) return false;
  if (cp >= 0x2000 && cp <= 0x2BFF) return false;  // punctuation, symbols, arrows, dingbats
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;  // supplemental punctuation
  if (cp >= 0x3000 && cp <= 0x303F) return false;  // CJK punctuation
  if (cp >= 0xFE30 && cp <= 0xFE6F) return false;  // CJK compatibility / small forms
  if (cp >= 0xFF00 && cp <= 0xFF20) return false;  // fullwidth punctuation and digits
  if (cp >= 0xFF3B && cp <= 0xFF40) return false;
  if (cp >= 0xFF5B && cp <= 0xFF65) return false;
  if (cp == 0xFEFF || cp == 0xFFFD) return false;
  return true;
}

size_t normalizeLookupKey(const char* token, const size_t len, char* out, const size_t outSize, TokenInfo& info) {
  info = TokenInfo{};
  if (outSize == 0) return 0;
  out[0] = '\0';
  const auto* s = reinterpret_cast<const uint8_t*>(token);

  // Trim to the first and last LETTER: quotes, brackets, dashes and sentence punctuation around a
  // word never belong to it, while an apostrophe or hyphen between letters does ("don't",
  // "well-known").
  size_t start = len;
  size_t end = 0;
  for (size_t i = 0; i < len;) {
    const size_t at = i;
    const uint32_t cp = decodeAt(s, len, i);
    if (cp >= '0' && cp <= '9') return 0;
    if (isLetter(cp)) {
      if (start == len) start = at;
      end = i;
    }
  }
  if (start >= end) return 0;

  size_t pos = 0;
  bool sawLower = false;
  bool firstLetter = true;
  for (size_t i = start; i < end;) {
    uint32_t cp = decodeAt(s, len, i);
    if (isCombining(cp)) continue;
    if (isApostrophe(cp)) {
      cp = '\'';
    } else if (cp == 0x2010 || cp == 0x2011) {
      cp = '-';
    } else if (isLetter(cp)) {
      const uint32_t lower = foldCase(cp);
      if (firstLetter) info.capitalized = lower != cp;
      firstLetter = false;
      if (lower == cp) sawLower = true;
      cp = lower;
      if (info.letters < 255) info.letters++;
    } else if (cp != '-' && cp != '.') {
      // Anything else inside a word (a slash, a symbol) means this is not a single headword.
      return 0;
    }
    const size_t n = encodeInto(cp, out, pos, outSize);
    if (n == 0 || pos + n > LOOKUP_KEY_MAX_BYTES) {
      out[0] = '\0';
      return 0;
    }
    pos += n;
  }
  out[pos] = '\0';
  info.allCaps = info.letters > 0 && !sawLower;
  return info.letters > 0 ? pos : 0;
}

bool endsSentence(const char* token, const size_t len) {
  const auto* s = reinterpret_cast<const uint8_t*>(token);
  uint32_t last = 0;
  for (size_t i = 0; i < len;) {
    const uint32_t cp = decodeAt(s, len, i);
    // Closing quotes and brackets after the full stop do not end anything themselves.
    const bool closer = cp == '"' || cp == '\'' || cp == ')' || cp == ']' || cp == 0x201D || cp == 0x2019 ||
                        cp == 0x00BB || cp == 0x203A || cp == 0x201C;
    if (!closer) last = cp;
  }
  return last == '.' || last == '!' || last == '?' || last == 0x2026 || last == ':';
}

}  // namespace lingua::wordlens
