#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "WordStore.h"
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
  // Word text lives in wordStore (chunked bump arena, NUL-terminated entries);
  // words holds 8-byte handles into it. This replaces the former
  // std::deque<std::string>: per-word string objects, their SSO spills, and
  // every hyphenation/NFC temporary were the layout path's dominant
  // small-allocation churn, and any failed implicit allocation abort()s under
  // -fno-exceptions. Handles stay in a std::deque for the #2814 reason: no
  // large contiguous reallocation at CJK token counts (deque grows in fixed
  // ~512 B nodes). On arena OOM the word is dropped and hadDroppedWords()
  // latches so the section build can fail readably instead of aborting.
  // rubyTexts stays a deque of strings: ruby is rare and per-block small.
  // The per-token parallel arrays below stay vectors: 1 byte / 1 bit each,
  // they never approach the contiguous-block ceiling.
  WordStore wordStore;
  std::deque<WordStore::StoredWord> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   continues=false, noSpace=false: ordinary breakable word gap
  //   continues=false, noSpace=true:  breakable zero-width, stretchable CJK/Korean gap
  //   continues=true,  noSpace=false: unbreakable attachment
  //   continues=true,  noSpace=true:  breakable zero-width, non-stretching attachment
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Focus Reading emphasis: bytes [0, wordFocusBoundary) render bold, the rest at wordStyles.
  // 0 = none. An annotation rather than a token split, so the hyphenator and line breaker still
  // see whole words; TextBlock stores emphasis the same way, so extractLine passes it through.
  std::vector<uint8_t> wordFocusBoundary;
  // Internal-link identity through tokenization, hyphenation and BiDi reorder.
  // Zero means plain text; non-zero indexes linkTargets. Kept at one byte per
  // token and discarded after layout, never added to the page-cache TextBlock.
  std::vector<uint8_t> wordLinkIds;
  // Words sized by an inline font-size span: a slot per word into sizeSlotScales (absolute
  // multiples of the reader's body size). Slot 0 follows the block's own size; the parser
  // resolves each slot's font before layout.
  std::vector<uint8_t> wordSizeSlots;
  static constexpr uint8_t MAX_SIZE_SLOTS = TextBlock::MAX_WORD_FONTS + 1;
  float sizeSlotScales[MAX_SIZE_SLOTS] = {1.0f};
  int32_t sizeSlotFontIds[MAX_SIZE_SLOTS] = {};
  uint8_t sizeSlotCount = 1;
  int wordFontId(size_t wordIndex, int blockFontId) const;
  std::vector<std::string> linkTargets;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks retain absolute ranges for portable clipping anchors.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  // Sparse NFC source positions, in small nothrow chunks rather than a large contiguous array.
  struct AbsorbedSourceChunk {
    std::unique_ptr<AbsorbedSourceChunk> next;
    uint32_t base = 0;
    uint16_t deltas[64] = {};
    uint8_t begin = 0;
    uint8_t count = 0;
    ~AbsorbedSourceChunk() {
      while (next) {
        auto retired = std::move(next);
        next = std::move(retired->next);
      }
    }
  };
  std::unique_ptr<AbsorbedSourceChunk> absorbedSourceHead;
  AbsorbedSourceChunk* absorbedSourceTail = nullptr;
  mutable const AbsorbedSourceChunk* absorbedSourceCursor = nullptr;
  mutable uint32_t absorbedSourceCursorStart = 0;
  std::deque<std::string> rubyTexts;
  BlockStyle blockStyle;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphIndentSpaces;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  bool droppedWords = false;
  int linesAfterCurrent = -1;
  bool firstLineConsumed = false;
  // Separate from firstLineConsumed: a <br> line suppresses the indent but still starts a
  // new clipping paragraph.
  bool paragraphStartEmitted = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  std::string_view wordAt(const size_t i) const { return wordStore.view(words[i]); }
  bool storeWord(std::string_view text, WordStore::StoredWord& out);
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  uint32_t sourceOffsetAfter(uint32_t start, uint32_t renderedLength) const;
  bool recordAbsorbedSourceOffset(uint32_t offset);
  void retireAbsorbedSourceOffsets(uint32_t remainingStart);
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
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
                   const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const uint8_t paragraphIndentSpaces = 2)
      : blockStyle(blockStyle),
        paragraphIndentSpaces(paragraphIndentSpaces),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0, uint8_t sizeSlot = 0, bool noSpaceBefore = false);
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  // The word's renderer style: stored flags removed, small caps restored from its size slot.
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    if (index >= wordStyles.size()) return EpdFontFamily::REGULAR;
    const bool smallCaps = index < wordSizeSlots.size() && (wordSizeSlots[index] & TextBlock::SMALL_CAPS_SLOT_FLAG);
    return static_cast<EpdFontFamily::Style>((wordStyles[index] & ~TextBlock::DISCRETIONARY_HYPHEN_FLAG) |
                                             (smallCaps ? EpdFontFamily::SMALL_CAPS : 0));
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void suppressFirstLineIndent() { firstLineConsumed = true; }
  void resetFirstLineIndent() { firstLineConsumed = paragraphStartEmitted = false; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // True once any word was dropped because the text arena could not allocate.
  // Callers must treat the block as incomplete and fail the section build.
  bool hadDroppedWords() const { return droppedWords; }
  // While a line is being handed to layoutAndExtractLines' callback: how many lines of the
  // paragraph follow it, or -1 when the rest of the paragraph is not laid out yet.
  int linesAfterCurrentLine() const { return linesAfterCurrent; }
  // Slot for words at an absolute size; reuses a matching slot, or the nearest once all are taken.
  uint8_t sizeSlotFor(float scale);
  uint8_t sizeSlotsInUse() const { return sizeSlotCount; }
  float sizeSlotScale(uint8_t slot) const { return sizeSlotScales[slot]; }
  void setSizeSlotFontId(uint8_t slot, int32_t fontId) { sizeSlotFontIds[slot] = fontId; }
  // A set stop flag abandons extraction; the caller must discard this block.
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true, int8_t characterSpacing = 0, uint8_t wordSpacingPercent = 100,
                             size_t maxLines = SIZE_MAX, const bool* stopRequested = nullptr,
                             std::vector<TrackedWordPos>* trackedOutParam = nullptr);  // Lingua: see setTrackedWords

  // ── Lingua (CrossLingua fork) ───────────────────────────────────────────────────────────────────
  // Fork-only API, kept in one block so upstream changes to this class merge cleanly. Definitions
  // live in src/modules/lingua/layout/ParsedTextLingua.cpp unless they are inline here.

  // The reader's STYLE-FREE first-line paragraph indent, in `fontId`'s own metrics:
  // `paragraphIndentSpaces` space widths, scaled by the word-spacing setting; 0 means no indent.
  // This is exactly the branch resolveFirstLineIndent() takes for a block without a negative CSS
  // text-indent, lifted out so text that carries NO CSS at all can indent by the same rule instead
  // of re-deriving it — the Page Translation overlay draws translated paragraphs that have no
  // stylesheet of their own, in a font that may differ from the body's.
  static int defaultFirstLineIndent(const GfxRenderer& renderer, int fontId, uint8_t paragraphIndentSpaces,
                                    uint8_t wordSpacingPercent = 100);
  // Grow the parallel per-token vectors to hold `additionalTokens` more entries in ONE step, instead
  // of letting each double independently from zero. Call it before any external push loop whose
  // length is known (LinguaLayout::Interlinear re-emitting one sentence span into an annotation row).
  // A caller that under-estimates is still correct — the vectors simply fall back to doubling.
  void reserveAdditionalWords(size_t additionalTokens);
  // Read-only access to a word still in LOGICAL order, i.e. before layoutAndExtractLines consumes
  // the block (which also hyphenates and BiDi-reorders). LinguaLayout::Interlinear reads the buffered
  // TRANSLATION block this way: it is never laid out as a paragraph of its own, only re-emitted word
  // by word into the small annotation rows, so its words must be readable without laying it out.
  // Out-of-range yields an empty view rather than undefined behaviour. The view points into the
  // block's word arena and stays valid until the word is consumed by layout.
  std::string_view wordViewAt(const size_t index) const {
    return index < words.size() ? wordStore.view(words[index]) : std::string_view{};
  }
  // NUL-terminated form of wordViewAt, for C APIs such as getTextAdvanceX ("" when out of range).
  const char* wordCStrAt(const size_t index) const { return index < words.size() ? wordStore.cstr(words[index]) : ""; }
  // Companion to wordViewAt, and mandatory for any caller that RE-EMITS a span of this block into
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
    // Upstream now stores a focus split as a byte boundary inside one token,
    // rather than emitting a second suffix token. Lingua's pairing code keeps
    // this compatibility query, but there are no suffix tokens to collapse.
    (void)index;
    return false;
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
  //
  // layoutAndExtractLines' `trackedOutParam`, when non-null, is filled with ONE entry per index
  // passed here, in the same order. An index whose line was dropped (TextBlock arena OOM), or that
  // was never reached because this was a partial layout, keeps TrackedWordPos::NOT_PLACED. The
  // strict 1:1 correspondence is what lets renderInterlinear map annotation k to a line and an x,
  // so nothing is ever deduplicated or dropped from it.
  void setTrackedWords(std::vector<uint16_t> wordIndices) { trackedWords = std::move(wordIndices); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::unique_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, int8_t characterSpacing = 0, uint8_t wordSpacingPercent = 100,
                             std::vector<TrackedWordPos>* trackedOutParam = nullptr) {
    layoutAndExtractLines(
        renderer, fontId, viewportWidth,
        [&processLine](std::unique_ptr<TextBlock> line, uint32_t) { processLine(std::move(line)); }, includeLastLine,
        characterSpacing, wordSpacingPercent, SIZE_MAX, nullptr, trackedOutParam);
  }
  // Lay out exactly ONE line at `width` and consume only that line's words, leaving the rest of the
  // block for a later call — at a DIFFERENT width if the caller wants one.
  //
  // layoutAndExtractLines wraps a whole block at a single measure, which LinguaLayout::Interlinear
  // cannot use: each annotation row is confined to the horizontal BAND its own source line leaves
  // free, and those bands differ row by row (the first is shortened by the x its source sentence
  // starts at, the last by the x the NEXT source sentence starts at). So the rows of one sentence
  // are wrapped at several different widths, one call each.
  //
  // Only the block's first line can take a first-line indent: once any line has been consumed, later
  // calls lay out continuation lines. Interlinear builds its annotation stream with
  // paragraphIndentSpaces = 0, so no row is indented and each row is positioned by placing its whole
  // box, which is also what puts an RTL row's right edge on its band's right edge.
  //
  // Returns true when a line reached `processLine`, false when the block is empty or the line was
  // dropped (TextBlock arena OOM). The words are consumed either way, so a loop on size() always
  // terminates. Tracked words are NOT reported: the report is 1:1 with one whole-block layout.
  bool extractNextLine(const GfxRenderer& renderer, int fontId, uint16_t width,
                       const std::function<void(std::unique_ptr<TextBlock>)>& processLine, int8_t characterSpacing = 0,
                       uint8_t wordSpacingPercent = 100);
  // Every token the next addWord() calls push is translated text (a translated block, or an inline
  // <span lang> inside an original one) while this is set. The parser sets it around its addWord
  // and clears it straight after, so no other caller (list markers, Interlinear's annotation
  // re-emit, the Side by Side marker) ever inherits it.
  //
  // `inserted`: the words are not in the original text at all (a sidecar's inserted translation,
  // whose characters the parser does not count), whatever their language. Their lines carry no
  // source range for them, so clipping never anchors a position to text the original lacks.
  void setLinguaTranslatedInput(const bool translated, const bool inserted = false) {
    linguaTranslatedInput = translated;
    linguaInsertedInput = inserted;
    linguaDropInput = false;
  }
  // addWord() ignores words until the next setLinguaTranslatedInput(): the parser's way to drop a
  // marker it adds for a block whose text the active layout filters out.
  void setLinguaDropInput() { linguaDropInput = true; }
  // Translated flag of a token still in the pre-layout stream ("false" when out of range).
  bool linguaWordTranslatedAt(const size_t index) const {
    return index < linguaTranslated.size() && linguaTranslated[index];
  }

 private:
  // Word indices (in the CURRENT layout call's PRE-layout index space) whose final resting place the
  // caller wants reported back. Ascending, at most INTERLINEAR_MAX_ANNOTATIONS entries. Empty for
  // every layout but LinguaLayout::Interlinear, and every path below short-circuits on empty, so an
  // untouched block breaks and costs byte-for-byte what it did before this member existed.
  std::vector<uint16_t> trackedWords;
  // Destination for those reports, borrowed for the duration of one layoutAndExtractLines call and
  // null at every other moment. A member rather than a parameter chain because only extractLine
  // knows the x, and it is four frames down.
  std::vector<TrackedWordPos>* trackedOut = nullptr;
  // Everything that must be settled before the first word is measured: the paragraph's base
  // direction, whether its alignment is the natural one for that direction, and (for an SD-card
  // face) the glyph advances this block needs. Idempotent, so the per-line entry point can repeat
  // it on every call without changing what a whole-block layout would have decided.
  void prepareForLayout(const GfxRenderer& renderer, int fontId);
  // Drop the first `consumed` tokens from every parallel vector once their line is out.
  void consumeWords(size_t consumed);
  // Per-token translated flag, parallel to `words`. The upstream style byte has no free bit, so the
  // flag lives here. EMPTY until the block's first translated token, so a block without one -- every
  // block of a book without translations -- pays nothing; once non-empty it is exactly words.size().
  std::vector<bool> linguaTranslated;
  bool linguaTranslatedInput = false;
  // Per-token "inserted by the sidecar rewriter" flag, kept exactly like linguaTranslated (empty
  // until the first such token).
  std::vector<bool> linguaInserted;
  bool linguaInsertedInput = false;
  bool linguaDropInput = false;
  // Hooks that keep linguaTranslated / linguaInserted in lockstep with the upstream per-token arrays.
  void linguaOnTokenPushed();
  void linguaOnWordSplit(size_t wordIndex);
  void linguaErasePrefix(size_t consumed);
  // Run on every line extractLine actually emits: report its tracked words and stamp the
  // TextBlock's translated-word flags (visual order, like every other TextBlock array).
  void finishLinguaLine(TextBlock& block, size_t lastBreakAt, size_t lineBreak, size_t emittedOrdinal, bool willReorder,
                        const std::vector<int16_t>& lineXPos);
};
