#pragma once
#include <cstddef>
#include <cstdint>

class GfxRenderer;
class Page;

// Word Lens word navigation: the hard words of the page on screen, in reading order, one of them
// optionally focused. Pure logic over a laid-out Page and a hard-word predicate, so it is
// host-tested; the reader integration owns the input mapping, drawing and the lookup.
namespace lingua::wordlens {

// Pack entry of a word the layout marked hard (WordLensGlosser::lookupEntry): its rank and pool
// entry (lemma + full gloss). A plain lookup -- the hard/not-hard decision was the layout's.
using EntryFn = bool (*)(void* ctx, const char* word, size_t length, uint16_t& rank, uint32_t& entryOffset);

struct NavTarget {
  // The source word on the page: PageLine element index and word index in its TextBlock.
  uint16_t element = 0;
  uint16_t word = 0;
  // Its box in page coordinates (add the reader's margins for the screen), letters only.
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  uint16_t rank = 0;
  uint32_t entryOffset = 0;
  // False when the word's pack entry could not be found again (Confirm then has nothing to show).
  bool hasEntry = false;
  // The word as a lowercase lookup key (for the dictionary), empty when too long to keep.
  char surface[24] = {};
};

class WordLensNavigator {
 public:
  static constexpr size_t MAX_TARGETS = 64;
  enum class Step : uint8_t { Moved, Wrapped, PastEnd, PastStart, NoTargets };

  // Rebuild the targets for `page`: the words the Word Lens layout marked hard -- each annotation
  // strip carries an empty marker word at the x of every hard word of the body line below it,
  // glossed or not -- in page order. Clears the focus.
  void collect(const GfxRenderer& renderer, const Page& page, int bodyFontId, EntryFn entryFor, void* ctx);
  void clear();

  size_t count() const { return count_; }
  const NavTarget& target(const size_t i) const { return targets_[i]; }
  bool hasFocus() const { return focus_ >= 0; }
  int focus() const { return focus_; }
  const NavTarget* focused() const { return focus_ >= 0 ? &targets_[focus_] : nullptr; }
  void clearFocus() { focus_ = -1; }

  // With no focus, next() lands on the first target and prev() on the last. At an end, `loop`
  // wraps around; otherwise the focus stays and PastEnd / PastStart tells the caller to turn the
  // page.
  Step next(bool loop);
  Step prev(bool loop);

  // Focus the target whose word, or the gloss strip above it, contains (x, y) in page coordinates.
  // `stripHeight` is how far above the word its gloss reaches.
  bool focusAt(int x, int y, int stripHeight);

  // Test seam: install targets directly.
  void setTargetsForTest(const NavTarget* targets, size_t count);

 private:
  NavTarget targets_[MAX_TARGETS];
  size_t count_ = 0;
  int focus_ = -1;
};

// What Confirm on a focused word does.
enum class LookupAction : uint8_t { DictionarySurface, DictionaryLemma, GlossPopup };

// Dictionary first (the surface form, then the lemma when it differs), the full gloss otherwise.
constexpr LookupAction decideLookup(const bool dictionaryAvailable, const bool surfaceFound, const bool lemmaDiffers,
                                    const bool lemmaFound) {
  if (dictionaryAvailable && surfaceFound) return LookupAction::DictionarySurface;
  if (dictionaryAvailable && lemmaDiffers && lemmaFound) return LookupAction::DictionaryLemma;
  return LookupAction::GlossPopup;
}

}  // namespace lingua::wordlens
