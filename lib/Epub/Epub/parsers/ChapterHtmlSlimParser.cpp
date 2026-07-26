#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <iterator>
#include <new>

#include "../../../../src/fontIds.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/TranslationDetection.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "ParagraphBoundary.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;

// This number comes from PR #73
// If we have > 750 words buffered up, perform the layout and consume out all but the last line
// There should be enough here to build out 1-2 full pages and doing this will free up a lot of
// memory.
// Spotted when reading Intermezzo, there are some really long text blocks in there.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;

// When CSS is enabled, flush earlier to save RAM. 320 is still more than enough to build a CJK
// page at font size 14
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
// Paragraph-boundary block tags (p, li, div, br, blockquote and h1..h6) now live
// in the shared paraboundary predicate (ParagraphBoundary.h) so the layout parser
// and the PageTranslationOverlay SAX reparser cannot diverge. HEADER_TAGS above is retained
// only for header-specific STYLING (centered + bold), not boundary detection.
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
constexpr const char* SKIP_TAGS[] = {"head", "rp"};

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

// Single source of truth for "is this a paragraph-boundary tag" — the union of
// HEADER_TAGS and the container/hard-break block tags. Forwards to the shared
// paraboundary predicate that the overlay reparser also uses.
bool isHeaderOrBlock(const char* name) { return paraboundary::isParagraphBlockTag(name); }

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if ((decoration & CssTextDecoration::Underline) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyDirectionToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  effectiveDirectionDefined = currentCssStyle.hasDirection();
  effectiveDirection = currentCssStyle.direction;
  effectiveSup = false;
  effectiveSub = false;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
  }

  // Keep inherited direction in the active empty text block so upcoming block starts
  // can inherit from non-block ancestors such as <html dir="rtl"> / <body dir="rtl">.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    if (effectiveDirectionDefined) {
      style.directionDefined = true;
      style.isRtl = (effectiveDirection == CssTextDirection::Rtl);
    } else {
      style.directionDefined = false;
      style.isRtl = false;
    }
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

// Pre-Translation: layout-based block filtering, shared by flushPartWordBuffer and the ruby
// handlers. Both, SideBySide and Interlinear emit everything (SideBySide pairs the two languages into
// columns instead of dropping either; Interlinear needs the translated block to become a ParsedText
// so renderInterlinear can read its words for the annotation rows -- it is never laid out as a
// paragraph of its own); OriginalOnly drops translated text -- which is also what the Page
// Translation and Tooltip display modes need, since they surface translations through a popup at view time and
// emitting them inline would double the text and break the tooltip's underline/sentence-index math.
// The top of the inline style stack carries whether the current text belongs to a translated block
// (block-opening and inline tags stamp isTranslatedBlock onto their StyleStackEntry, and children
// inherit it through nesting).
bool ChapterHtmlSlimParser::wordIsFiltered() const {
  const bool inTranslatedBlock = !inlineStyleStack.empty() && inlineStyleStack.back().isTranslatedBlock;
  switch (ptLayout) {
    case PtLayout::OriginalOnly:
      return inTranslatedBlock;
    case PtLayout::TranslationOnly:
      return !inTranslatedBlock;
    case PtLayout::Both:
    case PtLayout::SideBySide:
    case PtLayout::Interlinear:
      return false;
  }
  return false;  // unreachable: every enumerator returns above
}

