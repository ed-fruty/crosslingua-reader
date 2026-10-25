#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/PageFontSet.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"
#include "modules/lingua/layout/LinguaLayout.h"
#include "modules/lingua/modes/interlinear/InterlinearAnnotation.h"
#include "modules/lingua/modes/word_lens/WordLensGlossSource.h"

class Page;
class PageLine;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphIndentSpaces = 2;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Lingua feature: the page layout to produce. The parser never sees the user's display
  // mode -- CrossPointSettings::linguaLayoutForDisplayMode() collapses the modes onto these layouts.
  LinguaLayout linguaLayout = LinguaLayout::Both;
  // Lingua: font the TRANSLATED text is laid out in, or 0 for "same as fontId" (the ONLY
  // unset sentinel — negative ids are normal, see PageFontSet.h). A non-zero id makes translated
  // lines a second type size on the page: they are measured, broken and advanced with it, and
  // stamped LineFontRole::Translation so the renderer resolves the same id back out of the page's
  // PageFontSet. 0 keeps every line Body, which is byte-for-byte the pre-existing layout.
  int translationFontId = 0;
  // Lingua (LinguaLayout::Interlinear): font the small ANNOTATION rows are laid out in, or 0
  // for "same as fontId" (the same single sentinel as translationFontId). Rows are measured, broken
  // and advanced with it and stamped LineFontRole::Annotation, so the renderer resolves the same id
  // back out of the page's PageFontSet.
  int annotationFontId = 0;
  // Lingua (LinguaLayout::Interlinear): the app's sentence aligner (see
  // Lingua InterlinearAnnotation contract). nullptr means "no annotations": the source paragraph still lays
  // out, it just carries no rows.
  InterlinearPairFn interlinearPairFn = nullptr;
  // Reusable annotation buffer for the interlinear pass, sized once on the first annotated paragraph
  // (INTERLINEAR_MAX_ANNOTATIONS entries, 300 bytes) and reused for every paragraph after it -- no
  // per-paragraph allocation, and nothing allocated at all under any other layout.
  std::vector<InterlinearAnnotation> interlinearAnnotations;
  // Lingua (LinguaLayout::WordLens): the app's gloss decision, or nullptr for a chapter laid out
  // with no strips (no pack installed). Set by Section::startBuild through setWordLensSource().
  WordLensGlossSource* wordLensSource = nullptr;
  // Lingua: the paired and Word Lens layouts lay blocks out themselves, without the drop-cap pass of
  // layoutCurrentBlock, so they must not capture an initial letter out of the text.
  bool linguaAllowsDropCaps() const {
    return linguaLayout != LinguaLayout::SideBySide && linguaLayout != LinguaLayout::Interlinear &&
           !(linguaLayout == LinguaLayout::WordLens && wordLensSource);
  }
  // Lingua (LinguaLayout::WordLens): the hard words of the block being laid out -- the token each
  // starts at and its strip gloss, stored in wordLensGlossText. Reused block after block, so their
  // capacity is paid once per chapter. See makePagesWordLens.
  struct WordLensPending {
    uint16_t token;
    uint16_t tokenCount;
    uint16_t rank;
    uint32_t textOffset;
    uint8_t textLength;
  };
  std::vector<WordLensPending> wordLensPending;
  std::string wordLensGlossText;
  // Monotonic index over ORIGINAL content paragraphs. Advances once per content-bearing original
  // block regardless of nesting depth, matching the per-content-paragraph granularity of the
  // PageTranslationOverlay/TooltipOverlay reparsers (ParagraphBoundary.h SSOT). Empty/whitespace-only blocks
  // and translated blocks do NOT advance it.
  int16_t paragraphCounter = 0;
  // Paragraph index stamped on the currently-open block's laid-out lines; -1 before any block opens.
  int16_t currentBlockParagraphIdx = -1;
  // True once the current physical text block has been assigned a paragraph index. Reset to false
  // wherever a FRESH ParsedText is installed: startNewTextBlock, and the <table> open and close in
  // startElement / endElement, which install or drop the block directly. A wrapper container and the
  // child block that reuses its empty ParsedText (via the isEmpty / listItemBulletOnly reuse paths)
  // therefore share ONE index, and each distinct content block consumes exactly one.
  bool currentBlockIndexAssigned = false;
  // True while the current block is one a <br> opened and has no word yet (see classifyBrOpenedBlock).
  // Set after breakTextLine in startElement's <br> branch; cleared by its first word and at the same
  // fresh-block sites as currentBlockIndexAssigned.
  bool currentBlockOpenedByBr = false;
  // Lingua: words the active layout has dropped so far. Monotonic, so a border box can tell a block
  // whose words were dropped from one that is empty on purpose (see closeBoxScope).
  uint32_t linguaFilteredWords = 0;
  // Lingua: the current block dropped a word since its element opened, so a <br> in it is not a
  // blank line. Cleared where an element opens or a fresh block is installed.
  bool currentBlockDroppedWord = false;
  bool currentBlockIsTranslated = false;  // True when the currently-open block has lang= differing from bookPrimaryLang
  std::string bookPrimaryLang;            // Book's content.opf language; a differing lang= marks a translated block
  std::string translatedHyphenLang;       // Last lang= applied to the Hyphenator's translated slot; avoids re-resolving
  // A translated sidecar's inserted blocks (lingua::content::INSERTED_TRANSLATION_ATTR) are not
  // counted in visibleTextOffset or the XPath paragraph / list-item indices, and their words carry no
  // source range, so every position means the same as in the chapter's own XHTML.
  void linguaOnElementStart(const char* name, const char** atts);
  // Depth the open inserted block started at, or -1. A word flushed by the block's own closing tag
  // still sees depth > this (endElement flushes before it decrements depth).
  int linguaInsertedDepth = -1;
  bool linguaTranslatedSidecar = false;  // parsing a `.translated.html` sidecar (Section sets it)
  bool linguaInsideInsertedTranslation() const { return linguaInsertedDepth >= 0 && depth > linguaInsertedDepth; }
  // Before a block's list marker is added: the marker of a translated item is translated text, and
  // dropped with it by a layout that drops translated words (no orphan "•" line).
  void linguaPrepareBlockMarker(const char* name);

  // Lingua (SideBySide and Interlinear): the current ORIGINAL outermost block is buffered
  // here until its paired translation arrives, because the parser flushes the original long before
  // the <p lang="..."> translation block is read and both layouts need the two texts in hand at once
  // (see renderSideBySide / renderInterlinear). bufferedOriginalParagraphIdx carries the original's
  // paragraph index across the buffering window — the block-level currentBlockParagraphIdx has advanced to
  // the next block by the time the pair is emitted, so the emitted PageLines would otherwise stamp
  // the wrong index for the Page Translation overlay's line->paragraph mapping.
  std::unique_ptr<ParsedText> bufferedOriginalBlock = nullptr;
  int16_t bufferedOriginalParagraphIdx = -1;
  // ...and so does the buffered original's FOOTNOTE LEDGER, for the same reason its paragraph index
  // does: a pending footnote's wordIndex is relative to the words of the block it was parsed in, and
  // startNewTextBlock zeroes that base the moment the buffered block is replaced by the next one. Left
  // in the shared pendingFootnotes, two consecutive unpaired originals both index from base 0, so
  // laying the FIRST one out drained the SECOND one's low-index entries onto the first one's pages and
  // the end-of-block net then cleared whatever was left — footnote markers on the wrong page, or gone.
  // Moving the ledger into the buffer with the block makes an entry unambiguous about which block it
  // belongs to by construction: a drain can only ever see the ledger that is installed
  // (adoptBufferedFootnoteLedger), so it can only ever match its own block.
  std::vector<std::pair<int, FootnoteEntry>> bufferedOriginalFootnotes;
  int bufferedOriginalWordsExtracted = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasTextAlign = false;
    CssTextAlign textAlign = CssTextAlign::Left;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    // Lingua: true when the enclosing block/inline element has a lang= attribute
    // differing from the book's primary language (propagated to children through nesting).
    bool isTranslatedBlock = false;
    bool hasSmallCaps = false, smallCaps = false;
    bool hasWhiteSpace = false, preserveWhitespace = false;
    // Inline font size: a multiple of the parent size, or of the body size when rem.
    bool hasFontScale = false, fontScaleRem = false;
    float fontScale = 1.0f;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  // Open elements by depth, for descendant/child CSS selectors. Deeper elements
  // resolve without ancestors, so contextual rules simply don't match there.
  static constexpr size_t MAX_CSS_ANCESTORS = 16;
  std::array<CssAncestor, MAX_CSS_ANCESTORS> cssAncestors{};
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveTextAlignDefined = false;
  CssTextAlign effectiveTextAlign = CssTextAlign::Left;
  bool effectiveSup = false;
  bool effectiveSub = false;
  bool effectiveSmallCaps = false;
  bool effectivePreserveWhitespace = false;
  bool pendingPageBreak = false;  // set when a page-break-after element closes

  // Initial letter captured from a ::first-letter block or a leading float/initial-letter span,
  // drawn as a PageDropCap when its block is laid out.
  static constexpr size_t MAX_DROP_CAP_BYTES = 12;
  struct DropCapState {
    char text[MAX_DROP_CAP_BYTES + 1] = {};
    uint8_t length = 0;
    uint8_t codepoints = 0;
    uint8_t lines = 0;  // text lines the letter spans
    bool bold = false;
    bool firstLetterPending = false;  // capture the current block's first letter
    int spanDepth = -1;               // capturing the text of the span at this depth
  } dropCap;

  // Open elements with a CSS border or background shade. Each emits a PageBorderBox around
  // its content on every page it spans.
  struct BoxScope {
    int depth = 0;
    int16_t left = 0;
    int16_t right = 0;
    int16_t padTop = 0;  // outer edge to content, border included
    int16_t padBottom = 0;
    CssBorderSide sides[4];  // top, right, bottom, left
    bool shaded = false;
    int16_t top = -1;  // content extent on the current page
    int16_t bottom = -1;
    bool continued = false;             // sliced onto an earlier page
    uint32_t linguaFilteredAtOpen = 0;  // Lingua: linguaFilteredWords when the box opened
  };
  static constexpr size_t MAX_BOX_SCOPES = 8;

  // Page-break carry-over: a paragraph's orphan/widow lines and a heading kept with what
  // follows move to the next page instead of being left behind.
  static constexpr size_t MAX_CARRIED_LINES = 6;
  const ParsedText* layoutParagraph = nullptr;
  bool layoutParagraphHasDropCap = false;
  uint8_t paragraphLinesOnPage = 0;
  uint8_t keepWithNextLines = 0;                       // trailing heading lines on this page
  uint32_t recentLineOffsets[MAX_CARRIED_LINES] = {};  // visible offsets of this page's last lines, newest last
  uint8_t recentLineCount = 0;
  size_t linesToCarry(size_t& paragraphLines) const;
  size_t keepWithNextCarry() const;
  bool canCarry(size_t carry) const;
  void breakPageCarryingLines(size_t carry, size_t paragraphLines, uint32_t visibleOffset);
  std::vector<BoxScope> boxScopes;
  static constexpr size_t MAX_GRID_TABLE_COLUMNS = 4;
  // ponytail: buffer at most 160 tokens / 2 KB per row; larger rows use stacked flow.
  static constexpr size_t MAX_GRID_TABLE_ROW_WORDS = 160;
  static constexpr size_t MAX_GRID_TABLE_ROW_BYTES = 2048;
  int tableDepth = 0;
  bool insideTableCell = false;
  bool tableRowStacked = false;
  bool tableRowRtl = false;
  uint16_t tableRowsSpannedRemaining = 0;
  size_t tableRowTextBytes = 0;
  std::vector<std::unique_ptr<ParsedText>> tableRowCells;
  std::array<std::vector<std::unique_ptr<TextBlock>>, MAX_GRID_TABLE_COLUMNS> tableCellLines;
  std::vector<uint32_t> tableLineVisibleOffsets;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet
  CssBorderSide tableBorder;        // grid lines for the current table; invisible when unbordered
  // Column widths planned from a pre-scan of the table's markup; 0 columns = equal widths.
  std::array<uint16_t, MAX_GRID_TABLE_COLUMNS> tableColumnWidths{};
  uint8_t tableColumnCount = 0;
  struct TableColumnMeasure {
    uint16_t minWidth[MAX_GRID_TABLE_COLUMNS] = {};   // longest word
    uint16_t prefWidth[MAX_GRID_TABLE_COLUMNS] = {};  // longest unwrapped line
    uint8_t columns = 0;
  };

  // A font-size span that opens a block and holds all of its text sizes the whole block,
  // e.g. <p><span class="big">Chapter One</span></p>. Text after the span cancels it.
  struct InlineSizeState {
    float scale = 1.0f;
    int depth = -1;  // outermost sizing span
    bool valid = false;
    bool open = false;
  } inlineSize;
  // Lingua: the inline size of the original a pairing layout holds in bufferedOriginalBlock.
  InlineSizeState bufferedOriginalInlineSize;
  // Lingua: the role of the block layoutCurrentBlock is laying out. A member so its line callback
  // captures no more than std::function's inline buffer holds.
  LineFontRole layoutLineRole = LineFontRole::Body;

  // Tracks the innermost open <ul>/<ol> so <li> knows whether to number itself,
  // bullet itself, or (list-style-type: none) emit no marker at all. Pushed on
  // <ul>/<ol> open, popped on close, so nested lists restart their own counter
  // without disturbing the parent list's.
  struct ListContext {
    bool ordered = false;    // true for <ol>, false for <ul>
    bool styleNone = false;  // true when list-style-type: none is set on this list
    int counter = 0;         // incremented before each direct <li>; used as its number when ordered
    int depth = 0;           // parser depth at open time; matches the depth seen in endElement
                             // for the same tag, so a hidden nested list's close can't pop
                             // an outer list's context
  };
  std::vector<ListContext> listStack;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on. A deque grows in
  // small chunks, so a long chapter never needs one large free block (a vector doubling past 512
  // anchors needs ~28 KB contiguous, and a failed allocation aborts).
  int completedPageCount = 0;
  std::deque<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool insideBody = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  uint8_t currentFootnoteLinkId = 0;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  // The FOOTNOTE LEDGER of the block currently open / being laid out: the anchors it has pushed but
  // not yet delivered, and the running count of its words that have already been laid out. A
  // wordIndex is meaningful ONLY against the base of its own block (the push site adds
  // wordsExtractedInBlock to currentTextBlock->size(), and startNewTextBlock zeroes the base for
  // every fresh block), so a drain must never see two blocks' entries at once — that is exactly what
  // bufferedOriginalFootnotes below exists to prevent.
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;
  // Latched when a ParsedText could not be created (OOM). Together with
  // ParsedText::hadDroppedWords() this turns layout OOM into ParseStatus::Error
  // so the section build fails readably instead of emitting pages with holes.
  bool layoutOom = false;
  bool stopRequested = false;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle, bool paragraphEnd = true);
  void flushPendingAnchor();
  void completeCurrentPage();
  void emitCurrentPage();
  void noteContent(int top, int bottom);
  void openBoxScope(BlockStyle& ownStyle, const CssStyle& cssStyle);
  void closeBoxScope();
  void emitBoxSegment(const BoxScope& box, bool closing);
  void armFirstLetterDropCap(const char* tagName, const std::string& classAttr, const char* idAttr);
  bool captureDropCapCodepoint(const char* bytes, int length);
  void cancelDropCapToWord();
  void layoutCurrentBlock(bool includeLastLine);
  void layoutDropCapLines(int layoutFontId, uint16_t effectiveWidth,
                          const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& emitLine,
                          bool includeLastLine);
  static uint8_t dropCapLines(const CssStyle& style);
  bool measureTableColumns(HalFile& file, TableColumnMeasure& out) const;
  void planTableColumns(const TableColumnMeasure& measure);
  void applyPendingPageBreak();
  void pushBlockStyle(const BlockStyle& accumulated);
  void flushPartWordBuffer();
  // Lingua: true when the block currently being parsed is one the active linguaLayout
  // drops, so its words never reach the layout engine. Shared by flushPartWordBuffer (which drops
  // the word) and the <ruby>/<rt> handlers (which must not annotate words that were never added).
  bool wordIsFiltered() const;
  // Lingua: settle a <br>-opened block's translation state and paragraph index from its first
  // word. <br/> carries no lang= of its own, so a block it opens is unclassified until content
  // arrives -- which is where Calibre's plugin puts its translation. Defined in
  // modules/lingua/layout/LinguaTextFlow.cpp; a no-op on any block not opened by <br>.
  void classifyBrOpenedBlock();
  // Lingua: THE paragraph-index rule, in one place. A translated block pairs with the most recent
  // original (paragraphCounter - 1) and never advances the counter; an original block claims the
  // next index once -- currentBlockIndexAssigned latches so a wrapper container and the child that
  // reuses its still-empty ParsedText share ONE index instead of double-counting. Every context that
  // opens a content block routes through here: header open, container open, a <br>-opened block's
  // first word, and an image's alt-text fallback.
  void claimParagraphIndex(bool isTranslated);
  // Lingua: the role every line of the currently-open block carries. Reads the SAME
  // block-level translated signal as the per-block hyphenation slot and the word-drop filter
  // (currentBlockIsTranslated), so the parser keeps exactly one notion of "translated".
  LineFontRole currentLineRole() const;
  // The font id a role is MEASURED and ADVANCED with. Built through the same PageFontSet resolver
  // the renderer uses, with the same slots LinguaReaderIntegration::resolvePageFontSet() fills, so a line
  // is laid out with precisely the id it will later be drawn with — the role byte is all that
  // crosses the section cache.
  int fontIdForRole(const LineFontRole role) const {
    return PageFontSet(fontId, translationFontId, annotationFontId).forRole(role);
  }
  // Deliver every anchor still pending for the block just laid out to the page currently being built.
  // The per-line drains attribute an anchor to the page carrying it; this is the end-of-block net for
  // the entries they could not reach (a block that produced fewer lines than its anchors need), shared
  // by makePages and both custom emitters so all three behave identically.
  void flushPendingFootnotesToCurrentPage();
  // Lingua: one block's footnote ledger — the anchors pending for it and the word base those
  // indices are relative to. Both drains compare an index against wordsExtractedInBlock, so installing
  // a ledger is what makes a drain match its own block and nothing else.
  struct FootnoteLedger {
    std::vector<std::pair<int, FootnoteEntry>> pending;
    int wordBase = 0;
  };
  // Install the BUFFERED original's ledger for the duration of laying that block out, returning the
  // in-flight block's ledger for the caller to park. Required by every path that lays the buffered
  // block out (flushBufferedOriginal, renderSideBySide, renderInterlinear): by then the in-flight block
  // has usually pushed anchors of its own, indexed from a base of its own, and draining those against
  // the buffered block's lines is what put one paragraph's footnotes on another's page.
  [[nodiscard]] FootnoteLedger adoptBufferedFootnoteLedger();
  // Undo adoptBufferedFootnoteLedger: net anything the buffered block's layout could not place, then
  // restore the parked in-flight ledger. Leaves bufferedOriginalFootnotes empty (and its capacity
  // recycled), which is the precondition the next buffered block relies on.
  void releaseFootnoteLedger(FootnoteLedger& parked);
  // Lingua: finishParse's trailing flush -- makePages(), or under a pairing layout the pairing
  // builder plus the drain of a still-buffered original.
  void makePagesForTrailingBlock();
  // Lingua: startNewTextBlock's flush of the finished block -- makePages(), or under a pairing layout
  // its builder, which buffers originals and pairs them with their translations.
  void makePagesForFlushedBlock(bool paragraphEnd);
  // Lingua: stamp a main-flow PageLine with the open block's paragraph index and its font role, and
  // extend the page's paragraph range.
  void stampLinguaLine(PageLine& line, LineFontRole role);
  void breakTextLine(const BlockStyle& style);
  void fallbackTableRowToStacked();
  void closeTableCell();
  void finishTableRow();
  void addTableRowSeparator();
  void setCurrentPageVisibleOffset(uint32_t offset);
  void makePages(bool includeLastLine = true, bool paragraphEnd = true);
  void applyBlockFontScale(BlockStyle& blockStyle, const CssStyle& cssStyle, const char* tagName) const;
  int fontIdForScale(float scale) const;
  int prepareBlockFont(ParsedText& text, float scale);
  // Lingua (LinguaLayout::SideBySide): two-column table layout. makePagesTableMode routes an
  // outermost block to either buffering (an original, held in bufferedOriginalBlock) or pairing
  // (a translation, laid beside the buffered original). renderSideBySide lays a buffered original
  // and its paired translation into two half-width columns, emitting them as lockstep PageLine
  // rows at one shared yPos: the SOURCE at sourceColX and the TRANSLATION at transColX (today 0
  // and colWidth+gapWidth respectively). The arguments are source-then-translation, NOT
  // left-then-right — the footnote drain and the block style follow the source stream, so passing
  // them the other way round would attribute the source's footnotes to the translation.
  // flushBufferedOriginal lays an unpaired original full-width with the italic "not translated"
  // marker.
  void makePagesTableMode();
  void flushBufferedOriginal();
  void renderSideBySide(std::unique_ptr<ParsedText> sourceBlock, std::unique_ptr<ParsedText> transBlock);
  // Lingua (LinguaLayout::SideBySide): if the outermost block currently held in
  // currentTextBlock is an ORIGINAL paragraph with no translation paired to it, append a
  // short ITALIC "not translated" marker inline after its source text before it lays out, so
  // the missing translation is visible but unobtrusive. Italic rather than a gray level because no
  // per-word gray vehicle can be confined to this marker; see the body. No-op outside SideBySide.
  void appendSideBySideNoTranslationMarkerIfUnpaired();
  // Lingua (LinguaLayout::Interlinear). Same buffering shape as SideBySide --
  // makePagesInterlinearMode holds an original until its translation arrives -- but the pair is
  // emitted as one full-width flow of STRICTLY ALTERNATING rows: one small annotation strip, one
  // source line, one strip, one line, all the way down. An unpaired original falls back to plain
  // makePages() (via flushBufferedOriginal), i.e. no annotation and no marker.
  void makePagesInterlinearMode();
  // Lingua (LinguaLayout::WordLens): makePages() for the Word Lens layout. The block is broken
  // exactly as makePages() breaks it; every resulting line is then emitted with one annotation
  // strip above it holding the glosses of its hard words (modes/word_lens/WordLensLayout.cpp).
  void makePagesWordLens(bool includeLastLine, bool paragraphEnd);
  void renderInterlinear(std::unique_ptr<ParsedText> origBlock, std::unique_ptr<ParsedText> transBlock);
  // One fragment of translation sitting on one annotation strip. A strip normally carries exactly
  // one of these; it carries two when a source line holds the tail of one sentence and the head of
  // the next, which is still ONE strip and one y advance.
  struct InterlinearRun {
    std::unique_ptr<TextBlock> row;  // null is legal: the strip is reserved and nothing is drawn
    int16_t x = 0;                   // EXTRA offset added to the block's left inset at placement
    // Right edge of this run's INK, in the same measure-relative space as x. What the next run on
    // the same strip must clear, so two sentence runs sharing a line can never overprint.
    int16_t rightEdge = 0;
    // Which of the sentence's slots -- i.e. which of ITS source lines, counted from the first --
    // this row belongs over. Carried explicitly because a slot with no room left on its strip is
    // skipped, so a run's position in the vector is not its slot.
    uint16_t slot = 0;
  };
  // The geometry one sentence's annotation rows are fitted to: each row gets the horizontal room its
  // OWN source line leaves free. All measure-relative, like InterlinearRun::x.
  struct InterlinearBands {
    // The paragraph's source lines. Rows firstLine..firstLine+slots-1 must still be live — the
    // caller releases a line only once it has been emitted, which is strictly after this is read.
    const std::vector<std::unique_ptr<TextBlock>>* srcLines = nullptr;
    size_t firstLine = 0;  // index of the sentence's first source line in *srcLines
    size_t slots = 0;      // source lines the sentence occupies, i.e. rows available to it
    int16_t headX = 0;     // x its source sentence starts at: where row 0 wants to start
    int16_t tailX = 0;     // x the NEXT source sentence starts at, or the measure when it owns the line
    int16_t floorX = 0;    // ink already on row 0's strip; the row starts clear of it, never over it
  };
  // Lay ONE annotation (a group of source sentences sharing a translation) out into at most `slots`
  // rows -- one per source line the sentence occupies -- and append them to `runs` in slot order.
  //
  // Each row is wrapped at the width of its OWN band and placed as a whole box at the band's start,
  // which is what puts an LTR row's first glyph and an RTL row's last one on their sentence's x. The
  // widths are chosen so the text fills every slot in proportion rather than packing into the first
  // rows and leaving the rest of the sentence's strips blank. A row never crosses the panel edge and
  // never crosses the ink already on its strip; short of that it is free to run past its band, which
  // is how an over-long translation stretches instead of losing its tail.
  //
  // `stream` is the PARAGRAPH's annotation text, owned by the caller and drained from the front:
  // this call appends the sentence's own words behind whatever the previous sentence could not
  // place, and leaves its own unplaced tail there for the next one. So the leftover is carried
  // forward through strips that already exist rather than dropped, and only the caller -- at the end
  // of the paragraph, where no further strip exists -- can observe a real loss. Passing a stream
  // that is not empty on entry is the normal case, not an error.
  void buildAnnotationRuns(const InterlinearAnnotation& annotation, const ParsedText& transBlock,
                           const InterlinearBands& bands, uint16_t measureWidth, int annotationFont,
                           std::vector<InterlinearRun>& runs, ParsedText& stream);
  // Place one already-laid-out row at an absolute y. No page-break test and no y advance -- both
  // belong to the pair, not to a row (see emitInterlinearPair).
  void placeInterlinearRow(std::unique_ptr<TextBlock> row, int16_t xPos, int16_t yPos, LineFontRole role);
  // Emit ONE annotation strip plus ONE source line as an atomic, fixed-height group: the strip is
  // reserved whether or not anything is drawn on it, so the page keeps a dead-even
  // annotation/source/annotation/source pitch and no source line ever follows another directly.
  //
  // A null `srcLine` emits a TAIL strip: the group is the strip alone, it advances y by stripHeight
  // only, and it releases no footnote (it carries no source words). That is how a translation too
  // long for every strip above its paragraph is still drawn instead of being cut -- see the tail
  // drain at the end of renderInterlinear. srcRowHeight is ignored in that case.
  void emitInterlinearPair(std::vector<InterlinearRun>& runs, std::unique_ptr<TextBlock> srcLine, int stripHeight,
                           int srcRowHeight, int16_t leftInset, uint32_t sourceOffset);
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyInlinePresentationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushBlockTextStyleEntry(const CssStyle& cssStyle);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const float lineCompression, const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
      const LinguaLayout linguaLayout = LinguaLayout::Both, const std::string& bookPrimaryLang = "",
      const int translationFontId = 0, const int annotationFontId = 0,
      const InterlinearPairFn interlinearPairFn = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        linguaLayout(linguaLayout),
        translationFontId(translationFontId),
        annotationFontId(annotationFontId),
        interlinearPairFn(interlinearPairFn),
        bookPrimaryLang(bookPrimaryLang),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();
  void setTextSpacing(const int8_t character, const uint8_t wordPercent) {
    characterSpacing = character;
    wordSpacingPercent = wordPercent;
  }
  void setParagraphIndentSpaces(const uint8_t spaces) { paragraphIndentSpaces = spaces; }
  // Lingua: the file is our `.translated.html` sidecar, whose inserted blocks are not original text.
  void setLinguaTranslatedSidecar(const bool sidecar) { linguaTranslatedSidecar = sidecar; }
  // Lingua: gloss source for LinguaLayout::WordLens (see ReaderRenderSpec::wordLensSource).
  void setWordLensSource(WordLensGlossSource* source) { wordLensSource = source; }

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; false on OOM/stop
  void abortParse();   // tear down without flushing (error / abandon)
  // Safe from completePageFn; cleanup waits until the active callback returns.
  void requestStop();

  // Lingua: `role` is the role the caller MEASURED this line with (see makePages); it decides both
  // the vertical advance and the byte stamped on the emitted PageLine.
  void addLineToPage(std::unique_ptr<TextBlock> line, uint32_t visibleOffset, LineFontRole role = LineFontRole::Body);
  const std::deque<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
