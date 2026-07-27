#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/InterlinearAnnotation.h"
#include "Epub/PageFontSet.h"
#include "Epub/ParsedText.h"
#include "Epub/PtLayout.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn;
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

  // Pre-Translation feature: the page layout to produce. The parser never sees the user's display
  // mode -- CrossPointSettings::ptLayoutForDisplayMode() collapses the modes onto these layouts.
  PtLayout ptLayout = PtLayout::Both;
  // Pre-Translation: font the TRANSLATED text is laid out in, or 0 for "same as fontId" (the ONLY
  // unset sentinel — negative ids are normal, see PageFontSet.h). A non-zero id makes translated
  // lines a second type size on the page: they are measured, broken and advanced with it, and
  // stamped LineFontRole::Translation so the renderer resolves the same id back out of the page's
  // PageFontSet. 0 keeps every line Body, which is byte-for-byte the pre-existing layout.
  int translationFontId = 0;
  // Pre-Translation (PtLayout::Interlinear): font the small ANNOTATION rows are laid out in, or 0
  // for "same as fontId" (the same single sentinel as translationFontId). Rows are measured, broken
  // and advanced with it and stamped LineFontRole::Annotation, so the renderer resolves the same id
  // back out of the page's PageFontSet.
  int annotationFontId = 0;
  // Pre-Translation (PtLayout::Interlinear): the app's sentence aligner (see
  // Epub/InterlinearAnnotation.h). nullptr means "no annotations": the source paragraph still lays
  // out, it just carries no rows.
  InterlinearPairFn interlinearPairFn = nullptr;
  // Reusable annotation buffer for the interlinear pass, sized once on the first annotated paragraph
  // (INTERLINEAR_MAX_ANNOTATIONS entries, 300 bytes) and reused for every paragraph after it -- no
  // per-paragraph allocation, and nothing allocated at all under any other layout.
  std::vector<InterlinearAnnotation> interlinearAnnotations;
  // Monotonic index over ORIGINAL content paragraphs. Advances once per content-bearing original
  // block regardless of nesting depth, matching the per-content-paragraph granularity of the
  // PageTranslationOverlay/TooltipOverlay reparsers (ParagraphBoundary.h SSOT). Empty/whitespace-only blocks
  // and translated blocks do NOT advance it.
  int16_t paragraphCounter = 0;
  // Paragraph index stamped on the currently-open block's laid-out lines; -1 before any block opens.
  int16_t currentBlockParagraphIdx = -1;
  // True once the current physical text block has been assigned a paragraph index. Reset to false
  // whenever startNewTextBlock creates a FRESH ParsedText, so a wrapper container and the child
  // block that reuses its empty ParsedText (via the isEmpty / listItemBulletOnly reuse paths) share
  // ONE index — the wrapper never wastes an index, and each distinct content block consumes exactly
  // one. This replaces the former "outermost block" gate that collapsed div/blockquote/li-wrapped
  // subtrees to a single index.
  bool currentBlockIndexAssigned = false;
  bool currentBlockIsTranslated = false;  // True when the currently-open block has lang= differing from bookPrimaryLang
  std::string bookPrimaryLang;            // Book's content.opf language; a differing lang= marks a translated block
  std::string translatedHyphenLang;       // Last lang= applied to the Hyphenator's translated slot; avoids re-resolving

  // Pre-Translation (SideBySide and Interlinear): the current ORIGINAL outermost block is buffered
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
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    // Pre-Translation: true when the enclosing block/inline element has a lang= attribute
    // differing from the book's primary language (propagated to children through nesting).
    bool isTranslatedBlock = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
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
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  // Pre-Translation: true when the block currently being parsed is one the active ptLayout
  // drops, so its words never reach the layout engine. Shared by flushPartWordBuffer (which drops
  // the word) and the <ruby>/<rt> handlers (which must not annotate words that were never added).
  bool wordIsFiltered() const;
  // Pre-Translation: the role every line of the currently-open block carries. Reads the SAME
  // block-level translated signal as the per-block hyphenation slot and the word-drop filter
  // (currentBlockIsTranslated), so the parser keeps exactly one notion of "translated".
  LineFontRole currentLineRole() const;
  // The font id a role is MEASURED and ADVANCED with. Built through the same PageFontSet resolver
  // the renderer uses, with the same slots CrossPointSettings::readerPageFontSet() fills, so a line
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
  // Pre-Translation: one block's footnote ledger — the anchors pending for it and the word base those
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
  void makePages();
  // Pre-Translation (PtLayout::SideBySide): two-column table layout. makePagesTableMode routes an
  // outermost block to either buffering (an original, held in bufferedOriginalBlock) or pairing
  // (a translation, laid beside the buffered original). renderSideBySide lays a buffered original
  // and its paired translation into two half-width columns, emitting them as lockstep PageLine
  // rows (left at xPos=0, right at rightColX, both at the same yPos). flushBufferedOriginal lays
  // an unpaired original full-width with the dim "not translated" marker.
  void makePagesTableMode();
  void flushBufferedOriginal();
  void renderSideBySide(std::unique_ptr<ParsedText> leftBlock, std::unique_ptr<ParsedText> rightBlock);
  // Pre-Translation (PtLayout::SideBySide): if the outermost block currently held in
  // currentTextBlock is an ORIGINAL paragraph with no translation paired to it, append a
  // short dim "not translated" marker inline after its source text before it lays out, so
  // the missing translation is visible but unobtrusive. No-op outside SideBySide mode.
  void appendSideBySideNoTranslationMarkerIfUnpaired();
  // Pre-Translation (PtLayout::Interlinear). Same buffering shape as SideBySide --
  // makePagesInterlinearMode holds an original until its translation arrives -- but the pair is
  // emitted as one full-width flow of STRICTLY ALTERNATING rows: one small annotation strip, one
  // source line, one strip, one line, all the way down. An unpaired original falls back to plain
  // makePages() (via flushBufferedOriginal), i.e. no annotation and no marker.
  void makePagesInterlinearMode();
  void renderInterlinear(std::unique_ptr<ParsedText> origBlock, std::unique_ptr<ParsedText> transBlock);
  // One fragment of translation sitting on one annotation strip. A strip normally carries exactly
  // one of these; it carries two when a source line holds the tail of one sentence and the head of
  // the next, which is still ONE strip and one y advance.
  struct InterlinearRun {
    std::shared_ptr<TextBlock> row;  // null is legal: the strip is reserved and nothing is drawn
    int16_t x = 0;                   // EXTRA offset added to the block's left inset at placement
  };
  // Lay ONE annotation (a group of source sentences sharing a translation) out into at most `slots`
  // rows and append them to `chunks`. `headX` is the x its source sentence starts at and becomes the
  // block's first-line indent, so chunk 0 occupies "from there to the right margin" and chunks 1..N
  // the full measure -- geometrically congruent with the source sentence's own lines. Rows past
  // `slots` are discarded (see the truncation note in renderInterlinear). Emits nothing for an empty
  // translation.
  void buildAnnotationChunks(const InterlinearAnnotation& annotation, const ParsedText& transBlock, int16_t headX,
                             uint16_t measureWidth, int annotationFont, size_t slots, bool rtlTarget,
                             std::vector<std::shared_ptr<TextBlock>>& chunks);
  // Place one already-laid-out row at an absolute y. No page-break test and no y advance -- both
  // belong to the pair, not to a row (see emitInterlinearPair).
  void placeInterlinearRow(const std::shared_ptr<TextBlock>& row, int16_t xPos, int16_t yPos, LineFontRole role);
  // Emit ONE annotation strip plus ONE source line as an atomic, fixed-height group: the strip is
  // reserved whether or not anything is drawn on it, so the page keeps a dead-even
  // annotation/source/annotation/source pitch and no source line ever follows another directly.
  void emitInterlinearPair(const std::vector<InterlinearRun>& runs, const std::shared_ptr<TextBlock>& srcLine,
                           int stripHeight, int srcRowHeight, int16_t leftInset);
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const bool focusReadingEnabled,
                                 const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 std::vector<std::string> tocAnchors = {},
                                 const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
                                 const PtLayout ptLayout = PtLayout::Both, const std::string& bookPrimaryLang = "",
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
        ptLayout(ptLayout),
        translationFontId(translationFontId),
        annotationFontId(annotationFontId),
        interlinearPairFn(interlinearPairFn),
        bookPrimaryLang(bookPrimaryLang),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

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
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  // `role` is the role the caller MEASURED this line with (see makePages): it decides both the
  // vertical advance here and the byte stamped on the emitted PageLine, so measurement, pitch and
  // drawing can never be resolved from three different fonts.
  void addLineToPage(std::shared_ptr<TextBlock> line, LineFontRole role);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
