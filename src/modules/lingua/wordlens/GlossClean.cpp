#include "modules/lingua/wordlens/GlossClean.h"

#include <cstring>
#include <vector>

namespace lingua::wordlens {
namespace {

using Text = std::u32string;

Text decode(const std::string_view in) {
  Text out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    const auto c = static_cast<uint8_t>(in[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0 && i + 3 < in.size()) {
      cp = ((c & 0x07u) << 18) | ((static_cast<uint8_t>(in[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<uint8_t>(in[i + 2]) & 0x3Fu) << 6) | (static_cast<uint8_t>(in[i + 3]) & 0x3Fu);
      n = 4;
    } else if (c >= 0xE0 && i + 2 < in.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<uint8_t>(in[i + 1]) & 0x3Fu) << 6) |
           (static_cast<uint8_t>(in[i + 2]) & 0x3Fu);
      n = 3;
    } else if (c >= 0xC0 && i + 1 < in.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(in[i + 1]) & 0x3Fu);
      n = 2;
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

size_t utf8Len(const uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

size_t utf8Len(const Text& t) {
  size_t n = 0;
  for (const uint32_t cp : t) n += utf8Len(cp);
  return n;
}

void encodeAppend(const uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string encode(const Text& t) {
  std::string out;
  out.reserve(utf8Len(t));
  for (const uint32_t cp : t) encodeAppend(cp, out);
  return out;
}

bool isWhitespace(const uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

bool isStrippedMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0487) || (cp >= 0x064B && cp <= 0x0652) ||
         cp == 0x0670;
}

bool isAsciiLetter(const uint32_t cp) { return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'); }
bool isAsciiAlnum(const uint32_t cp) { return isAsciiLetter(cp) || (cp >= '0' && cp <= '9'); }

bool isEdgeTrim(const uint32_t cp) {
  return cp == ' ' || cp == ',' || cp == ';' || cp == ':' || cp == 0x2013 || cp == 0x2014;
}

bool isSenseSeparator(const uint32_t cp, const bool monolingual) {
  return cp == 0xFF1B || cp == ';' || (!monolingual && cp == '/');
}

bool isSynonymSeparator(const uint32_t cp) { return cp == ',' || cp == 0xFF0C || cp == 0x3001; }

Text replaceAll(const Text& in, const Text& needle) {
  Text out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    if (in.compare(i, needle.size(), needle) == 0) {
      i += needle.size();
      continue;
    }
    out.push_back(in[i++]);
  }
  return out;
}

// Delete matched open..close spans; an unmatched opener is deleted on its own.
Text removeSpans(const Text& in, const uint32_t open, const uint32_t close) {
  Text out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    if (in[i] == open) {
      const size_t j = in.find(close, i + 1);
      i = (j != Text::npos) ? j + 1 : i + 1;
      continue;
    }
    out.push_back(in[i++]);
  }
  return out;
}

Text collapseWhitespace(const Text& in) {
  Text out;
  out.reserve(in.size());
  bool pendingSpace = false;
  for (const uint32_t cp : in) {
    if (isWhitespace(cp)) {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty()) out.push_back(' ');
    pendingSpace = false;
    out.push_back(cp);
  }
  return out;
}

Text stripSpaces(const Text& in) {
  size_t a = 0;
  size_t b = in.size();
  while (a < b && in[a] == ' ') a++;
  while (b > a && in[b - 1] == ' ') b--;
  return in.substr(a, b - a);
}

Text trimEdges(const Text& in) {
  size_t a = 0;
  size_t b = in.size();
  while (a < b && isEdgeTrim(in[a])) a++;
  while (b > a && isEdgeTrim(in[b - 1])) b--;
  return in.substr(a, b - a);
}

// Leading dictionary part-of-speech tags: "v." "adj. " "interj.".
Text stripLeadingPos(Text text) {
  for (;;) {
    size_t i = 0;
    while (i < text.size() && isWhitespace(text[i])) i++;
    size_t j = i;
    while (j < text.size() && j - i < 7 && isAsciiLetter(text[j])) j++;
    const size_t letters = j - i;
    if (letters >= 1 && letters <= 6 && j < text.size() && text[j] == '.') {
      size_t k = j + 1;
      while (k < text.size() && isWhitespace(text[k])) k++;
      text = text.substr(k);
      continue;
    }
    return text.substr(i);
  }
}

// A CC-CEDICT "CL:" classifier clause and everything after it.
Text cutClassifier(const Text& text) {
  size_t start = 0;
  for (;;) {
    const size_t k = text.find(U"CL:", start);
    if (k == Text::npos) return text;
    if (k == 0 || !isAsciiAlnum(text[k - 1])) return text.substr(0, k);
    start = k + 1;
  }
}

Text cleanSense(const Text& sense, const bool monolingual) {
  // Wiktionary link markup "[[word]]" keeps its text; a single-bracket "[...]" span is a pinyin /
  // domain annotation and goes with its content.
  Text s = replaceAll(replaceAll(sense, U"[["), U"]]");
  s = removeSpans(s, '[', ']');
  Text t;
  t.reserve(s.size());
  for (const uint32_t cp : s) {
    if (cp != ']' && cp != '{' && cp != '}' && cp != '|') t.push_back(cp);
  }
  s = removeSpans(t, '(', ')');
  t.clear();
  for (const uint32_t cp : s) {
    if (cp != ')' && !isStrippedMark(cp)) t.push_back(cp);
  }
  s = std::move(t);
  if (!monolingual) {
    s = cutClassifier(stripLeadingPos(std::move(s)));
  }
  s = stripSpaces(collapseWhitespace(s));
  if (!monolingual) {
    size_t cut = 0;
    while (cut < s.size() && !isSynonymSeparator(s[cut])) cut++;
    s = stripSpaces(s.substr(0, cut));
  }
  return trimEdges(s);
}

Text truncateUtf8(const Text& text, const size_t maxBytes) {
  if (utf8Len(text) <= maxBytes) return text;
  const size_t budget = maxBytes - (sizeof(GLOSS_ELLIPSIS) - 1);
  Text out;
  size_t used = 0;
  for (const uint32_t cp : text) {
    const size_t n = utf8Len(cp);
    if (used + n > budget) break;
    out.push_back(cp);
    used += n;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  out.push_back(0x2026);
  return out;
}

}  // namespace

std::string cleanGloss(const std::string_view raw, const bool monolingual) {
  const Text all = decode(raw);
  std::vector<Text> senses;
  senses.reserve(GLOSS_MAX_SENSES);
  size_t total = 0;
  const size_t joinerBytes = sizeof(GLOSS_SENSE_JOINER) - 1;
  size_t start = 0;
  for (size_t i = 0; i <= all.size(); i++) {
    if (i < all.size() && !isSenseSeparator(all[i], monolingual)) continue;
    Text s = cleanSense(all.substr(start, i - start), monolingual);
    start = i + 1;
    if (s.empty()) continue;
    bool duplicate = false;
    for (const Text& kept : senses) duplicate = duplicate || kept == s;
    if (duplicate) continue;
    if (senses.empty()) {
      s = truncateUtf8(s, GLOSS_MAX_BYTES);
      total = utf8Len(s);
      senses.push_back(std::move(s));
    } else {
      const size_t add = joinerBytes + utf8Len(s);
      if (total + add > GLOSS_MAX_BYTES) break;
      total += add;
      senses.push_back(std::move(s));
    }
    if (senses.size() >= GLOSS_MAX_SENSES) break;
  }
  std::string out;
  out.reserve(total);
  for (size_t i = 0; i < senses.size(); i++) {
    if (i > 0) out += GLOSS_SENSE_JOINER;
    out += encode(senses[i]);
  }
  return out;
}

size_t truncateCodepoints(char* text, size_t len, const size_t maxCodepoints, const size_t capacity) {
  size_t count = 0;
  size_t cutAt = len;  // byte offset of codepoint number (maxCodepoints - 1)
  for (size_t i = 0; i < len; i++) {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) continue;
    if (count == maxCodepoints - 1) cutAt = i;
    count++;
  }
  if (count <= maxCodepoints || maxCodepoints == 0) {
    text[len] = '\0';
    return len;
  }
  len = cutAt;
  while (len > 0 && text[len - 1] == ' ') len--;
  const size_t ell = sizeof(GLOSS_ELLIPSIS) - 1;
  if (len + ell + 1 > capacity) len = capacity > ell + 1 ? capacity - ell - 1 : 0;
  std::memcpy(text + len, GLOSS_ELLIPSIS, ell);
  len += ell;
  text[len] = '\0';
  return len;
}

size_t displayGloss(const std::string_view stored, char* out, const size_t outSize) {
  if (outSize == 0) return 0;
  size_t firstLen = stored.find(GLOSS_SENSE_JOINER);
  if (firstLen == std::string_view::npos) firstLen = stored.size();
  if (firstLen >= outSize) firstLen = outSize - 1;
  std::memcpy(out, stored.data(), firstLen);
  return truncateCodepoints(out, firstLen, GLOSS_DISPLAY_CODEPOINTS, outSize);
}

}  // namespace lingua::wordlens
