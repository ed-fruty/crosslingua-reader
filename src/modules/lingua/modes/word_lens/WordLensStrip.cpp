#include "modules/lingua/modes/word_lens/WordLensStrip.h"

#include <cstring>

namespace lingua::wordlens {
namespace {

constexpr size_t MAX_STRIP_GLOSSES = 64;
constexpr char ELLIPSIS[] = "\xE2\x80\xA6";

int clampInt(const int v, const int lo, const int hi) { return v < lo ? lo : (v > hi ? hi : v); }

size_t codepointCount(const char* s) {
  size_t n = 0;
  for (; *s; s++) {
    if ((static_cast<uint8_t>(*s) & 0xC0) != 0x80) n++;
  }
  return n;
}

// Byte length of the first `n` codepoints of `s`.
size_t prefixBytes(const char* s, const size_t n) {
  size_t seen = 0;
  size_t i = 0;
  for (; s[i]; i++) {
    if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      if (seen == n) return i;
      seen++;
    }
  }
  return i;
}

bool overlapsPlaced(const StripGloss* glosses, const size_t count, const int x, const int width, const int gap) {
  for (size_t i = 0; i < count; i++) {
    const StripGloss& p = glosses[i];
    if (p.placed && x < p.x + p.width + gap && p.x < x + width + gap) return true;
  }
  return false;
}

// Longest "<prefix>…" of `text` (at least MIN_TRUNCATED_CODEPOINTS codepoints before the ellipsis)
// that fits `room`. Writes it to `out` and returns its width, or -1.
int truncateToFit(const char* text, const int room, char* out, StripMeasureFn measure, void* ctx) {
  const size_t total = codepointCount(text);
  if (total <= MIN_TRUNCATED_CODEPOINTS) return -1;
  for (size_t n = total - 1; n >= MIN_TRUNCATED_CODEPOINTS; n--) {
    size_t bytes = prefixBytes(text, n);
    while (bytes > 0 && text[bytes - 1] == ' ') bytes--;
    if (bytes + sizeof(ELLIPSIS) > STRIP_TEXT_BYTES) continue;
    std::memcpy(out, text, bytes);
    std::memcpy(out + bytes, ELLIPSIS, sizeof(ELLIPSIS));
    const int width = measure(ctx, out);
    if (width <= room) return width;
  }
  return -1;
}

}  // namespace

void placeStripGlosses(StripGloss* glosses, size_t count, const int lineWidth, const int minGap,
                       const StripMeasureFn measure, void* ctx) {
  if (count > MAX_STRIP_GLOSSES) {
    for (size_t i = MAX_STRIP_GLOSSES; i < count; i++) glosses[i].placed = false;
    count = MAX_STRIP_GLOSSES;
  }
  // Priority order: rarer words first, reading order among equals. A line holds a handful of
  // glosses, so an insertion sort over their indices is all this needs.
  uint8_t order[MAX_STRIP_GLOSSES];
  for (size_t i = 0; i < count; i++) {
    order[i] = static_cast<uint8_t>(i);
    glosses[i].placed = false;
    glosses[i].truncated = false;
  }
  for (size_t i = 1; i < count; i++) {
    const uint8_t v = order[i];
    size_t j = i;
    while (j > 0 && glosses[order[j - 1]].rank < glosses[v].rank) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }

  char shortened[STRIP_TEXT_BYTES];
  for (size_t k = 0; k < count; k++) {
    StripGloss& g = glosses[order[k]];
    if (g.text[0] == '\0' || lineWidth <= 0) continue;
    const int center = g.wordX + g.wordWidth / 2;
    int width = measure(ctx, g.text);
    if (width > lineWidth) {
      width = truncateToFit(g.text, lineWidth, shortened, measure, ctx);
      if (width < 0) continue;
      std::memcpy(g.text, shortened, std::strlen(shortened) + 1);
      g.truncated = true;
    }
    const int wanted = clampInt(center - width / 2, 0, lineWidth - width);
    if (!overlapsPlaced(glosses, count, wanted, width, minGap)) {
      g.x = static_cast<int16_t>(wanted);
      g.width = static_cast<int16_t>(width);
      g.placed = true;
      continue;
    }

    // Collision: find the free gap around the word's centre that the glosses already placed (all
    // of them rarer or earlier) leave.
    int left = 0;
    int right = lineWidth;
    bool blocked = false;
    for (size_t i = 0; i < count && !blocked; i++) {
      const StripGloss& p = glosses[i];
      if (!p.placed) continue;
      if (p.x + p.width <= center) {
        if (p.x + p.width + minGap > left) left = p.x + p.width + minGap;
      } else if (p.x >= center) {
        if (p.x - minGap < right) right = p.x - minGap;
      } else {
        blocked = true;  // a rarer word's gloss already covers this word's centre
      }
    }
    const int room = right - left;
    if (blocked || room <= 0) continue;
    if (width > room) {
      width = truncateToFit(g.text, room, shortened, measure, ctx);
      if (width < 0) continue;
      std::memcpy(g.text, shortened, std::strlen(shortened) + 1);
      g.truncated = true;
    }
    g.x = static_cast<int16_t>(clampInt(center - width / 2, left, right - width));
    g.width = static_cast<int16_t>(width);
    g.placed = true;
  }
}

}  // namespace lingua::wordlens
