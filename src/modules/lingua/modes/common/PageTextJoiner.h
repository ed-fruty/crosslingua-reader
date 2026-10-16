#pragma once

#include <Epub/blocks/TextBlock.h>

#include <cstdint>
#include <string>

#include "modules/lingua/utils/SentencePairing.h"

namespace lingua {

// Rebuilds the text a page's words were laid out from, for matching a page against the chapter's
// HTML. A page stores words, not the gaps between them, and several layout steps split ONE source
// word into several page words: an inline tag closing mid-word, an explicit hyphen or dash (upstream
// breaks "well-known" into "well-" + "known"), and line-end hyphenation ("Quadrat-" + "kilometer",
// with a hyphen the source never had). Joining such pieces with a space puts "well- known" on the page
// side, which the needle builder reads as a hyphenation artifact and cuts the needle there.
//
// The words' source ranges say exactly which ones touched in the source: a piece glued to the
// previous one starts where that one ended. Glued pieces join with no space, and an inserted
// (discretionary) hyphen is dropped from the join. Words without a source range (UINT32_MAX) join
// with a space, the old behaviour. Joining is in the TextBlock's visual order, so an RTL line keeps
// its spaces as before.
class PageTextJoiner {
 public:
  void reset() {
    prevEnd = UINT32_MAX;
    prevDiscretionaryHyphen = false;
  }

  // How word i continues the word last passed to this joiner (see WordGlue), then remembers word i.
  WordGlue next(const TextBlock& block, const uint16_t i) {
    const TextBlock::SourceRange range = block.wordSourceRange(i);
    WordGlue glue = WordGlue::Separate;
    if (prevEnd != UINT32_MAX && range.start != UINT32_MAX && range.start == prevEnd) {
      glue = prevDiscretionaryHyphen ? WordGlue::GluedDropHyphen : WordGlue::Glued;
    }
    prevEnd = range.start == UINT32_MAX ? UINT32_MAX : range.end;
    prevDiscretionaryHyphen = block.wordHasDiscretionaryHyphen(i);
    return glue;
  }

  // Appends word i to `text` and returns how it continued the previous word.
  WordGlue append(std::string& text, const TextBlock& block, const uint16_t i) {
    const WordGlue glue = next(block, i);
    if (!text.empty() && glue == WordGlue::GluedDropHyphen && text.back() == '-') {
      text.pop_back();
    } else if (!text.empty() && glue == WordGlue::Separate) {
      text += ' ';
    }
    text += block.wordText(i);
    return glue;
  }

 private:
  uint32_t prevEnd = UINT32_MAX;
  bool prevDiscretionaryHyphen = false;
};

}  // namespace lingua