// Pre-Translation: which role the lines of the block currently being laid out carry. Only one
// layout puts translated text in the main flow as a SECOND type size, so only one can tag a line:
//
//   Both           the two languages flow inline; a distinct translation font makes the translated
//                  blocks visibly secondary. This is the Interleaved size's only layout. (Normal
//                  shares this layout and must stay body-size, which is why the app hands a 0
//                  translation font for every mode but Interleaved -- the layout engine cannot tell
//                  the two modes apart, and must not try to.)
//   OriginalOnly   translated words never reach a line at all (wordIsFiltered drops them), so there
//                  is nothing to tag; the two overlay modes that map here composite their own,
//                  separately-sized text over the finished page.
//   TranslationOnly  the translation IS the page's primary text -- shrinking it would shrink the
//                  whole chapter, so it stays Body.
//   SideBySide     both columns are the same face by design (a shrunken column would defeat the
//                  pairing), so neither is tagged; renderSideBySide leaves both at Body.
//   Interlinear    the translated text does NOT flow inline at all: renderInterlinear re-emits it
//                  into its own small rows tagged LineFontRole::Annotation, which resolve through the
//                  annotation slot instead. Source lines therefore stay Body, and this function is
//                  never even reached for the translated block (it is consumed by the pairing, not
//                  flushed through makePages).
//
// Reads the block-level translated flag, not the per-word inline-stack one wordIsFiltered() uses: a
// role applies to a whole laid-out line, and currentBlockIsTranslated is exactly the granularity of
// the block that is being flushed (see the makePagesTableMode comment for why it is still valid
// here).
LineFontRole ChapterHtmlSlimParser::currentLineRole() const {
  // No distinct translation font configured: every line is Body, i.e. exactly the pre-existing
  // layout, and fontIdForRole would resolve Translation back to fontId anyway.
  if (translationFontId == 0 || !currentBlockIsTranslated) return LineFontRole::Body;
  switch (ptLayout) {
    case PtLayout::Both:
      return LineFontRole::Translation;
    case PtLayout::OriginalOnly:
    case PtLayout::TranslationOnly:
    case PtLayout::SideBySide:
    case PtLayout::Interlinear:
      break;
  }
  return LineFontRole::Body;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Pre-Translation: the top of the inline style stack carries whether the word being flushed
  // belongs to a translated block (block-opening and inline tags stamp isTranslatedBlock onto
  // their StyleStackEntry, and children inherit it through nesting).
  const bool inTranslatedBlock = !inlineStyleStack.empty() && inlineStyleStack.back().isTranslatedBlock;

  // Mode-based filtering: drop the word entirely so it never reaches the layout engine.
  // See wordIsFiltered() above for the per-mode rules.
  // Reset continuation on drop: a dropped word must not attach to the next (this also keeps the
  // CJK MAX_WORD_SIZE split intact, since both halves of a split word carry the same block state
  // and are filtered identically).
  if (wordIsFiltered()) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }
  // Pre-Translation: tag translated words so the renderer can apply gray-level dimming in
  // Dark/Light modes. The TRANSLATED bit (64) composes with the existing style bits.
  if (inTranslatedBlock) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::TRANSLATED);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      BlockStyle incoming = blockStyle;
      if (style.fromBrElement) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      return;
    }

    // Pre-Translation: the two PAIRING layouts route through their own builder, which buffers
    // originals and pairs them with their translations. currentBlockIsTranslated and
    // currentBlockParagraphIdx are stamped AFTER this flush by the caller, so here they still
    // describe the block being flushed — exactly what those builders inspect.
    if (ptLayout == PtLayout::SideBySide) {
      makePagesTableMode();
    } else if (ptLayout == PtLayout::Interlinear) {
      makePagesInterlinearMode();
    } else {
      makePages();
    }
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, blockStyle));
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
  // Pre-Translation: a fresh physical text block has not been assigned a paragraph index yet. The
  // next content block-open (original) that lands on it claims the index and advances the counter;
  // nested opens that reuse this same block (empty / li-bullet reuse paths above) leave the flag set
  // and inherit the index, so exactly one index is consumed per distinct content block.
  currentBlockIndexAssigned = false;
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, dir, and lang attributes for CSS/RTL/Pre-Translation processing.
  // langAttr detects translated paragraphs (Calibre and CrossPoint emit translated blocks with a
  // lang= / xml:lang= attribute differing from the book's primary language declared in content.opf).
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  const char* langAttr = nullptr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      } else if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0) {
        langAttr = atts[i + 1];
      }
    }
  }

  // Pre-Translation: determine whether this element introduces (or sits inside) a translated block.
  // Skip html/body to avoid false-positives from a document-level lang (e.g. <html lang="en">).
  // The language comparison itself is translationdetect::isTranslatedLangTag -- the SAME predicate
  // Section's per-chapter "does this chapter have a translation" gate scans with, so the gate can
  // never enable a layout that finds nothing to filter (or refuse one that would have worked).
  // It compares primary subtags case-insensitively, so `uk-UA` in an `en` book is translated while
  // `en-GB` in an `en` book is not.
  const bool langTagAllowed = strcmp(name, "html") != 0 && strcmp(name, "body") != 0;
  const bool isExplicitTranslated =
      langTagAllowed && translationdetect::isTranslatedLangTag(langAttr, self->bookPrimaryLang.c_str());
  const bool inheritedTranslated = !self->inlineStyleStack.empty() && self->inlineStyleStack.back().isTranslatedBlock;
  const bool currentIsTranslated = isExplicitTranslated || inheritedTranslated;

  // Point the Hyphenator's translated slot at this block's language so its words hyphenate with
  // their own script's rules (the primary/book hyphenator rejects every word in the other script).
  // Baked into section.bin at layout time; the lang= here is the one TranslatingHtmlRewriter emits.
  // Set at block-open (before this block's words lay out); guarded so it re-resolves only on change.
  if (isExplicitTranslated && self->translatedHyphenLang != langAttr) {
    self->translatedHyphenLang = langAttr;
    Hyphenator::setTranslatedLanguage(langAttr);
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Special handling for tables/cells: flatten into per-cell paragraphs with a prefixed header.
  if (strcmp(name, "table") == 0) {
    // skip nested tables
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableColIndex += 1;

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                           ? CssTextAlign::Justify
                           : static_cast<CssTextAlign>(self->paragraphAlignment);
    tableCellBlockStyle.alignment = align;
    self->startNewTextBlock(tableCellBlockStyle);

    const std::string headerText =
        "Tab Row " + std::to_string(self->tableRowIndex) + ", Cell " + std::to_string(self->tableColIndex) + ":";
    StyleStackEntry headerStyle;
    headerStyle.depth = self->depth;
    headerStyle.hasBold = true;
    headerStyle.bold = false;
    headerStyle.hasItalic = true;
    headerStyle.italic = true;
    // Pre-Translation: this synthetic "Tab Row N, Cell M:" label is UI text, not book content,
    // so it is intentionally left unmarked (isTranslatedBlock stays false).
    self->inlineStyleStack.push_back(headerStyle);
    self->updateEffectiveInlineStyle();
    const CssTextDecoration savedTextDecoration = self->effectiveTextDecoration;
    self->effectiveTextDecoration = CssTextDecoration::None;
    self->characterData(userData, headerText.c_str(), static_cast<int>(headerText.length()));
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->effectiveTextDecoration = savedTextDecoration;
    self->nextWordContinues = false;
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();

    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            {
              // Probe the dimensions from the entry's first bytes (early-aborted
              // inflate, a few KB) instead of extracting the whole image now —
              // extraction is deferred to the first render of the page (see
              // ImageBlock's lazy extractor). This is what keeps first-open of an
              // image-heavy chapter from stalling for seconds per image.
              ImageDimensions dims = {0, 0};
              ImageDimsProbe headerProbe;
              self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
              bool gotDimensions = headerProbe.getDimensions(dims);

              if (!gotDimensions) {
                // No header within the stream (rare) — fall back to extracting the
                // whole image and probing the file. That can take seconds, so
                // surface the indexing popup first (single-shot per parser).
                if (self->popupFn && !self->imagePopupFired) {
                  self->imagePopupFired = true;
                  self->popupFn();
                }
                HalFile cachedImageFile;
                bool extractSuccess = false;
                if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
                  extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
                  cachedImageFile.flush();
                  cachedImageFile.close();
                }
                if (extractSuccess) {
                  // Retry to absorb SD-card sync latency on slow cards, and to close
                  // the silent-drop bug where a single getDimensions failure was fatal.
                  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                  for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                    if (attempt > 0) {
                      delay(50);  // Give a slow SD card time to finish syncing before retrying
                    }
                    gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                  }
                } else {
                  LOG_ERR("EHP", "Failed to extract image");
                }
              }

              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                const CssStyle& imgStyle = cssStyle;
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // Apply vertical margins from the container to the image.
                // Top margin lives on the empty text block (deposited via vertical merge
                // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
                // deferred application at element close, so read it from the stack.
                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = bs.topInset();
                  if (self->blockStyleStack.size() > 1) {
                    imageMarginBottom = self->blockStyleStack.back().bottomInset();
                  }
                }

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                       self->xpathListItemIndex);
                  self->completedPageCount++;
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                  self->currentPageNextY = 0;
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                // Apply top margin from container block
                self->currentPageNextY += imageMarginTop;

                // Create ImageBlock and add to page
                // nothrow: make_shared uses bare new, which aborts on OOM under
                // -fno-exceptions; images arrive mid-parse when the heap is at its
                // most loaded, so this must fail soft into the null-check below.
                auto imageBlock = std::shared_ptr<ImageBlock>(
                    new (std::nothrow) ImageBlock(cachedImagePath, resolvedPath, displayWidth, displayHeight));
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage =
                    std::shared_ptr<PageImage>(new (std::nothrow) PageImage(imageBlock, xPos, self->currentPageNextY));
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
                self->currentPageNextY += displayHeight + imageMarginBottom;

                // The image consumed the empty block's accumulated vertical spacing.
                // Reset the block so the Vertical merge in startNewTextBlock doesn't
                // re-apply the same margins to the next text paragraph.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                             ? CssTextAlign::Justify
                                             : static_cast<CssTextAlign>(self->paragraphAlignment);
                  self->currentTextBlock->setBlockStyle(resetStyle);
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    self->flushPartWordBuffer();
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    self->flushPartWordBuffer();
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, std::size(SKIP_TAGS))) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      entry.isTranslatedBlock = currentIsTranslated;  // Pre-Translation: inherit from enclosing block
      applyDirectionToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    // Pre-Translation: push a no-op inline-style marker so child inline tags inherit
    // isTranslatedBlock through nesting. It carries no style bits, so it does not affect
    // effective styles; it is popped by the generic inline-pop in endElement.
    StyleStackEntry translationEntry;
    translationEntry.depth = self->depth;
    translationEntry.isTranslatedBlock = currentIsTranslated;
    self->inlineStyleStack.push_back(translationEntry);
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    // Pre-Translation (SideBySide, mode 5): the unpaired-original marker is no longer emitted
    // pre-emptively here. makePagesTableMode buffers each original and only decides it is
    // unpaired (appending the marker via flushBufferedOriginal) once the next original arrives
    // or EOF is reached, so a pre-emptive mark here would double-mark and bake into paired blocks.
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
    // Pre-Translation: stamp the paragraph index AFTER startNewTextBlock so the previous block is
    // flushed under the previous translation state. The counter advances once per content-bearing
    // original block at ANY nesting depth: the FIRST open to claim a freshly-created text block
    // takes the next index (currentBlockIndexAssigned latches so nested opens reusing the same empty
    // block inherit it instead of double-counting). A translated block never advances the counter;
    // it pairs with the most recent original paragraph (paragraphCounter - 1).
    self->currentBlockIsTranslated = currentIsTranslated;
    if (currentIsTranslated) {
      self->currentBlockParagraphIdx = static_cast<int16_t>(self->paragraphCounter - 1);
    } else if (!self->currentBlockIndexAssigned) {
      self->currentBlockParagraphIdx = self->paragraphCounter;
      self->paragraphCounter++;
      self->currentBlockIndexAssigned = true;
    }
  } else if (paraboundary::isParagraphBlockTag(name)) {
    // Reached only for NON-header block tags (headers handled by the branch
    // above), i.e. exactly the old BLOCK_TAGS set {p, li, div, br, blockquote}.
    if (paraboundary::isHardBreak(name)) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // Tag the new block so startNewTextBlock can inject a full line-height gap if
      // the block remains empty (i.e. <br> is a section separator between paragraphs).
      // If the block gets text added before the next block opens it becomes non-empty,
      // goes through makePages() normally, and the flag has no effect (inline <br> case).
      BlockStyle brStyle = self->blockStyleStack.back();
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      // Pre-Translation: push a no-op inline-style marker for isTranslatedBlock propagation.
      StyleStackEntry translationEntry;
      translationEntry.depth = self->depth;
      translationEntry.isTranslatedBlock = currentIsTranslated;
      self->inlineStyleStack.push_back(translationEntry);
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      // Pre-Translation (SideBySide, mode 5): the unpaired-original marker is no longer emitted
      // pre-emptively here — makePagesTableMode/flushBufferedOriginal own the unpaired decision
      // (see the header branch above for the full rationale).
      self->startNewTextBlock(accumulated.withoutBottom());
      self->updateEffectiveInlineStyle();
      // Pre-Translation: stamp the paragraph index AFTER startNewTextBlock so the previous block
      // flushed inside it uses the previous translation state. The counter advances once per
      // content-bearing original block at ANY nesting depth: the FIRST open to claim a freshly-
      // created text block takes the next index (currentBlockIndexAssigned latches so nested opens
      // reusing the same empty block inherit it instead of double-counting). A translated block
      // never advances the counter; it pairs with the most recent original (paragraphCounter - 1).
      self->currentBlockIsTranslated = currentIsTranslated;
      if (currentIsTranslated) {
        self->currentBlockParagraphIdx = static_cast<int16_t>(self->paragraphCounter - 1);
      } else if (!self->currentBlockIndexAssigned) {
        self->currentBlockParagraphIdx = self->paragraphCounter;
        self->paragraphCounter++;
        self->currentBlockIndexAssigned = true;
      }

      if (strcmp(name, "li") == 0) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
        self->listItemBulletOnly = true;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
    // Pre-Translation: inherit translated state (the helper does not know about it).
    self->inlineStyleStack.back().isTranslatedBlock = currentIsTranslated;
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
    // Pre-Translation: inherit translated state (the helper does not know about it).
    self->inlineStyleStack.back().isTranslatedBlock = currentIsTranslated;
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    entry.isTranslatedBlock = currentIsTranslated;  // Pre-Translation: inherit from enclosing block
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    entry.isTranslatedBlock = currentIsTranslated;  // Pre-Translation: inherit from enclosing block
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    entry.isTranslatedBlock = currentIsTranslated;  // Pre-Translation: inherit from enclosing block
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling. Also push an entry when this element
    // introduces an explicit translated marker (lang=) so it propagates to children, even without
    // any CSS styling.
    const bool hasCssStyle = cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
                             cssStyle.hasDirection() || cssStyle.hasVerticalAlign();
    if (hasCssStyle || isExplicitTranslated) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;                      // Track depth for matching pop
      entry.isTranslatedBlock = currentIsTranslated;  // Pre-Translation: inherit or set
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      if (cssStyle.hasVerticalAlign()) {
        if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
          entry.hasSup = true;
          entry.sup = true;
        } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
          entry.hasSub = true;
          entry.sub = true;
        }
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing. Pre-Translation: skip the append when the
  // enclosing block is dropped for the active mode — its base words never reach the text block, so
  // the annotation has nothing to attach to (see the </rt> handler).
  if (self->collectingRubyText) {
    if (!self->wordIsFiltered()) {
      self->rubyTextBuffer.append(s, len);
    }
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // Keep token growth bounded: CSS-heavy spans can fragment text into many tiny
  // words, so flush earlier when embedded CSS is active. We still keep the
  // "exclude last line" behavior to preserve paragraph flow across chunks.
  const size_t blockWordCount = self->currentTextBlock->size();
  const size_t softFlushThreshold =
      self->embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS : TEXT_BLOCK_SOFT_FLUSH_WORDS;
  if (blockWordCount > softFlushThreshold) {
    LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(blockWordCount));
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    // Same role/font pairing as makePages(): this is the SAME open block, just flushed early, so its
    // lines must be measured, advanced and stamped exactly as the block's remaining lines will be.
    const LineFontRole role = self->currentLineRole();
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontIdForRole(role), effectiveWidth,
        [self, role](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock, role); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    // Pre-Translation: in a mode that drops this block (OrigOnly/TransOnly/PageTranslation/Tooltip), the base
    // characters never became words, so baseWordCount would be 0 and the fallback below would walk
    // back and glue this furigana onto the last SURVIVING word of an unrelated run. Skip the whole
    // distribution instead; rubyTextBuffer is cleared below either way.
    if (self->inRuby && self->currentTextBlock && !self->wordIsFiltered()) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    // get rid of all text inside the nested table
    self->partWordBufferIndex = 0;
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, get rid of its content");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->nextWordContinues = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
      // Start a new text block with the parent style to prevent subsequent bare text
      // from inheriting the closed block style (e.g. alignment or margins).
      //
      // Pre-Translation: this close-side call is now the point at which the just-closed block is
      // flushed — startNewTextBlock() routes a non-empty currentTextBlock through
      // makePagesTableMode() in SideBySide (mode 5) or makePages() otherwise. Both READ
      // currentBlockParagraphIdx / currentBlockIsTranslated, so those fields must still describe
      // the block being closed and are intentionally NOT cleared here. The next content
      // block-open overwrites both, and the EOF flush in finishParse() likewise relies on the last
      // opened block's state surviving after all endElement callbacks have fired.
      //
      // Splitting on close does NOT double-count paragraphs: the fresh ParsedText created below
      // resets currentBlockIndexAssigned to false, and the next block-open finds that block empty,
      // takes the reuse early-return in startNewTextBlock (which leaves the latch untouched), and
      // then claims exactly one index. A close is never itself a block-open, so it never advances
      // paragraphCounter.
      self->startNewTextBlock(self->blockStyleStack.back());
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  void* const buf = XML_GetBuffer(xmlParser_, PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);

  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const int done = parseFile_.available() == 0;

  if (XML_ParseBuffer(xmlParser_, static_cast<int>(len), done) == XML_STATUS_ERROR) {
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
            XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    return ParseStatus::Error;
  }

  return done ? ParseStatus::Done : ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
}

