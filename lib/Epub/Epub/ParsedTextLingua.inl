// CrossLingua fork: the ParsedText members that upstream ParsedText.cpp itself calls through its
// one-line `// Lingua` hooks. Included once, at the end of ParsedText.cpp, so every target that
// compiles ParsedText.cpp (firmware and the upstream host tests alike) links them without knowing
// about src/modules/lingua. Everything else of the fork's ParsedText API lives in
// src/modules/lingua/layout/ParsedTextLingua.cpp.
//
// Not a standalone translation unit: no include guard, no includes of its own.

void ParsedText::linguaOnTokenPushed() {
  // Called right after words.push_back, so the new token is words.back().
  if (linguaTranslated.empty()) {
    if (!linguaTranslatedInput) return;
    // First translated token of the block: materialize the flags for the tokens before it.
    linguaTranslated.reserve(wordStyles.capacity());
    linguaTranslated.resize(words.size() - 1, false);
  } else if (linguaTranslated.capacity() < wordStyles.capacity()) {
    // Follow the upstream arrays' bulk reserve instead of growing on our own schedule.
    linguaTranslated.reserve(wordStyles.capacity());
  }
  linguaTranslated.push_back(linguaTranslatedInput);
}

void ParsedText::linguaOnWordSplit(const size_t wordIndex) {
  // Re-base the tracked-word list over the token that was just inserted at wordIndex + 1. This is
  // the ONLY place either line breaker adds a word -- the DP's oversized-word pre-pass and the greedy
  // loop both come through here -- so one shift keeps Interlinear's sentence starts pointing at the
  // right tokens for the rest of the layout. Adding 1 to a suffix of an ascending list leaves it
  // ascending, so the per-line binary search stays valid. `> wordIndex` and not `>=`: a sentence that
  // STARTS at the word being split still starts at its prefix.
  for (auto& tracked : trackedWords) {
    if (tracked > wordIndex) {
      ++tracked;
    }
  }
  // The remainder is the same word, so it keeps the prefix's translated flag.
  if (!linguaTranslated.empty() && wordIndex < linguaTranslated.size()) {
    const bool translated = linguaTranslated[wordIndex];
    linguaTranslated.insert(linguaTranslated.begin() + wordIndex + 1, translated);
  }
}

void ParsedText::linguaErasePrefix(const size_t consumed) {
  if (linguaTranslated.empty()) return;
  const size_t take = std::min(consumed, linguaTranslated.size());
  linguaTranslated.erase(linguaTranslated.begin(), linguaTranslated.begin() + take);
}

void ParsedText::finishLinguaLine(TextBlock& block, const size_t lastBreakAt, const size_t lineBreak,
                                  const size_t emittedOrdinal, const bool willReorder,
                                  const std::vector<int16_t>& lineXPos) {
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const auto logicalAt = [&](const size_t visual) {
    return lastBreakAt + (willReorder ? static_cast<size_t>(visualOrderScratch[visual]) : visual);
  };

  // TRANSLATED WORDS, in the TextBlock's visual order. none / all cost nothing; only a line mixing
  // the two (an inline <span lang> run inside an original paragraph) carries a bitmask.
  if (!linguaTranslated.empty() && lineWordCount > 0 && lineWordCount == block.wordCount()) {
    size_t translatedCount = 0;
    for (size_t i = 0; i < lineWordCount; ++i) {
      if (linguaWordTranslatedAt(logicalAt(i))) ++translatedCount;
    }
    LinguaWordFlags& flags = block.linguaTranslatedWords();
    if (translatedCount == lineWordCount) {
      flags.setAll();
    } else if (translatedCount > 0 && flags.beginMask(static_cast<uint16_t>(lineWordCount))) {
      for (size_t i = 0; i < lineWordCount; ++i) {
        if (linguaWordTranslatedAt(logicalAt(i))) flags.setBit(static_cast<uint16_t>(i));
      }
    }
  }

  // TRACKED WORDS. Report every tracked index that fell on THIS line, reading the x straight out of
  // the table the positioning loops just built -- the only moment in the whole pipeline where a token
  // index and its final x are both in hand. Called only after the line's TextBlock is known valid, so
  // a dropped line neither consumes an ordinal nor claims a tracked word.
  //
  // Empty in every layout but LinguaLayout::Interlinear, and the null test is the first thing checked,
  // so this costs one pointer compare per line for everyone else.
  if (trackedOut == nullptr || trackedWords.empty()) return;
  // Heterogeneous compare (uint16_t elements against a size_t probe) so a paragraph longer than
  // 65535 tokens cannot wrap the probe around and match the wrong entry.
  auto it = std::lower_bound(trackedWords.begin(), trackedWords.end(), lastBreakAt,
                             [](const uint16_t element, const size_t probe) { return element < probe; });
  for (; it != trackedWords.end() && static_cast<size_t>(*it) < lineBreak; ++it) {
    const size_t logical = static_cast<size_t>(*it) - lastBreakAt;
    // The x table is in VISUAL order, so an RTL line needs the inverse permutation. Dead code under
    // Interlinear (renderInterlinear refuses to annotate a block that reorders) but present so the
    // API is not a trap for the next caller.
    size_t slot = logical;
    if (willReorder) {
      slot = visualOrderScratch.size();
      for (size_t v = 0; v < visualOrderScratch.size(); ++v) {
        if (visualOrderScratch[v] == logical) {
          slot = v;
          break;
        }
      }
    }
    if (slot >= lineXPos.size()) continue;
    TrackedWordPos& out = (*trackedOut)[static_cast<size_t>(it - trackedWords.begin())];
    out.line = static_cast<uint16_t>(emittedOrdinal);
    out.x = lineXPos[slot];
    out.startsLine = logical == 0;
  }
}
