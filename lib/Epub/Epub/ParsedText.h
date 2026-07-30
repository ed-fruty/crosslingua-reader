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
 public:
  // Where a PRE-layout token index ended up once the block was broken into lines. Filled 1:1 with
  // setTrackedWords, in the same order, so a caller can map "the word that opens sentence k" to a
  // line and an x without re-deriving anything layout already knows exactly.
  //
  // The block is laid out with NO knowledge of the tracked indices — they constrain nothing. This is
  // a pure REPORT, which is what lets LinguaLayout::Interlinear break its source exactly as
  // LinguaLayout::OriginalOnly would and still know where each sentence landed.
  struct TrackedWordPos {
    static constexpr uint16_t NOT_PLACED = 0xFFFF;
    // Ordinal among the lines actually EMITTED. A line dropped to a TextBlock arena OOM does not
    // count, so this indexes the caller's own line vector directly, with no catch-up arithmetic.
    uint16_t line = NOT_PLACED;
    // x within the block's measure at which that token was laid out — the real laid-out value, so it
    // already carries the first-line indent, the block's alignment and any justification stretch of
    // the words before it on the line.
    int16_t x = 0;
    // The token is the FIRST LOGICAL word of that line. This single bit is what tells a caller
    // whether the previous tracked span ended on the line before or shares this one with it.
    bool startsLine = false;
  };

 private:
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
  // Word indices (in the CURRENT layout call's PRE-layout index space) whose final resting place the
  // caller wants reported back. Ascending, at most INTERLINEAR_MAX_ANNOTATIONS entries. Empty for
  // every layout but LinguaLayout::Interlinear, and every path below short-circuits on empty, so an
  // untouched block breaks and costs byte-for-byte what it did before this member existed.
  std::vector<uint16_t> trackedWords;
  // Destination for those reports, borrowed for the duration of one layoutAndExtractLines call and
  // null at every other moment. A member rather than a parameter chain because only extractLine
  // knows the x, and it is four frames down.
  std::vector<TrackedWordPos>* trackedOut = nullptr;

  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  // Everything that must be settled before the first word is measured: the paragraph's base
  // direction, whether its alignment is the natural one for that direction, and (for an SD-card
  // face) the glyph advances this block needs. Idempotent, so the per-line entry point can repeat
  // it on every call without changing what a whole-block layout would have decided.
  void prepareForLayout(const GfxRenderer& renderer, int fontId);
  // Drop the first `consumed` tokens from every parallel vector once their line is out.
  void consumeWords(size_t consumed);
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  // Returns true when a line was actually handed to processLine; false when the line was DROPPED
  // (TextBlock arena OOM). `emittedOrdinal` is the index this line will occupy among the emitted
  // ones, and is what a tracked-word report records — which is why the caller must only advance it
  // on a true return.
  bool extractLine(size_t breakIndex, size_t emittedOrdinal, int pageWidth, const std::vector<uint16_t>& wordWidths,
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
  // (LinguaLayout::Interlinear re-emitting one sentence span into an annotation row). A caller that
  // under-estimates is still correct — the vectors simply fall back to doubling.
  void reserveAdditionalWords(size_t additionalTokens);
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  // Read-only access to a word still in LOGICAL order, i.e. before layoutAndExtractLines consumes
  // the block (which also hyphenates and BiDi-reorders). LinguaLayout::Interlinear reads the buffered
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
  // True when this token is the REGULAR TAIL of a focus-reading bold-prefix split ("k" of "Ok"), and
  // therefore NOT a word in its own right: extractLine concatenates it back into the preceding entry
  // before any line leaves this class, so the page words every other consumer reads never contain
  // one. A caller that walks the PRE-layout token stream as if it were words must merge on this flag
  // or it counts "O" "k" where the rest of the system counts "Ok" — which is a different sentence,
  // a different match key and a different junk verdict (LinguaLayout::Interlinear's sentence pairing).
  bool wordIsFocusSuffixAt(const size_t index) const {
    return index < wordIsFocusSuffix.size() && wordIsFocusSuffix[index];
  }
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
  // Ask layout to REPORT where each listed word ends up. It constrains nothing — the block breaks
  // exactly as it would with an empty list, which is the whole point: LinguaLayout::Interlinear needs
  // its source to flow like any other paragraph and still needs to know which line each sentence
  // starts on and at what x.
  //
  // Indices are into the word stream as it stands right now, i.e. before layoutAndExtractLines
  // hyphenates and consumes it; the list is re-based internally whenever hyphenation inserts a
  // remainder word, so it keeps pointing at the same tokens. Must be ascending (extractLine binary
  // searches it per line).
  //
  // Scoped to ONE layout call: a caller that lays a block out in parts (includeLastLine = false)
  // must set the list again for the next part, since the consumed words are erased and the surviving
  // indices shift down. No caller does that today.
  void setTrackedWords(std::vector<uint16_t> wordIndices) { trackedWords = std::move(wordIndices); }
  // `trackedOutParam`, when non-null, is filled with ONE entry per index passed to setTrackedWords,
  // in the same order. An index whose line was dropped (TextBlock arena OOM), or that was never
  // reached because this was a partial layout, keeps TrackedWordPos::NOT_PLACED. The strict 1:1
  // correspondence is what lets renderInterlinear map annotation k to a line and an x, so nothing is
  // ever deduplicated or dropped from it.
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, std::vector<TrackedWordPos>* trackedOutParam = nullptr);
  // Lay out exactly ONE line at `width` and consume only that line's words, leaving the rest of the
  // block for a later call — at a DIFFERENT width if the caller wants one.
  //
  // layoutAndExtractLines wraps a whole block at a single measure, which LinguaLayout::Interlinear
  // cannot use: each annotation row is confined to the horizontal BAND its own source line leaves
  // free, and those bands differ row by row (the first is shortened by the x its source sentence
  // starts at, the last by the x the NEXT source sentence starts at). So the rows of one sentence
  // are wrapped at several different widths, one call each.
  //
  // The line is always treated as a FIRST line — resolveFirstLineIndent sees breakIndex 0 on every
  // call — so a caller that wants an indent on the opening row only must clear blockStyle.textIndent
  // itself before the second call. Interlinear sets it to zero throughout and positions each row by
  // placing its whole box, which is also what puts an RTL row's right edge on its band's right edge.
  //
  // Returns true when a line reached `processLine`, false when the block is empty or the line was
  // dropped (TextBlock arena OOM). The words are consumed either way, so a loop on size() always
  // terminates. Tracked words are NOT reported: the report is 1:1 with one whole-block layout.
  bool extractNextLine(const GfxRenderer& renderer, int fontId, uint16_t width,
                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
};