bool ChapterHtmlSlimParser::finishParse() {
  if (xmlParser_) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - parseStartTime_);
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    // Pre-Translation: flush the trailing outermost block through the pairing builder that owns this
    // layout. A trailing original is buffered (not laid out) there, so drain any block still buffered
    // — the last original of the chapter — full-width via flushBufferedOriginal (which appends the
    // dim "not translated" marker under SideBySide only; Interlinear just shows the source).
    if (ptLayout == PtLayout::SideBySide || ptLayout == PtLayout::Interlinear) {
      if (ptLayout == PtLayout::SideBySide) {
        makePagesTableMode();
      } else {
        makePagesInterlinearMode();
      }
      if (bufferedOriginalBlock) {
        flushBufferedOriginal();
      }
    } else {
      makePages();
    }
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line, const LineFontRole role) {
  // Pitch follows the line's OWN font. yPos is the top of the line box and TextBlock::render puts the
  // baseline at yPos + rubyShift + ascender(drawing font), with the glyphs living inside one
  // advanceY of that top -- so advancing by exactly this line's advanceY tiles the boxes edge to
  // edge whatever font each one used. A smaller translated line therefore consumes less height and
  // the next line (of either size) starts right where it ended: no overlap, no leftover gap. Both
  // terms must come from the SAME id the line was measured with, or the ruby shift baked into the
  // advance would not match the one render() adds to the baseline.
  const int roleFontId = fontIdForRole(role);
  const int lineHeight = renderer.getLineHeight(roleFontId, lineCompression) +
                         line->getRubyShift(renderer.getFontAscenderSize(roleFontId));

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  auto pageLine = std::make_shared<PageLine>(line, xOffset, currentPageNextY);
  // Pre-Translation: stamp the line with its originating paragraph index so the renderer (and the
  // Page Translation overlay) can map rendered lines back to original paragraphs.
  pageLine->paragraphIdx = currentBlockParagraphIdx;
  // ... and with the role it was measured and advanced with, which is the whole of what the renderer
  // needs to draw it in the same font (PageLine::render -> PageFontSet::forRole).
  pageLine->fontRole = role;
  // Pre-Translation: track which original paragraph indices contribute to this page. Translated
  // blocks share their original's index (via the pairing logic in startElement), so the range
  // still represents original paragraphs.
  if (currentBlockParagraphIdx >= 0) {
    if (currentPage->firstParagraphIdx < 0) {
      currentPage->firstParagraphIdx = currentBlockParagraphIdx;
    }
    currentPage->lastParagraphIdx = currentBlockParagraphIdx;
  }
  currentPage->elements.push_back(std::move(pageLine));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::flushPendingFootnotesToCurrentPage() {
  if (pendingFootnotes.empty()) return;
  if (currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
  }
  // Cleared either way. An entry left behind here would be drained against the NEXT block's word base,
  // which is precisely the misattribution this net exists to prevent. Every caller has a live
  // currentPage by construction — makePages and both custom emitters create one before laying a single
  // line — so the undelivered case is unreachable in practice.
  pendingFootnotes.clear();
}

ChapterHtmlSlimParser::FootnoteLedger ChapterHtmlSlimParser::adoptBufferedFootnoteLedger() {
  FootnoteLedger parked;
  parked.wordBase = wordsExtractedInBlock;
  // Swaps, never copies: an entry lives in exactly one ledger at every instant, so nothing can be
  // delivered twice or lost in transit. Both swaps are O(1) and recycle the two buffers between the
  // ledgers instead of allocating, so a chapter of footnoted paragraphs allocates once.
  parked.pending.swap(pendingFootnotes);
  pendingFootnotes.swap(bufferedOriginalFootnotes);
  wordsExtractedInBlock = bufferedOriginalWordsExtracted;
  return parked;
}

void ChapterHtmlSlimParser::releaseFootnoteLedger(FootnoteLedger& parked) {
  // Anything still pending belongs to the buffered block and had no line of its own to land on (its
  // layout produced fewer lines than its anchors need, or bailed out before emitting any), so give it
  // the same best-effort page the end-of-block net gives every such entry rather than letting it cross
  // into the in-flight block's ledger. Normally a no-op: makePages and both custom emitters run that
  // net themselves before returning.
  flushPendingFootnotesToCurrentPage();
  bufferedOriginalFootnotes.swap(pendingFootnotes);
  bufferedOriginalWordsExtracted = 0;
  pendingFootnotes.swap(parked.pending);
  wordsExtractedInBlock = parked.wordBase;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Pre-Translation: resolve the role ONCE for the whole block and use its font for measurement,
  // for the per-line advance (addLineToPage) and for the paragraph gap below, so every vertical
  // number this block contributes is in the type size it is drawn at.
  const LineFontRole role = currentLineRole();
  const int roleFontId = fontIdForRole(role);
  const int lineHeight = renderer.getLineHeight(roleFontId, lineCompression);

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, roleFontId, effectiveWidth,
      [this, role](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock, role); });

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  flushPendingFootnotesToCurrentPage();

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior).
  // Pre-Translation (SideBySide, mode 5): paired blocks lay out through renderSideBySide, not
  // makePages, so there is no longer a translated block flowing through here to suppress. The
  // only mode-5 callers of makePages are full-width fallbacks (an unpaired original, or a
  // translation with no buffered original), which both want normal paragraph spacing.
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

