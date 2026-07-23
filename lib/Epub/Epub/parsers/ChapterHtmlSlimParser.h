#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
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

  // Pre-Translation feature:
  uint8_t translationMode = 0;  // 0=Normal, 1=Dark, 2=Light, 3=OrigOnly, 4=TransOnly, 5=SideBySide, 6=Modal
  // Monotonic index over ORIGINAL content paragraphs. Advances once per content-bearing original
  // block regardless of nesting depth, matching the per-content-paragraph granularity of the
  // ModalOverlay/TooltipOverlay reparsers (ParagraphBoundary.h SSOT). Empty/whitespace-only blocks
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

  // Pre-Translation (SideBySide, mode 5): the current ORIGINAL outermost block is buffered here
  // until its paired translation arrives, so both lay out together into two half-width columns
  // (see renderSideBySide). bufferedOriginalParagraphIdx carries the original's paragraph index
  // across the buffering window — the block-level currentBlockParagraphIdx has already advanced to
  // the next block by the time the pair is emitted, so the emitted PageLines would otherwise stamp
  // the wrong index for the Modal overlay's line->paragraph mapping.
  std::unique_ptr<ParsedText> bufferedOriginalBlock = nullptr;
  int16_t bufferedOriginalParagraphIdx = -1;

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
  void makePages();
  // Pre-Translation (SideBySide, mode 5): two-column table layout. makePagesTableMode routes an
  // outermost block to either buffering (an original, held in bufferedOriginalBlock) or pairing
  // (a translation, laid beside the buffered original). renderSideBySide lays a buffered original
  // and its paired translation into two half-width columns, emitting them as lockstep PageLine
  // rows (left at xPos=0, right at rightColX, both at the same yPos). flushBufferedOriginal lays
  // an unpaired original full-width with the dim "not translated" marker.
  void makePagesTableMode();
  void flushBufferedOriginal();
  void renderSideBySide(std::unique_ptr<ParsedText> leftBlock, std::unique_ptr<ParsedText> rightBlock);
  // Pre-Translation (SideBySide, mode 5): if the outermost block currently held in
  // currentTextBlock is an ORIGINAL paragraph with no translation paired to it, append a
  // short dim "not translated" marker inline after its source text before it lays out, so
  // the missing translation is visible but unobtrusive. No-op outside SideBySide mode.
  void appendSideBySideNoTranslationMarkerIfUnpaired();
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
                                 const uint8_t translationMode = 0, const std::string& bookPrimaryLang = "")

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
        translationMode(translationMode),
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

  void addLineToPage(std::shared_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
