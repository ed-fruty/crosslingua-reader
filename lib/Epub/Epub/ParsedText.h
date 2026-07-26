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
  // Word indices (in the CURRENT layout call's PRE-layout index space) that must begin a line.
  // Ascending, at most INTERLINEAR_MAX_ANNOTATIONS entries. Empty for every layout but
  // PtLayout::Interlinear, and every path below short-circuits on empty, so an untouched block
  // breaks byte-for-byte as it did before this member existed.
  std::vector<uint16_t> forcedBreakBefore;

  // True when a line break before `idx` is both REQUESTED and ACHIEVABLE.
  //
  // Achievability is not a detail: computeLineBreaks refuses to end a line before a continuation
  // token (the `continuesVec[j + 1]` skip) and computeHyphenatedLineBreaks backtracks off one, so a
  // forced break on a continuation could never be honoured — bounding the DP by it would leave no
  // legal line at all and collapse the paragraph into the single-word fallback. Such an entry is
  // ignored here instead, and the sentence degrades to "annotation above the line that CONTAINS its
  // start", i.e. exactly the pre-v41 behaviour, for that one sentence.
  bool isForcedBreakAt(size_t idx) const;
  // Smallest achievable forced index strictly greater than `i`, or words.size() when there is none.
  // A line starting at `i` may not extend to or past it.
  size_t nextForcedBreakAfter(size_t i) const;
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
  // Constrain line breaking so each listed word STARTS a line. Indices are into the word stream as
  // it stands right now, i.e. before layoutAndExtractLines hyphenates and consumes it; the list is
  // re-based internally whenever hyphenation inserts a remainder word, so post-layout it still
  // points at the same tokens. Must be ascending. Only PtLayout::Interlinear calls this, to make a
  // source sentence never begin mid-line so its translation row can sit squarely above it.
  //
  // Scoped to ONE layout call: a caller that lays a block out in parts (includeLastLine = false)
  // must set the list again for the next part, since the consumed words are erased and the surviving
  // indices shift down. No caller does that today.
  void setForcedLineBreaks(std::vector<uint16_t> wordIndices) { forcedBreakBefore = std::move(wordIndices); }
  // `forcedBreakLineOrdinals`, when non-null, is filled with ONE entry per index passed to
  // setForcedLineBreaks, in the same order: the 0-based ordinal of the emitted line that index
  // landed on. For an achievable break that is the line it STARTS; for an ignored one (see
  // isForcedBreakAt) the line that merely contains it. The strict 1:1 correspondence is what lets
  // renderInterlinear map annotation k to a line without re-deriving word offsets, so nothing is
  // ever deduplicated or dropped from it.
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, std::vector<uint16_t>* forcedBreakLineOrdinals = nullptr);
};