// Pre-Translation (SideBySide, mode 5): route one flushed outermost block to the two-column
// table. currentBlockIsTranslated / currentBlockParagraphIdx still describe the block being
// flushed at this point (the caller stamps the next block's state only after this returns).
void ChapterHtmlSlimParser::makePagesTableMode() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  if (currentBlockIsTranslated) {
    // Translation paragraph: pair it beside the buffered original if one is waiting, otherwise
    // fall back to a full-width layout (a translation with no preceding original — unusual).
    if (bufferedOriginalBlock) {
      // renderSideBySide drains against the LEFT column, i.e. the buffered original, so that block's
      // ledger is the one that must be installed while it runs. A sidecar translation cannot itself
      // carry an anchor (see the drain comment there), but a book whose OWN markup marks a paragraph
      // with a differing lang= can, and its indices restart from a base of their own — parking them
      // keeps them out of the left column's drain, and the ledger swap also pins the word BASE, which
      // is otherwise whatever the translation block left behind if it was large enough to soft-flush.
      FootnoteLedger parkedFootnotes = adoptBufferedFootnoteLedger();
      renderSideBySide(std::move(bufferedOriginalBlock), std::move(currentTextBlock));
      releaseFootnoteLedger(parkedFootnotes);
    } else {
      makePages();
    }
  } else {
    // Original paragraph: if a previous original is still buffered it never got a translation,
    // so lay it out full-width (with the dim marker) before buffering this one for pairing.
    if (bufferedOriginalBlock) {
      flushBufferedOriginal();
    }
    bufferedOriginalParagraphIdx = currentBlockParagraphIdx;
    // The block's footnote ledger goes into the buffer WITH it: its anchor indices are relative to its
    // own words, and startNewTextBlock is about to zero that base for the next block. The buffered
    // ledger is empty here by construction — flushBufferedOriginal above, and every other consumer of
    // the buffer, leaves it so — hence a swap: it hands this block's ledger over AND leaves the
    // in-flight one clean for the next block, with the two buffers recycled rather than reallocated.
    bufferedOriginalFootnotes.swap(pendingFootnotes);
    bufferedOriginalWordsExtracted = wordsExtractedInBlock;
    bufferedOriginalBlock = std::move(currentTextBlock);
  }
}

