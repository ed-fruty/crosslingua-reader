#include "modules/lingua/modes/word_lens/WordLensNavigation.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstring>

#include "modules/lingua/wordlens/LensText.h"

namespace lingua::wordlens {
namespace {

constexpr size_t MAX_JOINED_BYTES = 96;

// Byte range of the letters of `word` (see the layout's letterSpan).
void letters(const char* word, size_t& first, size_t& end) {
  const size_t len = std::strlen(word);
  first = len;
  end = 0;
  const auto* start = reinterpret_cast<const unsigned char*>(word);
  const auto* p = start;
  while (*p) {
    const auto at = static_cast<size_t>(p - start);
    if (isWordCodepoint(utf8NextCodepoint(&p))) {
      if (at < first) first = at;
      end = static_cast<size_t>(p - start);
    }
  }
  if (end <= first) {
    first = 0;
    end = len;
  }
}

int advance(const GfxRenderer& renderer, const int fontId, const char* word, const size_t bytes,
            const EpdFontFamily::Style style, const int8_t tracking) {
  if (bytes == 0) return 0;
  char buf[MAX_JOINED_BYTES + 1];
  const size_t n = bytes < MAX_JOINED_BYTES ? bytes : MAX_JOINED_BYTES;
  std::memcpy(buf, word, n);
  buf[n] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, style, tracking);
}

const PageLine* bodyLine(const Page& page, const size_t element) {
  const auto& el = page.elements[element];
  if (el->getTag() != TAG_PageLine) return nullptr;
  const auto* line = static_cast<const PageLine*>(el.get());
  if (line->fontRole != LineFontRole::Body || !line->getBlock()) return nullptr;
  return line;
}

// Find a marked word's pack entry from its laid-out text: the word itself, then (a word hyphenated at
// the line end) joined with the next body line's first word, then glued to the words after it on
// the line (an inline tag inside a word: a drop cap, "<b>W</b>hispered").
bool findEntry(const Page& page, const size_t element, const TextBlock& block, const uint16_t w, EntryFn entryFor,
               void* ctx, char* joined, uint16_t& rank, uint32_t& entry, const char*& surface, size_t& surfaceLen) {
  surface = block.wordText(w);
  surfaceLen = block.wordTextLen(w);
  if (entryFor(ctx, surface, surfaceLen, rank, entry)) return true;
  if (w + 1 == block.wordCount() && surfaceLen > 1 && surface[surfaceLen - 1] == '-') {
    for (size_t n = element + 1; n < page.elements.size(); n++) {
      const PageLine* next = bodyLine(page, n);
      if (!next) continue;
      if (next->getBlock()->wordCount() == 0) break;
      const size_t tailLen = next->getBlock()->wordTextLen(0);
      if (surfaceLen - 1 + tailLen > MAX_JOINED_BYTES) break;
      std::memcpy(joined, surface, surfaceLen - 1);
      std::memcpy(joined + surfaceLen - 1, next->getBlock()->wordText(0), tailLen);
      if (entryFor(ctx, joined, surfaceLen - 1 + tailLen, rank, entry)) {
        surface = joined;
        surfaceLen = surfaceLen - 1 + tailLen;
        return true;
      }
      break;
    }
  }
  size_t len = 0;
  for (uint16_t k = w; k < block.wordCount() && k < w + 4; k++) {
    const size_t partLen = block.wordTextLen(k);
    if (len + partLen > MAX_JOINED_BYTES) break;
    std::memcpy(joined + len, block.wordText(k), partLen);
    len += partLen;
    if (k > w && entryFor(ctx, joined, len, rank, entry)) {
      surface = joined;
      surfaceLen = len;
      return true;
    }
  }
  return false;
}

}  // namespace

void WordLensNavigator::clear() {
  count_ = 0;
  focus_ = -1;
}

