#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <Epub/parsers/ParagraphBoundary.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "fontIds.h"
#include "modules/lingua/services/TranslatedContentDetector.h"

// Lingua: settle the translation state and paragraph index of a block that was opened by <br>.
//
// A <br/> carries no lang= attribute, so startElement's block-open stamping has nothing to classify
// the block it creates by -- and Calibre's translation plugin emits exactly that shape:
//
//   <p>Original sentence.<br/><span lang="uk">Translated sentence.</span></p>
//
// The <span> opens AFTER the break, so the block's language only becomes knowable once its first
// word arrives. flushPartWordBuffer calls this on every word; everything but the first word of a
// <br>-opened block falls out on the guard below.
//
// The stamping goes through claimParagraphIndex, the single owner of the paragraph-index rule, so
// this context cannot drift from the others. Runs before the word-drop filter, so the block is
// classified even under a layout that drops its words.
void ChapterHtmlSlimParser::classifyBrOpenedBlock() {
  // Only the FIRST word of a still-empty <br>-opened block: partWordBufferIndex > 0 means a real
  // word is about to land (an empty flush must not consume a paragraph index), and isEmpty() stops
  // the second and later words of the same block from re-stamping it.
  if (partWordBufferIndex == 0 || !currentTextBlock || !currentTextBlock->isEmpty()) return;
  if (!currentTextBlock->getBlockStyle().fromBrElement) return;

  claimParagraphIndex(!inlineStyleStack.empty() && inlineStyleStack.back().isTranslatedBlock);
}

// Lingua: layout-based block filtering, shared by flushPartWordBuffer and the ruby
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
  switch (linguaLayout) {
    case LinguaLayout::OriginalOnly:
      return inTranslatedBlock;
    case LinguaLayout::TranslationOnly:
      return !inTranslatedBlock;
    case LinguaLayout::Both:
    case LinguaLayout::SideBySide:
    case LinguaLayout::Interlinear:
      return false;
  }
  return false;  // unreachable: every enumerator returns above
}

// Lingua: which role the lines of the block currently being laid out carry. Only one
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
//                  pairing), so nothing that reaches THIS function is tagged. The paired path does
//                  tag its translation column LineFontRole::Translation, but for COLOUR, not for a
//                  font — the translation slot is 0 under this mode, so the role resolves back to
//                  the body font (see renderSideBySide). Two paths reach here under SideBySide and
//                  both are main-flow text, hence Body: the soft-flush escape, and the full-width
//                  fallback makePagesTableMode takes for a translation with no original to pair
//                  with. KNOWN GAP, benign: translated text arriving either way is outside the
//                  Translation Colour row's reach and draws black like the source. Black is the
//                  row's own default, so it is never a colour the user did not pick; tagging these
//                  Translation would close it (the font resolves to Body either way) at the cost of
//                  making a rare fallback path carry a role its comment says it must not.
//   Interlinear    the translated text does NOT flow inline at all: renderInterlinear re-emits it
//                  into its own small rows tagged LineFontRole::Annotation, which resolve through the
//                  annotation slot instead. Source lines therefore stay Body. The translated block
//                  normally never reaches here — it is consumed by the pairing, not flushed through
//                  makePages — with ONE exception: characterData's soft flush is a mid-block escape
//                  that bypasses the pairing, and it answers Body for a translated block, i.e. the
//                  translation at full body size in the main flow. That is why the soft flush holds
//                  a paired layout to the base ceiling and treats a flush there as an OOM backstop
//                  rather than a routine chunking step (see the threshold comment there).
//
// Reads the block-level translated flag, not the per-word inline-stack one wordIsFiltered() uses: a
// role applies to a whole laid-out line, and currentBlockIsTranslated is exactly the granularity of
// the block that is being flushed (see the makePagesTableMode comment for why it is still valid
// here).
LineFontRole ChapterHtmlSlimParser::currentLineRole() const {
  // No distinct translation font configured: every line is Body, i.e. exactly the pre-existing
  // layout, and fontIdForRole would resolve Translation back to fontId anyway.
  if (translationFontId == 0 || !currentBlockIsTranslated) return LineFontRole::Body;
  switch (linguaLayout) {
    case LinguaLayout::Both:
      return LineFontRole::Translation;
    case LinguaLayout::OriginalOnly:
    case LinguaLayout::TranslationOnly:
    case LinguaLayout::SideBySide:
    case LinguaLayout::Interlinear:
      break;
  }
  return LineFontRole::Body;
}