// Pre-Translation (SideBySide, mode 5): lay out a buffered original that never received a paired
// translation. It renders full-width (via makePages) with the dim "not translated" marker inline.
void ChapterHtmlSlimParser::flushBufferedOriginal() {
  if (!bufferedOriginalBlock) return;

  // Swap the buffered original into currentTextBlock so makePages() lays it out, and point the
  // block-level paragraph state at the buffered original so both the marker guard and the emitted
  // PageLines' paragraphIdx match it. Save/restore the caller's in-flight state around the swap.
  auto savedBlock = std::move(currentTextBlock);
  const int16_t savedParagraphIdx = currentBlockParagraphIdx;
  const bool savedIsTranslated = currentBlockIsTranslated;
  // The footnote ledger travels with the block for the same reason the paragraph index does, and it is
  // load-bearing HERE above all: the in-flight block that TRIGGERED this flush (the next original) has
  // usually already pushed anchors of its own, and its indices restart from base 0 exactly as the
  // buffered block's did. Left in the same ledger, the low-index entries of the block that has not been
  // laid out yet satisfy addLineToPage's `first <= wordsExtractedInBlock` test on one of the buffered
  // block's first lines and were delivered to ITS page, then erased — so the next paragraph's marker
  // appeared one paragraph early, or (once the net below cleared the remainder) not at all.
  FootnoteLedger parkedFootnotes = adoptBufferedFootnoteLedger();

  currentTextBlock = std::move(bufferedOriginalBlock);
  currentBlockParagraphIdx = bufferedOriginalParagraphIdx;
  currentBlockIsTranslated = false;

  appendSideBySideNoTranslationMarkerIfUnpaired();
  makePages();

  releaseFootnoteLedger(parkedFootnotes);
  currentTextBlock = std::move(savedBlock);
  currentBlockParagraphIdx = savedParagraphIdx;
  currentBlockIsTranslated = savedIsTranslated;
}

// Pre-Translation (SideBySide, mode 5): lay an original (left) and its paired translation (right)
// into two half-width columns, emitted as lockstep PageLine rows — left at xPos=0, right at
// rightColX, both sharing one yPos and advancing one lineHeight per row. Both columns stamp the
// original paragraph's index (bufferedOriginalParagraphIdx) so the Page Translation overlay's line->paragraph
// mapping still resolves. RTL is handled per-word inside each half-width line by
// layoutAndExtractLines; the columns themselves are never mirrored (original always left).
void ChapterHtmlSlimParser::renderSideBySide(std::unique_ptr<ParsedText> leftBlock,
                                             std::unique_ptr<ParsedText> rightBlock) {
  if (!leftBlock || !rightBlock) return;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Both columns are laid out AND drawn in the body font: the pairing is what distinguishes the two
  // languages here, and shrinking one column would break the lockstep row geometry this loop relies
  // on (one shared yPos and one shared advance per row). So no line here is tagged -- PageLine's
  // fontRole default (Body) is deliberate, and currentLineRole() returns Body under SideBySide to
  // match. See currentLineRole().
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  const uint16_t gapWidth = static_cast<uint16_t>(viewportWidth * 0.04f);
  const uint16_t colWidth = static_cast<uint16_t>((viewportWidth - gapWidth) / 2);
  const int16_t rightColX = static_cast<int16_t>(colWidth + gapWidth);

  // Lay each column out at half width into its own line vector. Paragraphs are short (one block
  // each), so a small reserve avoids the first few reallocs without over-committing DRAM.
  std::vector<std::shared_ptr<TextBlock>> leftLines;
  std::vector<std::shared_ptr<TextBlock>> rightLines;
  leftLines.reserve(8);
  rightLines.reserve(8);
  leftBlock->layoutAndExtractLines(renderer, fontId, colWidth,
                                   [&leftLines](const std::shared_ptr<TextBlock>& line) { leftLines.push_back(line); });
  rightBlock->layoutAndExtractLines(renderer, fontId, colWidth, [&rightLines](const std::shared_ptr<TextBlock>& line) {
    rightLines.push_back(line);
  });

  // Top spacing comes from the original (left) block.
  const BlockStyle& bs = leftBlock->getBlockStyle();
  if (bs.marginTop > 0) currentPageNextY += bs.marginTop;
  if (bs.paddingTop > 0) currentPageNextY += bs.paddingTop;

  const size_t maxLines = std::max(leftLines.size(), rightLines.size());
  for (size_t i = 0; i < maxLines; i++) {
    // Page-break check: v2 uses the 3-arg completePageFn + explicit page counter.
    if (currentPageNextY + lineHeight > viewportHeight) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    // FOOTNOTES, attributed to the page carrying the anchor exactly as addLineToPage (:1828-1834) does
    // it. SideBySide needs its own copy because a PAIRED paragraph never reaches addLineToPage, and
    // under this layout every translated paragraph pairs: without this, pendingFootnotes accumulated
    // for the whole chapter and was either dumped wholesale onto whatever page happened to be current at
    // the next unpaired paragraph (the end-of-block net, flushPendingFootnotesToCurrentPage) or never
    // delivered at all, since finishParse does not drain it.
    //
    // LEFT (original) column only. The right column is the translation, which the sidecar writes as a
    // fresh block element holding nothing but the ESCAPED plain text of the translation
    // (TranslatingHtmlRewriter's write-out loop, via appendEscaped — '<' becomes "&lt;"), so it carries
    // no <a epub:type="noteref"> and can never push a pending footnote, which is why one counter over
    // the left column's lines is the whole story here.
    //
    // The pending indices and this counter share the LEFT block's base because the caller installed
    // that block's ledger (adoptBufferedFootnoteLedger) before calling: both the entries and the base
    // below are the buffered original's own, and the in-flight block's are parked out of reach. Relying
    // instead on "startNewTextBlock zeroed the counter and nothing advances it" was not sound — the soft
    // flush (:1452) advances it for any block over 750 words (320 with embedded CSS), and the entries of
    // an as-yet-unlaid block sat in the same list. The pre-layout anchor index vs post-layout
    // wordCount() mismatch is addLineToPage's own approximation, kept identical here.
    if (i < leftLines.size()) {
      wordsExtractedInBlock += leftLines[i]->wordCount();
      auto footnoteIt = pendingFootnotes.begin();
      while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
        currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
        ++footnoteIt;
      }
      pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

      auto leftLine = std::make_shared<PageLine>(leftLines[i], 0, currentPageNextY);
      leftLine->paragraphIdx = bufferedOriginalParagraphIdx;
      currentPage->elements.push_back(std::move(leftLine));
      // The page owns this line now (PageLine's ctor takes the shared_ptr BY VALUE and moves it into
      // its member, so the copy made above is the page's own reference), so release ours instead of
      // pinning every line of BOTH columns until the call returns. Nothing below reads an earlier
      // line: this loop only ever touches the CURRENT index of each column, and the sizes it compares
      // against are unaffected by a reset. That restores the makePages peak -- one page's worth of
      // TextBlocks, freed as onPageComplete serializes each page -- for a pair long enough to span
      // several pages, instead of holding object + arena + control block for every line of both
      // columns at once on top of the page being built.
      leftLines[i].reset();
    }
    if (i < rightLines.size()) {
      auto rightLine = std::make_shared<PageLine>(rightLines[i], rightColX, currentPageNextY);
      rightLine->paragraphIdx = bufferedOriginalParagraphIdx;
      currentPage->elements.push_back(std::move(rightLine));
      rightLines[i].reset();  // same handoff as the left column above
    }

    // Both columns belong to the same original paragraph; keep the page's paragraph range current
    // (also fixes up firstParagraphIdx on a page freshly reset by the break above).
    if (bufferedOriginalParagraphIdx >= 0) {
      if (currentPage->firstParagraphIdx < 0) {
        currentPage->firstParagraphIdx = bufferedOriginalParagraphIdx;
      }
      currentPage->lastParagraphIdx = bufferedOriginalParagraphIdx;
    }

    currentPageNextY += lineHeight;
  }

  // Same end-of-block net makePages keeps: every entry in the installed ledger belongs to the LEFT
  // column just emitted (the caller parked the in-flight block's ledger before this call), so flushing
  // it to the current page can never touch another paragraph's entry. The per-line drain above already
  // covers the normal case — an anchor index can never exceed the block's pre-layout word count, and
  // hyphenation only ADDS post-layout words — so this fires only when the left column produced fewer
  // lines than the anchors need (a line dropped to a TextBlock arena OOM).
  flushPendingFootnotesToCurrentPage();

  // Bottom spacing + the usual half-line paragraph gap after the pair.
  if (bs.marginBottom > 0) currentPageNextY += bs.marginBottom;
  if (bs.paddingBottom > 0) currentPageNextY += bs.paddingBottom;
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::appendSideBySideNoTranslationMarkerIfUnpaired() {
  // Only PtLayout::SideBySide surfaces the inline marker; every other layout either drops or
  // pairs content elsewhere.
  if (ptLayout != PtLayout::SideBySide) return;
  // currentBlockIsTranslated reflects the most-recently-opened outermost block. If it was a
  // translation, the preceding original is already paired — no marker. If it was an original,
  // the caller has determined nothing will pair with it (next outermost block is also an
  // original, or we reached EOF), so it is unpaired.
  if (currentBlockIsTranslated) return;
  // No outermost block has opened yet (nothing to mark), or the block is empty/whitespace-only
  // (which the layout parser never counts as a paragraph — see ParagraphBoundary.h).
  if (currentBlockParagraphIdx < 0) return;
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  // Option C (unpaired original): append a short dim "not translated" marker inline after the
  // source text so the gap is visible but unobtrusive. RAM-cheap: a few extra addWord() calls on
  // the block already being flushed, no new buffers. Dimming uses the same per-word vehicle as
  // translated text in v2 — the EpdFontFamily::TRANSLATED style bit, which the renderer maps to
  // the configured translation gray level (see GfxRenderer renderChar*). This is v2's equivalent
  // of the fork's grayLevel=1 marker.
  const char* marker = tr(STR_NO_TRANSLATION);
  std::string markerWord;
  for (const char* p = marker;; ++p) {
    if (*p == ' ' || *p == '\0') {
      if (!markerWord.empty()) {
        currentTextBlock->addWord(markerWord, EpdFontFamily::TRANSLATED);
        markerWord.clear();
      }
      if (*p == '\0') break;
    } else {
      markerWord.push_back(*p);
    }
  }
}