void WordLensNavigator::collect(const GfxRenderer& renderer, const Page& page, const int bodyFontId,
                                const EntryFn entryFor, void* ctx) {
  clear();
  const int lineHeight = renderer.getLineHeight(bodyFontId);
  char joined[MAX_JOINED_BYTES + 1];
  const size_t elements = page.elements.size();
  for (size_t e = 0; e + 1 < elements && count_ < MAX_TARGETS; e++) {
    const auto& el = page.elements[e];
    if (el->getTag() != TAG_PageLine) continue;
    const auto* strip = static_cast<const PageLine*>(el.get());
    if (strip->fontRole != LineFontRole::Annotation || !strip->getBlock()) continue;
    // The strip's source line is the element right after it (the layout emits them as a pair).
    const PageLine* line = bodyLine(page, e + 1);
    if (!line) continue;
    const TextBlock& markers = *strip->getBlock();
    const TextBlock& block = *line->getBlock();
    const int8_t tracking = block.getBlockStyle().characterSpacing;
    for (uint16_t m = 0; m < markers.wordCount() && count_ < MAX_TARGETS; m++) {
      if (markers.wordTextLen(m) != 0) continue;  // a gloss, not a marker
      uint16_t w = 0;
      bool found = false;
      for (uint16_t k = 0; k < block.wordCount() && !found; k++) {
        if (block.wordXpos(k) == markers.wordXpos(m)) {
          w = k;
          found = true;
        }
      }
      if (!found) continue;
      NavTarget& t = targets_[count_];
      const char* surface = nullptr;
      size_t surfaceLen = 0;
      t.hasEntry = findEntry(page, e + 1, block, w, entryFor, ctx, joined, t.rank, t.entryOffset, surface, surfaceLen);
      TokenInfo info;
      normalizeLookupKey(surface, surfaceLen, t.surface, sizeof(t.surface), info);
      const char* word = block.wordText(w);
      size_t first = 0;
      size_t end = 0;
      letters(word, first, end);
      const int x0 = block.wordXpos(w) + advance(renderer, bodyFontId, word, first, block.wordStyle(w), tracking);
      const int x1 = block.wordXpos(w) + advance(renderer, bodyFontId, word, end, block.wordStyle(w), tracking);
      t.element = static_cast<uint16_t>(e + 1);
      t.word = w;
      t.x = static_cast<int16_t>(line->xPos + x0);
      t.y = line->yPos;
      t.width = static_cast<int16_t>(x1 > x0 ? x1 - x0 : 1);
      t.height = static_cast<int16_t>(lineHeight);
      count_++;
    }
  }
}

WordLensNavigator::Step WordLensNavigator::next(const bool loop) {
  if (count_ == 0) return Step::NoTargets;
  if (focus_ < 0) {
    focus_ = 0;
    return Step::Moved;
  }
  if (static_cast<size_t>(focus_ + 1) < count_) {
    focus_++;
    return Step::Moved;
  }
  if (!loop) return Step::PastEnd;
  focus_ = 0;
  return Step::Wrapped;
}

WordLensNavigator::Step WordLensNavigator::prev(const bool loop) {
  if (count_ == 0) return Step::NoTargets;
  if (focus_ < 0) {
    focus_ = static_cast<int>(count_) - 1;
    return Step::Moved;
  }
  if (focus_ > 0) {
    focus_--;
    return Step::Moved;
  }
  if (!loop) return Step::PastStart;
  focus_ = static_cast<int>(count_) - 1;
  return Step::Wrapped;
}

bool WordLensNavigator::focusAt(const int x, const int y, const int stripHeight) {
  for (size_t i = 0; i < count_; i++) {
    const NavTarget& t = targets_[i];
    // The gloss may be wider than its word: give the touch target a little slack sideways.
    const int slack = t.height / 2;
    if (x >= t.x - slack && x < t.x + t.width + slack && y >= t.y - stripHeight && y < t.y + t.height) {
      focus_ = static_cast<int>(i);
      return true;
    }
  }
  return false;
}

void WordLensNavigator::setTargetsForTest(const NavTarget* targets, const size_t count) {
  clear();
  count_ = count < MAX_TARGETS ? count : MAX_TARGETS;
  std::memcpy(targets_, targets, count_ * sizeof(NavTarget));
}

}  // namespace lingua::wordlens
