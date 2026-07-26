#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // true = may break before token, but no synthetic space when joined
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  std::vector<std::string> rubyTexts;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<bool> reorderedFocusSuffixScratch;
  std::vector<uint16_t> visualOrderScratch;

  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  // The reader's STYLE-FREE first-line paragraph indent, in `fontId`'s own metrics: three space
  // widths when Extra Paragraph Spacing is OFF (the setting trades a gap for an indent), none when
  // it is ON. This is exactly the branch resolveFirstLineIndent() takes for a block that declares no
  // CSS text-indent, lifted out so text that carries NO CSS at all can indent by the same rule
  // instead of re-deriving it — the Page Translation overlay draws translated paragraphs that have
  // no stylesheet of their own, in a font that may differ from the body's.
  static int defaultFirstLineIndent(const GfxRenderer& renderer, int fontId, bool extraParagraphSpacing);

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  // Grow all five parallel token vectors (and rubyTexts, when it is in use) to hold `additionalTokens`
  // more entries in ONE step, instead of letting each double independently from zero. addWord uses it
  // on its multi-token paths; call it directly before any external push loop whose length is known
  // (PtLayout::Interlinear re-emitting one sentence span into an annotation row). A caller that
  // under-estimates is still correct — the vectors simply fall back to doubling.
  void reserveAdditionalWords(size_t additionalTokens);
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  // Read-only access to a word still in LOGICAL order, i.e. before layoutAndExtractLines consumes
  // the block (which also hyphenates and BiDi-reorders). PtLayout::Interlinear reads the buffered
  // TRANSLATION block this way: it is never laid out as a paragraph of its own, only re-emitted word
  // by word into the small annotation rows, so its words must be readable without laying it out.
  // Out-of-range yields the empty string rather than undefined behaviour.
  const std::string& wordAt(const size_t index) const {
    static const std::string kEmpty;
    return index < words.size() ? words[index] : kEmpty;
  }
  // Companion to wordAt, and mandatory for any caller that RE-EMITS a span of this block into
  // another ParsedText: the token stream is not one token per visual word. Focus reading splits every
  // word into a bold prefix plus a regular suffix, an NBSP / U+202F run splits mid-word, so does the
  // MAX_WORD_SIZE part-buffer overflow and an inline tag closing mid-word. Each of those boundaries
  // is marked here, and dropping it turns the boundary into a full getSpaceAdvance gap in
  // extractLine ("Bo njour ," instead of "Bonjour,"). Pass this straight into addWord's
  // attachToPrevious, as flushPartWordBuffer does for the parser's own re-emit.
  bool wordAttachesToPrevious(const size_t index) const { return index < wordContinues.size() && wordContinues[index]; }
  // True when at least one word added to this block starts with an RTL codepoint. Set as words arrive,
  // so it is final once the block is complete and readable before or after layout.
  //
  // A caller whose geometry depends on the laid-out words staying in LOGICAL order must test THIS and
  // not only blockStyle.isRtl: extractLine reorders a line whenever `isRtl || hasRtlWord`, while
  // layoutAndExtractLines only auto-resolves isRtl from the first RTL_PARAGRAPH_PROBE_WORDS words --
  // so an LTR paragraph carrying an inline Hebrew/Arabic run leaves isRtl false and still gets that
  // line BiDi-permuted into visual order.
  bool containsRtlWord() const { return hasRtlWord; }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};