// ── Pre-Translation (PtLayout::Interlinear) ───────────────────────────────────
//
// Route one flushed outermost block to the interlinear builder. Identical buffering shape to
// makePagesTableMode (see there for why currentBlockIsTranslated / currentBlockParagraphIdx are
// still the FLUSHED block's state at this point).
void ChapterHtmlSlimParser::makePagesInterlinearMode() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  if (currentBlockIsTranslated) {
    // Translation paragraph: pair it with the buffered original if one is waiting, otherwise fall
    // back to a full-width layout (a translation with no preceding original — unusual).
    if (bufferedOriginalBlock) {
      // Same ledger handover as makePagesTableMode: emitInterlinearRow drains against the SOURCE rows,
      // i.e. the buffered original, so that block's ledger is the one installed while it runs.
      FootnoteLedger parkedFootnotes = adoptBufferedFootnoteLedger();
      renderInterlinear(std::move(bufferedOriginalBlock), std::move(currentTextBlock));
      releaseFootnoteLedger(parkedFootnotes);
    } else {
      makePages();
    }
  } else {
    // Original paragraph: a previous original still buffered never got a translation, so lay it out
    // full-width (flushBufferedOriginal; the "not translated" marker is SideBySide-only, so an
    // unpaired original here simply appears with no annotation row) before buffering this one.
    if (bufferedOriginalBlock) {
      flushBufferedOriginal();
    }
    bufferedOriginalParagraphIdx = currentBlockParagraphIdx;
    // The block's footnote ledger goes into the buffer WITH it — see makePagesTableMode.
    bufferedOriginalFootnotes.swap(pendingFootnotes);
    bufferedOriginalWordsExtracted = wordsExtractedInBlock;
    bufferedOriginalBlock = std::move(currentTextBlock);
  }
}

void ChapterHtmlSlimParser::buildAnnotationRows(const InterlinearAnnotation& annotation, const ParsedText& transBlock,
                                                const int16_t anchorOffset, const uint16_t measureWidth,
                                                const int annotationFont,
                                                std::vector<std::shared_ptr<TextBlock>>& rows) {
  // A sentence whose translation is empty emits no row at all (never a blank line).
  if (annotation.transEndWord <= annotation.transStartWord) return;

  BlockStyle annStyle;
  // Never justify a row this small over a short measure; and Left is what makes ParsedText treat the
  // block as "naturally aligned", which is the precondition for honouring textIndent at all
  // (ParsedText::resolveFirstLineIndent).
  //
  // textAlignDefined is deliberately left FALSE. It is read in exactly one place — extractLine's
  // "resolved RTL + no explicit text-align + Left" rule, which flips the row to Right — and that is
  // exactly the degradation an RTL TARGET language needs. When the span's words make the row resolve
  // RTL (a Hebrew / Arabic / Persian translation), isNaturalAlign goes false, resolveFirstLineIndent
  // returns 0 and the anchor is lost either way; with the flag set the row was then stranded flush
  // LEFT, i.e. un-anchored AND on the wrong margin. Left false it lands on its own natural margin
  // (flush right) and reads as a whole-line translation. Genuinely anchoring an RTL row over an LTR
  // sentence needs an END-side first-line inset, which BlockStyle cannot express (CSS text-indent
  // insets from the start edge, i.e. the right, under RTL), so v1 does not attempt it. LTR rows are
  // unaffected: the flag only ever gated the isRtl branch.
  annStyle.alignment = CssTextAlign::Left;
  // HANGING INDENT: row 1 starts exactly at the sentence, continuation rows at the margin.
  //
  // A limit is unavoidable — computeLineBreaks force-hyphenates any word wider than the width left to
  // it, so a first row squeezed to a near-zero measure would shred every word — but a sentence that
  // begins past it falls back ALL THE WAY to the margin rather than being parked at the limit. A row
  // sitting at 3/4 of the measure is not over its own sentence and not at the margin either: it reads
  // as translating the words it now sits above, which belong to the PREVIOUS sentence. At the margin
  // it unambiguously means "this whole line". Documented under Version 40 in docs/file-formats.md.
  const int16_t maxIndent = static_cast<int16_t>(measureWidth * 3 / 4);
  annStyle.textIndent = (anchorOffset > 0 && anchorOffset <= maxIndent) ? anchorOffset : 0;
  annStyle.textIndentDefined = true;

  // extraParagraphSpacing=false is not cosmetic here: resolveFirstLineIndent only returns a POSITIVE
  // textIndent when it is false (with it true, indents are suppressed in favour of the paragraph
  // gap). hyphenationEnabled=false keeps a long compound wrapping early instead of being broken at
  // 8pt, and focusReading is a body-text affordance that has no business in an annotation.
  ParsedText annotationText(/*extraParagraphSpacing=*/false, /*hyphenationEnabled=*/false,
                            /*focusReadingEnabled=*/false, annStyle);
  // One growth step for the whole span. This block is constructed per SENTENCE, so without it five
  // parallel vectors double from zero for every sentence of every paragraph on the background build
  // path — the variable-size DRAM churn the reserve-before-push_back rule exists to prevent. The span
  // length is the exact token count for every target but CJK, where per-character splitting can add
  // more and the vectors simply fall back to doubling.
  annotationText.reserveAdditionalWords(annotation.transEndWord - annotation.transStartWord);
  for (uint16_t w = annotation.transStartWord; w < annotation.transEndWord && w < transBlock.size(); w++) {
    // REGULAR explicitly: the 8pt family ships a single face, so bold/italic would resolve back to
    // regular anyway, and dropping the inherited EpdFontFamily::TRANSLATED bit keeps the row plain
    // black rather than the Interleaved gray.
    //
    // attachToPrevious MUST be carried across the re-emit, exactly as flushPartWordBuffer carries it
    // for the parser's own re-emit: the buffered translation is not one token per visual word (see
    // ParsedText::wordAttachesToPrevious), and a dropped flag renders every continuation boundary in
    // the span as a full space. The span's FIRST word is forced false — there is no previous word in
    // this row for it to attach to, so a span that happens to start mid-run does not open with a
    // stray glue.
    const bool attach = w > annotation.transStartWord && transBlock.wordAttachesToPrevious(w);
    annotationText.addWord(transBlock.wordAt(w), EpdFontFamily::REGULAR, /*underline=*/false, attach);
  }
  if (annotationText.isEmpty()) return;

  annotationText.layoutAndExtractLines(renderer, annotationFont, measureWidth,
                                       [&rows](const std::shared_ptr<TextBlock>& row) { rows.push_back(row); });
}

