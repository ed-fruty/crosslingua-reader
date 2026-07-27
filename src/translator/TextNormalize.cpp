#include "TextNormalize.h"

namespace textnorm {

namespace {
inline bool isAsciiWs(uint8_t b) { return b == ' ' || b == '\n' || b == '\r' || b == '\t'; }
}  // namespace

int whitespaceLenAt(const std::string& text, size_t i) {
  if (i >= text.size()) return 0;
  const uint8_t b0 = static_cast<uint8_t>(text[i]);
  if (isAsciiWs(b0)) return 1;
  // U+00A0 NO-BREAK SPACE = C2 A0
  if (b0 == 0xC2 && i + 1 < text.size() && static_cast<uint8_t>(text[i + 1]) == 0xA0) return 2;
  // 3-byte Unicode spaces (E2 80 80..8A, E2 80 AF, E2 81 9F)
  if (b0 == 0xE2 && i + 2 < text.size()) {
    const uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
    const uint8_t b2 = static_cast<uint8_t>(text[i + 2]);
    if (b1 == 0x80 && b2 >= 0x80 && b2 <= 0x8A) return 3;  // U+2000..U+200A
    if (b1 == 0x80 && b2 == 0xAF) return 3;                // U+202F narrow NBSP
    if (b1 == 0x81 && b2 == 0x9F) return 3;                // U+205F medium math space
  }
  return 0;
}

void foldForMatchInPlace(std::string& s, int limit) {
  const size_t n = s.size();
  size_t r = 0;        // read cursor
  size_t w = 0;        // write cursor (always <= r)
  bool lastSp = true;  // treat a leading run of whitespace as already consumed

  while (r < n) {
    if (limit >= 0 && static_cast<int>(w) >= limit) break;
    const uint8_t b0 = static_cast<uint8_t>(s[r]);
    const int wsl = whitespaceLenAt(s, r);

    // ── Whitespace (ASCII + Unicode NBSP-style spaces) -> single space ──────
    // Collapse any run to one space; trailing space(s) are trimmed after the
    // loop. whitespaceLenAt is the SSOT for which bytes count as a separator.
    if (wsl > 0) {
      if (!lastSp) {
        s[w++] = ' ';
        lastSp = true;
      }
      r += static_cast<size_t>(wsl);
    }
    // ── 2-byte U+00xx (0xC2): soft hyphen, « » ──────────────────────────────
    else if (b0 == 0xC2 && r + 1 < n) {
      const uint8_t b1 = static_cast<uint8_t>(s[r + 1]);
      if (b1 == 0xAD) {  // soft hyphen -> drop (lastSp unchanged: invisible)
        r += 2;
      } else if (b1 == 0xAB || b1 == 0xBB) {  // « » -> "
        s[w++] = '"';
        lastSp = false;
        r += 2;
      } else {  // other U+00xx (accented Latin, etc.) -> passthrough
        s[w++] = static_cast<char>(b0);
        s[w++] = static_cast<char>(b1);
        lastSp = false;
        r += 2;
      }
    }
    // ── 2-byte U+02xx (0xCA): modifier letter apostrophe U+02BC ─────────────
    else if (b0 == 0xCA && r + 1 < n) {
      const uint8_t b1 = static_cast<uint8_t>(s[r + 1]);
      if (b1 == 0xBC) {  // ʼ -> '
        s[w++] = '\'';
      } else {
        s[w++] = static_cast<char>(b0);
        s[w++] = static_cast<char>(b1);
      }
      lastSp = false;
      r += 2;
    }
    // ── 3-byte U+2xxx (0xE2): fancy quotes, dashes, ellipsis, minus ─────────
    else if (b0 == 0xE2 && r + 2 < n) {
      const uint8_t b1 = static_cast<uint8_t>(s[r + 1]);
      const uint8_t b2 = static_cast<uint8_t>(s[r + 2]);
      char folded = 0;
      if (b1 == 0x80) {
        if (b2 == 0x90 || b2 == 0x93 || b2 == 0x94) {
          folded = '-';  // ‐ – —
        } else if (b2 == 0x98 || b2 == 0x99 || b2 == 0x9A) {
          folded = '\'';  // ‘ ’ ‚
        } else if (b2 == 0x9C || b2 == 0x9D || b2 == 0x9E || b2 == 0x9F) {
          folded = '"';  // “ ” „ ‟
        } else if (b2 == 0xA6) {
          folded = ELLIPSIS_SENTINEL;  // …
        }
      } else if (b1 == 0x88 && b2 == 0x92) {
        folded = '-';  // − U+2212 minus sign
      }
      if (folded != 0) {
        s[w++] = folded;
      } else {  // unrecognized U+2xxx -> passthrough
        s[w++] = static_cast<char>(b0);
        s[w++] = static_cast<char>(b1);
        s[w++] = static_cast<char>(b2);
      }
      lastSp = false;
      r += 3;
    }
    // ── ASCII dot run (>=2 dots) -> ellipsis sentinel ───────────────────────
    else if (b0 == '.') {
      size_t run = 1;
      while (r + run < n && s[r + run] == '.') run++;
      s[w++] = (run >= 2) ? ELLIPSIS_SENTINEL : '.';
      lastSp = false;
      r += run;
    }
    // ── literal byte ────────────────────────────────────────────────────────
    else {
      s[w++] = static_cast<char>(b0);
      lastSp = false;
      r += 1;
    }
  }

  // Trim trailing space(s).
  while (w > 0 && s[w - 1] == ' ') w--;
  s.resize(w);
}

std::string foldForMatch(const std::string& text, int limit) {
  std::string out(text);
  foldForMatchInPlace(out, limit);
  return out;
}

int terminatorLenAt(const std::string& text, size_t i) {
  if (i >= text.size()) return 0;
  const uint8_t b0 = static_cast<uint8_t>(text[i]);
  if (b0 == '.' || b0 == '!' || b0 == '?' || b0 == static_cast<uint8_t>(ELLIPSIS_SENTINEL)) {
    return 1;
  }
  // CJK terminators are unchanged by the fold: 。E3 80 82  ！EF BC 81  ？EF BC 9F
  if (i + 2 < text.size()) {
    const uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
    const uint8_t b2 = static_cast<uint8_t>(text[i + 2]);
    if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x82) return 3;
    if (b0 == 0xEF && b1 == 0xBC && (b2 == 0x81 || b2 == 0x9F)) return 3;
  }
  return 0;
}

int closingQuoteLenAt(const std::string& text, size_t i) {
  if (i >= text.size()) return 0;
  const char c = text[i];
  // Post-fold, every fancy quote is already ASCII " or '.
  if (c == '"' || c == '\'' || c == ')' || c == ']') return 1;
  return 0;
}

}  // namespace textnorm
