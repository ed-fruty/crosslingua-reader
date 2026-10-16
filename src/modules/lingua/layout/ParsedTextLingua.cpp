// CrossLingua fork: ParsedText members used only by the Lingua layouts, kept out of the upstream
// ParsedText.cpp so upstream changes there merge cleanly. Declared in ParsedText.h's Lingua block.
#include <Epub/ParsedText.h>
#include <GfxRenderer.h>

#include <algorithm>

int ParsedText::defaultFirstLineIndent(const GfxRenderer& renderer, const int fontId,
                                       const uint8_t paragraphIndentSpaces, const uint8_t wordSpacingPercent) {
  if (paragraphIndentSpaces == 0) return 0;
  // Same arithmetic as resolveFirstLineIndent()'s default branch (scaleSpace in ParsedText.cpp).
  const int space = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int scaled = wordSpacingPercent == 100 ? space : (space * wordSpacingPercent + 50) / 100;
  return scaled * paragraphIndentSpaces;
}

// Only the std::vector arrays are reserved: words and rubyTexts are std::deque (chunked growth, no
// reserve()/capacity() and no large contiguous reallocation to avoid). wordStyles' capacity gauges
// them all since every push keeps the arrays in lockstep.
void ParsedText::reserveAdditionalWords(const size_t additionalTokens) {
  if (additionalTokens == 0) return;
  const size_t requiredSize = words.size() + additionalTokens;
  if (wordStyles.capacity() >= requiredSize) return;

  size_t newCapacity = wordStyles.capacity();
  if (newCapacity < 16) {
    newCapacity = 16;
  }
  while (newCapacity < requiredSize) {
    newCapacity *= 2;
  }

  wordStyles.reserve(newCapacity);
  wordContinues.reserve(newCapacity);
  wordNoSpaceBefore.reserve(newCapacity);
  wordFocusBoundary.reserve(newCapacity);
  wordLinkIds.reserve(newCapacity);
  wordVisibleOffsetDeltas.reserve(newCapacity);
}

void ParsedText::consumeWords(const size_t consumed) {
  if (consumed == 0) return;
  const size_t take = std::min(consumed, words.size());
  // Lines after this one are continuations: no first-line indent.
  firstLineConsumed = true;
  for (size_t i = 0; i < take; ++i) {
    wordStore.release(words[i]);  // retires arena chunks as lines are consumed
  }
  words.erase(words.begin(), words.begin() + take);
  wordStyles.erase(wordStyles.begin(), wordStyles.begin() + take);
  wordContinues.erase(wordContinues.begin(), wordContinues.begin() + take);
  wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + take);
  wordFocusBoundary.erase(wordFocusBoundary.begin(), wordFocusBoundary.begin() + take);
  wordLinkIds.erase(wordLinkIds.begin(), wordLinkIds.begin() + take);
  linguaErasePrefix(take);
  eraseVisibleOffsetPrefix(take);
  if (!rubyTexts.empty()) {
    const size_t rtConsumed = std::min(take, rubyTexts.size());
    rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
  }
}

bool ParsedText::extractNextLine(const GfxRenderer& renderer, const int fontId, const uint16_t width,
                                 const std::function<void(std::unique_ptr<TextBlock>)>& processLine,
                                 const int8_t characterSpacing, const uint8_t wordSpacingPercent) {
  if (words.empty()) return false;

  blockStyle.characterSpacing = characterSpacing;
  this->wordSpacingPercent = wordSpacingPercent;
  prepareForLayout(renderer, fontId);

  const int pageWidth = width;
  auto wordWidths = calculateWordWidths(renderer, fontId);
  // The whole remaining block is broken, and only the first line of that break is taken. Breaking
  // just the opening line is not separable from breaking the rest — the DP's cost for line 0 is
  // defined by what the lines after it can do with what is left — and a sentence's translation is a
  // few dozen tokens, so the repeated work is bounded and small.
  const std::vector<size_t> lineBreakIndices =
      hyphenationEnabled
          ? computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore)
          : computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  if (lineBreakIndices.empty()) return false;

  const bool emitted = extractLine(
      0, 0, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices,
      [&processLine](std::unique_ptr<TextBlock> line, uint32_t) { processLine(std::move(line)); }, renderer, fontId);
  consumeWords(lineBreakIndices[0]);
  return emitted;
}