void ChapterHtmlSlimParser::emitInterlinearRow(const std::shared_ptr<TextBlock>& row, const int16_t xPos,
                                               const int rowHeight, const LineFontRole role, const bool breakIfNeeded) {
  // Only the degenerate path breaks here (see renderInterlinear). The break is gated on the page
  // holding a COMMITTED element, not merely on currentPageNextY > 0: the two differ on a page whose y
  // has been advanced by a paragraph's top spacing but which carries nothing yet, and breaking there
  // would serialize a blank page. It is also the stronger anti-loop guard the weaker test was added
  // for -- an empty page never breaks, so a row taller than the whole viewport always lands.
  if (breakIfNeeded && !currentPage->elements.empty() && currentPageNextY + rowHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // FOOTNOTES, attributed to the page carrying the anchor exactly as addLineToPage (:1828-1834) does
  // it. Interlinear needs its own copy because a PAIRED paragraph never reaches addLineToPage, and
  // under this layout essentially every paragraph pairs: without this, pendingFootnotes accumulated
  // for the whole chapter and was either dumped wholesale onto whatever page happened to be current at
  // the next unpaired paragraph (the end-of-block net, flushPendingFootnotesToCurrentPage) or never
  // delivered at all, since finishParse does not drain it.
  //
  // BODY rows only: an annotation row is synthetic and must not consume source word indices.
  //
  // Every entry here comes from an ORIGINAL block. A translated paragraph is written into the sidecar
  // as a fresh <p> holding nothing but the ESCAPED plain text of the translation
  // (TranslatingHtmlRewriter's write-out loop, via appendEscaped), so it carries no
  // <a epub:type="noteref"> and can never push a pending footnote — which is why one counter over the
  // source lines is the whole story here.
  //
  // The indices and this counter share the SOURCE block's base because makePagesInterlinearMode
  // installed that block's ledger (adoptBufferedFootnoteLedger) before calling renderInterlinear: both
  // the entries and the base below are the buffered original's own, and the in-flight block's are parked
  // out of reach. See the matching drain in renderSideBySide for why the old "startNewTextBlock zeroed
  // it" reasoning was not sound. The pre-layout anchor index vs post-layout wordCount() mismatch is
  // addLineToPage's own approximation, kept identical here.
  if (role == LineFontRole::Body) {
    wordsExtractedInBlock += row->wordCount();
    auto footnoteIt = pendingFootnotes.begin();
    while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
      currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
      ++footnoteIt;
    }
    pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);
  }

  auto pageLine = std::make_shared<PageLine>(row, xPos, currentPageNextY);
  // Both the source lines and the annotation rows carry the ORIGINAL paragraph's index, so the
  // line->paragraph mapping the overlays and the reader rely on still resolves (same as
  // renderSideBySide: currentBlockParagraphIdx has already advanced past the pair by now).
  pageLine->paragraphIdx = bufferedOriginalParagraphIdx;
  pageLine->fontRole = role;
  if (bufferedOriginalParagraphIdx >= 0) {
    if (currentPage->firstParagraphIdx < 0) {
      currentPage->firstParagraphIdx = bufferedOriginalParagraphIdx;
    }
    currentPage->lastParagraphIdx = bufferedOriginalParagraphIdx;
  }
  currentPage->elements.push_back(std::move(pageLine));
  currentPageNextY += rowHeight;
}

// Lay an original paragraph and its paired translation out as ONE full-width flow: the source breaks
// exactly as it would under Original Only, and each sentence's translation is emitted as small
// LineFontRole::Annotation rows immediately ABOVE the source line that sentence starts on,
// left-aligned to the sentence.
//
// GEOMETRY: every row is placed at the pre-advance currentPageNextY and advances by exactly its own
// box height — the same invariant addLineToPage documents — so the two type sizes tile edge to edge
// with no overlap and no gap, whatever each row's face is.
void ChapterHtmlSlimParser::renderInterlinear(std::unique_ptr<ParsedText> origBlock,
                                              std::unique_ptr<ParsedText> transBlock) {
  if (!origBlock || !transBlock) return;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int annotationFont = fontIdForRole(LineFontRole::Annotation);
  const BlockStyle& blockStyle = origBlock->getBlockStyle();
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const int16_t leftInset = blockStyle.leftInset();

  // STEP 1 — lay the SOURCE out first, unchanged. The annotation is computed AFTER breaking and
  // rendered on its own rows, so interlinear source line breaking is byte-identical to Original Only.
  std::vector<std::shared_ptr<TextBlock>> srcLines;
  srcLines.reserve(8);
  origBlock->layoutAndExtractLines(renderer, fontId, effectiveWidth,
                                   [&srcLines](const std::shared_ptr<TextBlock>& line) { srcLines.push_back(line); });
  if (srcLines.empty()) return;

  // v1 guards. RTL source: extractLine permutes words into VISUAL order for BiDi, so the flat
  // post-layout indices the anchoring depends on are not the logical sentence order — the paragraph
  // renders as plain source instead of being annotated in the wrong places.
  //
  // BOTH RTL flags are needed, because extractLine reorders on `isRtl || hasRtlWord`. blockStyle.isRtl
  // covers a wholly RTL paragraph; containsRtlWord() covers an LTR paragraph with an inline
  // Hebrew/Arabic span, which the paragraph probe (first RTL_PARAGRAPH_PROBE_WORDS words only) misses
  // and which would otherwise both mis-cut the sentence spans (terminators land in the wrong places in
  // a visually reordered word array) and anchor the row under the wrong word (wordXpos of a permuted
  // index). Both are final here: this runs after the layout above, which is what resolves isRtl.
  const bool annotate =
      interlinearPairFn != nullptr && !blockStyle.isRtl && !origBlock->containsRtlWord() && !transBlock->isEmpty();

  int annotationCount = 0;
  if (annotate) {
    // Sentence boundaries are found over the POST-layout words, not the pre-layout ones: hyphenation
    // INSERTS the remainder word into the block, so every pre-layout index would shift by one per
    // split. wordText() returns a NUL-terminated pointer into the line's arena, and srcLines holds
    // those lines alive for the whole call. This is the identical input shape the Tooltip overlay
    // already splits (a trailing "dun-" is not a terminator, so a hyphen fragment adds no boundary).
    size_t totalSrcWords = 0;
    for (const auto& line : srcLines) totalSrcWords += line->wordCount();
    std::vector<const char*> srcWordPtrs;
    srcWordPtrs.reserve(totalSrcWords);
    for (const auto& line : srcLines) {
      for (uint16_t w = 0; w < line->wordCount(); w++) srcWordPtrs.push_back(line->wordText(w));
    }

    // The translation comes straight out of the buffered block in LOGICAL order — it is never laid
    // out, so no hyphenation or BiDi reordering touched it and the mapping is purely textual.
    std::vector<const char*> transWordPtrs;
    transWordPtrs.reserve(transBlock->size());
    for (size_t w = 0; w < transBlock->size(); w++) transWordPtrs.push_back(transBlock->wordAt(w).c_str());

    if (interlinearAnnotations.empty()) interlinearAnnotations.resize(INTERLINEAR_MAX_ANNOTATIONS);
    if (!srcWordPtrs.empty() && !transWordPtrs.empty()) {
      annotationCount = interlinearPairFn(srcWordPtrs.data(), static_cast<int>(srcWordPtrs.size()),
                                          transWordPtrs.data(), static_cast<int>(transWordPtrs.size()),
                                          interlinearAnnotations.data(), INTERLINEAR_MAX_ANNOTATIONS);
    }
  }

  // Top spacing comes from the original block, before the paragraph's first annotation row, so that
  // row sits below the margin rather than inside it. It COLLAPSES at the very top of a page: there is
  // nothing above it there for the margin to separate the paragraph from, and it is the ONLY way
  // currentPageNextY can be > 0 on a page with no committed element -- which is precisely the state
  // that would make the atomic fit test below complete a BLANK page (a forced break, e.g. a TOC
  // anchor in flushPendingAnchor, leaves exactly such a page). Collapsing here is also what the
  // common case already does implicitly: when the previous paragraph ended at the page bottom, this
  // spacing is added to the OLD page's y and thrown away with it by the break below.
  if (!currentPage->elements.empty()) {
    if (blockStyle.marginTop > 0) currentPageNextY += blockStyle.marginTop;
    if (blockStyle.paddingTop > 0) currentPageNextY += blockStyle.paddingTop;
  }

  const int annotationRowHeight = renderer.getLineHeight(annotationFont, lineCompression);
  const int bodyLineHeight = renderer.getLineHeight(fontId, lineCompression);
  const int bodyAscender = renderer.getFontAscenderSize(fontId);

  int nextAnnotation = 0;  // annotations arrive in ascending sourceStartWord order
  uint16_t lineFirstWord = 0;
  std::vector<std::shared_ptr<TextBlock>> annotationRows;
  annotationRows.reserve(4);

  for (size_t i = 0; i < srcLines.size(); i++) {
    const std::shared_ptr<TextBlock>& srcLine = srcLines[i];
    const uint16_t lineWordCount = srcLine->wordCount();
    // Unchanged source pitch, ruby shift included, so furigana headroom survives on annotated lines.
    const int srcRowHeight = bodyLineHeight + srcLine->getRubyShift(bodyAscender);

    // Every group whose FIRST sentence starts on this line. Annotation blocks carry no ruby, so their
    // own shift is zero and their pitch is just the 8pt line height.
    annotationRows.clear();
    while (nextAnnotation < annotationCount &&
           interlinearAnnotations[nextAnnotation].sourceStartWord < lineFirstWord + lineWordCount) {
      const InterlinearAnnotation& annotation = interlinearAnnotations[nextAnnotation];
      if (annotation.sourceStartWord >= lineFirstWord) {
        const uint16_t wordOnLine = static_cast<uint16_t>(annotation.sourceStartWord - lineFirstWord);
        buildAnnotationRows(annotation, *transBlock, srcLine->wordXpos(wordOnLine), effectiveWidth, annotationFont,
                            annotationRows);
      }
      nextAnnotation++;
    }

    const int groupHeight = static_cast<int>(annotationRows.size()) * annotationRowHeight + srcRowHeight;
    const bool degenerate = groupHeight > viewportHeight;
    // ATOMIC FIT: test the WHOLE group ONCE, before its first row. If the group fits at the tested y
    // then every row inside it fits by construction, so no row re-tests and an annotation can never
    // be split from the source line it belongs to.
    //
    // The !elements.empty() guard is the one emitHorizontalRule already uses: an EMPTY page must never
    // be completed or it reaches section.bin as a blank page the reader then displays. With the top
    // spacing collapsed above, an empty page here always has currentPageNextY == 0, so a non-degenerate
    // group fits by definition and the guard costs nothing in the normal case.
    if (!degenerate && !currentPage->elements.empty() && currentPageNextY + groupHeight > viewportHeight) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
    // A monster sentence whose annotation wraps past a whole page can never satisfy the atomic test,
    // so it falls back to per-row break checks. Without this the fit test would never succeed.
    for (const auto& row : annotationRows) {
      emitInterlinearRow(row, leftInset, annotationRowHeight, LineFontRole::Annotation, degenerate);
    }
    emitInterlinearRow(srcLine, leftInset, srcRowHeight, LineFontRole::Body, degenerate);
    // The page owns this line now, so release our reference instead of pinning every line of the
    // paragraph until the call returns. Nothing below reads an earlier line: the flat srcWordPtrs
    // array died with the annotate block above, the annotations hold indices rather than pointers, and
    // this loop only ever touches the CURRENT line's wordXpos / wordCount. That restores the makePages
    // peak -- one page's worth of TextBlocks, freed as onPageComplete serializes each page -- for a
    // paragraph long enough to span several pages, which is exactly the shape this layout produces
    // most of (see the +40% pages estimate in PtLayout.h).
    srcLines[i].reset();

    lineFirstWord = static_cast<uint16_t>(lineFirstWord + lineWordCount);
  }

  // Same end-of-block net makePages keeps: every entry in the installed ledger belongs to the SOURCE
  // paragraph just emitted (the caller parked the in-flight block's ledger before this call), so
  // flushing it to the current page can never touch another paragraph's entry. The per-row drain
  // already covers the normal case — an anchor index can never exceed the block's pre-layout word
  // count, and hyphenation only ADDS post-layout words — so this fires only when a line was dropped (a
  // TextBlock arena OOM).
  flushPendingFootnotesToCurrentPage();

  // Bottom spacing + the usual half-line paragraph gap after the pair.
  if (blockStyle.marginBottom > 0) currentPageNextY += blockStyle.marginBottom;
  if (blockStyle.paddingBottom > 0) currentPageNextY += blockStyle.paddingBottom;
  if (extraParagraphSpacing) {
    currentPageNextY += bodyLineHeight / 2;
  }
}
