#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include "Epub/TranslationDetection.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "ParagraphAnchor.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// SECTION FILE FORMAT VERSION
//
// READ THIS BEFORE COMPARING THE NUMBER TO ANYTHING OUTSIDE THIS FORK.
// This fork's format at version 34 is NOT upstream develop's format at any version. Our header
// carries four fields upstream's does not have at all -- translationFontId, annotationFontId, the
// PtLayout byte, and the translatedSource / embeddedTranslation pair -- so an upstream-written .bin
// and one of ours are mutually unreadable whatever number either stamps. The version byte is
// therefore only meaningful WITHIN this fork: it is a cache key for our own files, never a
// compatibility claim against upstream. Nothing may treat "34" as a portable format identifier, and
// no cross-fork cache sharing is possible or intended. (The .crosspoint cache is device-local and
// rebuilt on demand, so this costs a re-layout, not correctness -- but only as long as nobody tries
// to make the number mean more than it does.)
//
// WHY 34 AND NOT 42. This line once ran 33 -> 41 across internal iterations that were never
// released, then was collapsed back to 33 (upstream develop's value) because no build carrying
// 34..41 ever left this fork. 34 is now claimed for the FIRST format change since that rewind: the
// Interlinear line-parity rewrite. Do not "resume" at 42.
//
// WHY 34 EXISTS AT ALL, given the byte layout did not move. Rule 2 below: the version IS the cache
// key, and Interlinear's page CONTENT changed completely -- the source no longer breaks a line at
// every sentence, and each source line now carries exactly one annotation strip instead of a
// variable stack over the sentence's first line. Nothing else in the key moved, so without this bump
// a device holding pages built by the sentence-per-line model would serve them forever and the
// rewrite would appear to have done nothing.
//
// ---------------------------------------------------------------------------------------------
// THE FORMAT AT 34
//
// The header, in write order (see writeSectionFileHeader; loadSectionFile reads it back in exactly
// this order, and HEADER_SIZE below is the sum):
//   version:uint8, fontId:int, translationFontId:int, annotationFontId:int, lineCompression:float,
//   extraParagraphSpacing:bool, paragraphAlignment:uint8, viewportWidth:uint16,
//   viewportHeight:uint16, hyphenationEnabled:bool, embeddedStyle:bool, ptLayout:uint8,
//   translatedSource:bool, embeddedTranslation:bool, imageRendering:uint8,
//   focusReadingEnabled:bool, pageCount:uint16, then four uint32 LUT offsets.
// Every field except embeddedTranslation is part of the cache key.
//
// The substantive properties this format has accumulated, and why each one is load-bearing:
//
// * Word/text serialization. wordStyles carry a line-through bit. TextBlock stores its words as one
//   flat arena (offset table + NUL-terminated blob) rather than length-prefixed strings and
//   per-field arrays. CJK text split on MAX_WORD_SIZE preserves word continuation. Arabic text is
//   measured as SHAPED visual text (getTextAdvanceX), so cached word positions match what drawText
//   actually renders. TextBlock also serializes a per-word ruby annotation string after the word
//   arena (native <ruby>/<rt>, <rp> skipped), and a closing tag starts a fresh text block so a
//   closed block's style cannot leak into following bare text. The persisted ruby style bit is 128
//   (bit 7); 64 is this line's TRANSLATED bit, which is why ruby could not keep upstream's value.
// * Images. ImageBlock serializes the book-internal source href after the cache path: images are
//   header-probed at build time and extracted lazily on first render.
// * Pages and lines. Each line carries a paragraphIdx and a LineFontRole byte, and each page a
//   paragraph range, so a page can mix body text with smaller annotation text and a reposition
//   anchor can be resolved without re-parsing.
// * Hyphenation is per block, not per book. A translated block (lang= differing from the book
//   language) hyphenates with its own script's rules, so a bilingual en->uk book's translated text
//   breaks correctly. Hyphenated splits are baked into the serialized pages, which is why changing
//   this rule is always a format change.
// * Pre-Translation stores the LAYOUT, not the display mode. The header holds a PtLayout byte
//   (Both / OriginalOnly / TranslationOnly / SideBySide / Interlinear) rather than the raw user
//   mode, so modes whose pages are byte-identical -- Normal vs Interleaved (a gray level, drawn not
//   laid out), Original Only vs Page Translation vs Tooltip (overlays composited at view time) --
//   share one cache entry and switching between them is instant. PtLayout.h states the rule: the
//   byte is part of the cache key, so adding a value to the enum is itself a format change.
//   - SideBySide lays original and translation into two half-width columns (renderSideBySide),
//     reusing the existing per-line xPos field rather than adding structure.
//   - Interlinear pairs EVERY source line with exactly one small LineFontRole::Annotation strip
//     directly above it, so a page alternates strip, source, strip, source with no doubles and no
//     gaps. The source itself is broken exactly as OriginalOnly would break it -- sentence starts
//     constrain nothing, they are only REPORTED back by layout (line, x, starts-line) so a
//     sentence's translation can begin under its own first word and then flow through the strips
//     above that sentence's remaining source lines. A source line holding the tail of one sentence
//     and the head of the next carries two annotation PageLines at ONE yPos, the shape SideBySide
//     already writes for its columns. A blank strip emits no PageLine at all and only advances y.
//     The pairing runs over the focus-MERGED word stream rather than the raw token array, so Focus
//     Reading -- which stores each word as a bold prefix plus a regular tail -- cannot move a
//     sentence boundary.
// * Fonts used for translated and annotation text are keyed. translationFontId and annotationFontId
//   are real layout inputs: they decide how translated text and annotation rows measure, wrap and
//   how tall they are. Both are keyed conditionally, via keyedTranslationFontId() /
//   keyedAnnotationFontId(), so a font that cannot reach the page under the current layout does not
//   pointlessly invalidate the cache. loadSectionFile normalizes the lookup the same way.
// * translatedSource records whether the HTML these pages were laid out from CONTAINED
//   TRANSLATIONS -- from either source: a reader-produced `.translated.html` sidecar, or
//   translations embedded in the chapter's own XHTML (a Calibre-plugin bilingual book interleaves
//   `<p lang="uk">` after each `lang="en"` original; there is no sidecar and no marker attribute).
//   The PtLayout byte cannot express this: Both is stamped both by an untranslated chapter and by
//   one that simply requested Normal. Without the flag, a chapter laid out before its translation
//   arrived would stay a cache HIT afterwards and silently serve untranslated pages in a bilingual
//   mode -- and, symmetrically, a translated cache would survive the translation being deleted.
//   The language comparison behind it is translationdetect::isTranslatedLangTag: primary subtag,
//   ASCII case-insensitive, with `-` and `_` both ending the subtag, so `uk-UA` in an `en` book is
//   translated while `en-GB` is not. That predicate is shared with ChapterHtmlSlimParser, so the
//   gate that enables a bilingual layout and the engine that renders it can never disagree.
// * embeddedTranslation is the ONLY field that is not a cache key. It memoizes the half of
//   translatedSource that is IMMUTABLE for a given book file -- "the chapter's own XHTML embeds
//   translated blocks" -- so a load can recompute translatedSource as
//   `hasTranslatedSidecar() || embeddedTranslation`, one SD stat, instead of SAX-scanning the
//   chapter HTML on every chapter load. Without it that scan would tax every reader, including
//   those who never enable a translation mode. It is stamped false by a build that read the
//   SIDECAR, which never looked at the chapter HTML and so does not know; that state is
//   recognisable as `translatedSource == true && embeddedTranslation == false`, and a load seeing
//   it with the sidecar now gone treats the answer as unknown rather than memoizing a value that
//   could understate the truth.
//
// ---------------------------------------------------------------------------------------------
// TWO RULES THIS HISTORY PAID FOR
//
// 1. NEVER change the byte layout without changing the number, even if a mismatch check "would
//    catch it". Two different header layouts were once written under the number 38 during
//    development -- first without translatedSource, then with it. That byte sits in the MIDDLE of
//    the header, so the earlier layout passes the version gate and then every field after the
//    PtLayout byte is read shifted by one. The parameter-mismatch check usually catches that as a
//    stale key and rebuilds, but it is NOT guaranteed to (shifted bytes can compare equal), and the
//    pageCount and LUT offsets that follow are consumed BEFORE any such check can help. Any
//    mid-header insertion makes a bump mandatory rather than merely correct.
// 2. A pure layout change with no byte change still needs a bump, because the version IS the cache
//    key and nothing else in the key moved. A device holding the old entries would otherwise serve
//    the old pages forever. Note this invalidates every cached chapter of every book for EVERY
//    layout, not just the one that changed -- the key is a single number, so the blast radius is
//    always total. That is the accepted cost: one background re-layout per book on next open.
//
// The invariant behind translatedSource is the sharpest case of rule 1: a chapter cached while it
// had no translation must never be served once it has one, and vice versa. Both transitions
// invalidate -- a downloaded or deleted sidecar flips the byte, and an embedded translation is
// baked into the chapter HTML, so a book that gains one is a different file with a different cache
// directory.
constexpr uint8_t SECTION_FILE_VERSION = 34;
// Written into the version field while a build is in progress; patched to
// SECTION_FILE_VERSION only when the build is finalized. An abandoned /
// crash-interrupted .bin therefore carries version 0, which loadSectionFile rejects
// as unknown and clears -- so an incomplete file is never mistaken for a valid one.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Written when a build is suspended partway (reader exited or device slept mid-build).
// The file carries valid pages 0..pageCount-1, all LUTs, and a trailer with the parse
// watermark (bytesConsumed, totalBytes) appended after the li LUT. loadSectionFile
// accepts it so a resume shows those pages instantly; the reader extends it by
// rebuilding in the background. Uses the same header layout as SECTION_FILE_VERSION,
// so finalized files are untouched by this feature; older firmware treats the sentinel
// as an unknown version and rebuilds, which is a safe downgrade.
// MUST change in lockstep with SECTION_FILE_VERSION: the sentinel IS the partial's
// format version, so a stale-format partial otherwise passes the header check and
// only fails (noisily, via the block-decode error path) when a page is loaded.
// Derived so the pairing can't be forgotten: 0xFE for v28, 0xFD for v29, ... 0xF8 for v34.
// The derivation walks DOWN as the version walks up, so rewinding the version walks it back up:
// the 33 -> 41 iterations put it at 0xF1, collapsing back to 33 returned it to 0xF9, 34 took it to
// 0xF8 and the former v35 to 0xF7. A partial left on a card by any of those builds carries a sentinel this
// firmware does not recognise, so it is rejected as an unknown version and rebuilt -- the same
// one-off invalidation a version bump costs finalized files.
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
// The derivation only stays a distinct sentinel while the two ranges have not met; assert it rather
// than trusting a future bump to notice. At 34 the sentinel is 0xF8 (248): comfortably above the
// version, and nothing but a version past 0xF8-28 = 220 could ever close the gap.
static_assert(SECTION_FILE_PARTIAL_VERSION > SECTION_FILE_VERSION &&
                  SECTION_FILE_PARTIAL_VERSION != SECTION_FILE_INCOMPLETE_VERSION,
              "Partial sentinel collides with a real version");
// The second sizeof(int) is the Pre-Translation translationFontId and the third is the
// annotationFontId; the extra sizeof(uint8_t) after the two bools is the Pre-Translation PtLayout
// byte, the sizeof(bool) right after it is the translated-source flag, and the sizeof(bool) after
// THAT is the embedded-translation memo.
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(int) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

// The translation font belongs in the cache key only where translated words are actually laid out IN
// IT, which is the Both layout and only Both -- the one layout that flows the two languages inline,
// so a second type size there re-breaks lines (see ChapterHtmlSlimParser::currentLineRole, which
// tags a line for that font under exactly this layout and no other). Under OriginalOnly the
// translated words are all dropped, and under TranslationOnly / SideBySide they are laid out in the
// BODY font by design, so in all three no line was measured in the translation font and no value of
// it can move a line break. Stamping the real id there would make a change to the Interleaved mode's
// Translation Size invalidate every cached chapter of the book, including chapters of a mode that
// lays out no differently-sized text at all. (The two modes that MAP to OriginalOnly, Tooltip and
// Page Translation, own SEPARATE size settings which are composited at view time and by design never
// reach a ReaderRenderSpec -- see CrossPointSettings::TRANSLATION_SIZE. This normalization covers the
// id that does reach one.)
//
// Keyed off the EFFECTIVE (post-fallback) layout, not the requested one, and applied to the header
// write, the lookup AND the id handed to the parser, so key and layout can never disagree: a chapter
// with no committed translation is laid out as Both even in Tooltip mode (Section::effectiveLayout),
// and keyedTranslationFontId keeps the real id for it exactly as it would for an actually-translated
// chapter. That is NOT a don't-care: ChapterHtmlSlimParser::currentBlockIsTranslated is a lang=
// mismatch against the book's primary language, not "this chapter has a committed Pre-Translation" --
// so the chapter's OWN original HTML can carry lang-tagged blocks (a foreign epigraph, a quoted
// phrase) that currentLineRole() still tags LineFontRole::Translation under Both and lays out in this
// font. Zeroing the id here for an untranslated chapter would let a later change to it (e.g. the
// Interleaved mode's Smaller Translation Size) silently keep serving those blocks from a stale cache
// at the wrong size instead of invalidating it.
constexpr int keyedTranslationFontId(const int translationFontId, const PtLayout effectiveLayout) {
  return effectiveLayout == PtLayout::Both ? translationFontId : 0;
}

// Same rule, same reasoning, for the annotation font: it only reaches a page under the ONE layout
// that emits LineFontRole::Annotation rows (Interlinear), where it decides both the row's wrap and
// its pitch. Under every other layout no line is measured in it, so stamping the real id there would
// let a future annotation-size row invalidate every cached chapter of a mode that draws no
// annotations at all. Applied in all three places the translation one is -- the header write, the
// lookup compare and the id handed to the parser -- so the key and the layout can never disagree.
constexpr int keyedAnnotationFontId(const int annotationFontId, const PtLayout effectiveLayout) {
  return effectiveLayout == PtLayout::Interlinear ? annotationFontId : 0;
}
}  // namespace

// Out-of-line so the unique_ptr<ChapterHtmlSlimParser> in BuildContext can be
// constructed/destroyed where the parser's full definition is visible.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", builtPageCount_);

  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

void Section::writeSectionFileHeader(const ReaderRenderSpec& spec, const bool translatedSource,
                                     const bool embeddedTranslation) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.translationFontId) +
                                   sizeof(spec.annotationFontId) + sizeof(spec.lineCompression) +
                                   sizeof(spec.extraParagraphSpacing) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) + sizeof(pageCount) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(uint8_t) /* PtLayout */ + sizeof(translatedSource) +
                                   sizeof(embeddedTranslation) + sizeof(spec.imageRendering) +
                                   sizeof(spec.focusReadingEnabled) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  // Written as the incomplete sentinel; finalizeBuild() patches it to
  // SECTION_FILE_VERSION as the last step, committing the file.
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, spec.fontId);
  serialization::writePod(file, spec.translationFontId);  // Pre-Translation translated-text font (cache key)
  // Pre-Translation annotation-row font (cache key). Non-zero only under PtLayout::Interlinear.
  serialization::writePod(file, spec.annotationFontId);
  serialization::writePod(file, spec.lineCompression);
  serialization::writePod(file, spec.extraParagraphSpacing);
  serialization::writePod(file, spec.paragraphAlignment);
  serialization::writePod(file, spec.viewportWidth);
  serialization::writePod(file, spec.viewportHeight);
  serialization::writePod(file, spec.hyphenationEnabled);
  serialization::writePod(file, spec.embeddedStyle);
  serialization::writePod(file, static_cast<uint8_t>(spec.ptLayout));  // Pre-Translation page layout (cache key)
  // Whether the HTML these pages were laid out from contained translations (cache key). The layout
  // byte cannot express it: Both is stamped both by an untranslated chapter and by one that simply
  // requested Normal.
  serialization::writePod(file, translatedSource);
  // NOT a cache key -- a memo, so the next load can recompute translatedSource without
  // re-scanning the chapter HTML. It records the half of the answer that is IMMUTABLE for a given
  // book file: "the chapter's own XHTML embeds translated blocks". The other half (a sidecar) is one
  // SD stat, so `translatedSource == hasTranslatedSidecar() || embeddedTranslation` costs a stat
  // instead of a whole-file SAX scan on every chapter load -- including for the many readers who
  // never enable a translation mode at all.
  // Stamped false when this build read the SIDECAR (it never looked at the chapter HTML, so it does
  // not know). That is the safe way to be wrong: it can only make a later load recompute a smaller
  // translatedSource and rebuild, never serve the wrong pages.
  serialization::writePod(file, embeddedTranslation);
  serialization::writePod(file, spec.imageRendering);
  serialization::writePod(file, spec.focusReadingEnabled);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }
    filePartial = (version == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId;
    int fileTranslationFontId;
    int fileAnnotationFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t filePtLayout;
    bool fileTranslatedSource;
    bool fileEmbeddedTranslation;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileTranslationFontId);
    serialization::readPod(file, fileAnnotationFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, filePtLayout);
    serialization::readPod(file, fileTranslatedSource);
    serialization::readPod(file, fileEmbeddedTranslation);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);

    // Match on the EFFECTIVE (post-fallback) layout: an untranslated chapter is laid out and its
    // header stamped with the Both layout by startBuild(), so it must also be looked up under Both
    // -- otherwise a filtering layout never matches the Both-stamped cache and rebuilds on every
    // visit. See effectiveLayout().
    //
    // The source flag is the other half of the Pre-Translation key. Both is stamped by an
    // untranslated chapter AND by a chapter that simply requested Normal, so without this compare a
    // chapter laid out before its translation was downloaded would stay a cache HIT afterwards and
    // serve untranslated pages in a bilingual mode (and, symmetrically, a translated cache would
    // survive the translation being deleted). Resolved once per Section (memoized inside
    // hasTranslation()) and shared with the layout resolution below.
    // Recomputed from the two independent halves rather than by re-scanning the chapter HTML: the
    // embedded half is immutable for a given book file, so the previous build's memo is still valid,
    // and the sidecar half is one SD stat. That keeps every chapter load -- Normal mode included --
    // at the single stat this check has always cost.
    const bool hasSidecar = hasTranslatedSidecar();
    const bool translatedSource = hasSidecar || fileEmbeddedTranslation;
    // ...but only ADOPT it as the memoized answer when it is exact. A build that read the sidecar
    // never looked at the chapter HTML, so it stamped embedded=false without knowing -- recognisable
    // as "translatedSource stamped true while embedded stamped false". If that sidecar has since been
    // deleted, `translatedSource` above understates the truth. It still forces the right outcome (a
    // key MISS, hence a rebuild -- which is required anyway, the source file changed), but it must
    // NOT be memoized, or the rebuild would trust it instead of scanning and would stamp a plugin-
    // translated chapter as untranslated.
    const bool embeddedIsKnown = fileEmbeddedTranslation || !fileTranslatedSource;
    if (hasSidecar || embeddedIsKnown) {
      translationPresence_ = translatedSource ? TranslationPresence::Yes : TranslationPresence::No;
    }
    // Record the source before the key check: on a HIT it is the source these pages came from, and
    // on a MISS it is superseded by the rebuild below. Either way it is what a caller recording a
    // reposition anchor needs, without a second SD stat.
    translatedSource_ = translatedSource;
    const PtLayout layout = effectiveLayout(spec.ptLayout, translatedSource);
    if (spec.fontId != fileFontId || keyedTranslationFontId(spec.translationFontId, layout) != fileTranslationFontId ||
        keyedAnnotationFontId(spec.annotationFontId, layout) != fileAnnotationFontId ||
        spec.lineCompression != fileLineCompression || spec.extraParagraphSpacing != fileExtraParagraphSpacing ||
        spec.paragraphAlignment != fileParagraphAlignment || spec.viewportWidth != fileViewportWidth ||
        spec.viewportHeight != fileViewportHeight || spec.hyphenationEnabled != fileHyphenationEnabled ||
        spec.embeddedStyle != fileEmbeddedStyle || translatedSource != fileTranslatedSource ||
        static_cast<uint8_t>(layout) != filePtLayout || spec.imageRendering != fileImageRendering ||
        spec.focusReadingEnabled != fileFocusReadingEnabled) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);

  if (filePartial) {
    // A partial's pageCount is the watermark of a suspended build. Read the watermark
    // trailer (appended after the li LUT) so estimatedTotalPages can extrapolate.
    uint32_t liLutOffset = 0;
    file.seek(HEADER_SIZE - sizeof(uint32_t));
    serialization::readPod(file, liLutOffset);
    const uint32_t trailerOffset = liLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint16_t);
    const bool trailerValid =
        pageCount > 0 && liLutOffset >= HEADER_SIZE && trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      clearCache();
      pageCount = 0;
      return false;
    }
    file.seek(trailerOffset);
    serialization::readPod(file, partialBytesConsumed_);
    serialization::readPod(file, partialTotalBytes_);
    partial_ = true;
    partialPageCount_ = pageCount;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages%s", pageCount, filePartial ? " (partial)" : "");
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }

  // Remove a stale partial translation if one was left behind by an interrupted run. The
  // completed translated HTML is intentionally preserved so that translations survive .bin
  // cache invalidation (font/size changes, re-layout). Only the ".part" is transient: the
  // translator writes there and renames into place atomically on a clean, complete write, so a
  // lingering ".part" always means an aborted run and is never a source to lay out from.
  const auto partPath = getTranslatedHtmlPath() + ".part";
  if (Storage.exists(partPath.c_str())) {
    Storage.remove(partPath.c_str());
  }

  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

std::string Section::getTranslatedHtmlPath() const {
  return epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".translated.html";
}

std::string Section::getCachedHtmlPath() const {
  return epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
}

bool Section::hasTranslatedSidecar() const {
  // The translated HTML is committed atomically: it is written to a ".part" file and only
  // renamed into place after a clean, complete write. So a finished translation is exactly
  // "the final file exists" — a power loss mid-translation leaves only a ".part", never a
  // truncated final file. This also keeps pre-existing translated caches (written before the
  // atomic-commit change) valid, so users are never forced to re-translate. The stale ".part"
  // itself is reclaimed by clearCache() on the next .bin invalidation.
  return Storage.exists(getTranslatedHtmlPath().c_str());
}

bool Section::hasTranslation() const {
  if (translationPresence_ != TranslationPresence::Unknown) {
    return translationPresence_ == TranslationPresence::Yes;
  }
  // A committed sidecar IS a translation by construction; no scan needed, and this keeps the
  // reader-translated path at exactly the single SD stat it always cost.
  if (hasTranslatedSidecar()) {
    translationPresence_ = TranslationPresence::Yes;
    return true;
  }
  // No sidecar: the translation, if any, is embedded in the chapter's own XHTML. That needs the
  // unzipped HTML, which is cached per book and outlives every .bin invalidation -- so any chapter
  // that has ever been built answers from disk here.
  const std::string htmlPath = getCachedHtmlPath();
  if (Storage.exists(htmlPath.c_str())) {
    translationPresence_ = translationdetect::htmlHasTranslatedBlock(htmlPath, epub->getLanguage())
                               ? TranslationPresence::Yes
                               : TranslationPresence::No;
    return translationPresence_ == TranslationPresence::Yes;
  }
  // Not knowable without inflating the spine, which is not this function's call to make. Stay
  // Unknown (so the next call re-resolves) and answer in the safe direction -- see Section.h.
  return true;
}

void Section::resolveTranslationPresence() {
  if (translationPresence_ != TranslationPresence::Unknown) return;
  // Try the free routes first (sidecar stat, or a scan of an already-unzipped chapter HTML). NOT
  // via its return value: hasTranslation() answers true while Unknown, so only the memo says
  // whether it actually resolved anything.
  (void)hasTranslation();
  if (translationPresence_ != TranslationPresence::Unknown) return;
  // Only reachable with no sidecar and no cached chapter HTML. Inflate it -- the same inflate a
  // build of this chapter pays, promoted to the same cache startBuild() then reuses, so this
  // hoists the cost rather than adding one.
  std::string parsePath;
  bool promoted = false;
  if (!ensureChapterHtml(parsePath, promoted)) {
    LOG_DBG("SCT", "Could not inflate spine %d to resolve translation presence", spineIndex);
    return;  // stays Unknown -> hasTranslation() keeps answering in the safe direction
  }
  translationPresence_ = translationdetect::htmlHasTranslatedBlock(parsePath, epub->getLanguage())
                             ? TranslationPresence::Yes
                             : TranslationPresence::No;
  if (!promoted) {
    // An un-promoted temp is nobody's to keep: startBuild() would re-inflate it under its own
    // ownership rules, and leaving it would strand a stale ".tmp_<n>.html" in the cache dir.
    Storage.remove(parsePath.c_str());
  }
}

PtLayout Section::effectiveLayout(const PtLayout requested, const bool translatedSource) {
  // With no translation in the source there are no translated words to drop, keep or pair, so EVERY
  // layout degrades to Both -- which on an untranslated chapter is just the plain original. Both is
  // therefore both the request and the fallback, which is exactly why it says nothing about the
  // source the pages came from and why the caller pairs this with the translatedSource flag in the
  // cache key. See the declaration comment in Section.h.
  return translatedSource ? requested : PtLayout::Both;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  // One-shot build: start, then lay out the whole section in a single pass.
  if (!startBuild(spec, popupFn)) {
    return false;
  }
  if (!buildSomeMore(0)) {  // 0 = build to completion
    return false;
  }
  return buildComplete_;
}

bool Section::ensureChapterHtml(std::string& outParsePath, bool& outPromoted) {
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = getCachedHtmlPath();
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";

  if (Storage.exists(htmlPath.c_str())) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
    outParsePath = htmlPath;
    outPromoted = true;
    return true;
  }

  Storage.mkdir(htmlDir.c_str());

  // Retry logic for SD card timing issues
  bool streamed = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    // Larger chunks mean far fewer SD writes inflating the HTML; a 1KB chunk turned a 584KB
    // single-spine novel into ~570 tiny writes (multi-second). 8KB keeps the transient buffers
    // small while cutting the write count 8x.
    streamed = epub->readItemContentsToStream(epub->getSpineItem(spineIndex).href, tmpHtml, 8192);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!streamed) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  // Promote to the persistent HTML cache immediately -- the inflate is complete and the bytes are
  // valid regardless of whether the layout build finishes, so reopening (even a window-only spine
  // that never finalizes its .bin) skips re-inflation. If the rename fails we just parse the temp.
  if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
    outParsePath = htmlPath;
    outPromoted = true;
  } else {
    LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    outParsePath = tmpHtmlPath;
    outPromoted = false;
  }
  return true;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }
  buildComplete_ = false;
  builtPageCount_ = 0;
  // Pages from a loaded partial stay readable (from filePath) while this build writes
  // to the tmp .bin, so availability never drops below the partial's watermark.
  pageCount = partial_ ? partialPageCount_ : 0;

  // Remove a stale tmp .bin from a crash-interrupted build; this build recreates it.
  {
    const std::string staleTmp = binTmpPath();
    if (Storage.exists(staleTmp.c_str())) {
      Storage.remove(staleTmp.c_str());
    }
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlPath = getCachedHtmlPath();
  const auto tmpHtmlPath = epub->getCachePath() + "/html/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Pre-Translation: prefer the persisted bilingual HTML when the translator subsystem has
  // produced one for this spine. It survives layout-cache invalidation (font/size/mode only
  // invalidate the .bin) and is parsed directly instead of the unzipped chapter HTML.
  const auto translatedPath = getTranslatedHtmlPath();
  // Only build from the translated HTML when it exists as a complete, committed file
  // (guaranteed by the atomic ".part" -> rename write); never lay out from a partial.
  // This is the "which file do I parse" question, NOT "does this chapter have a translation" --
  // a plugin-translated chapter has no sidecar and is parsed from its own HTML (see hasTranslation).
  const bool usingTranslatedSidecar = hasTranslatedSidecar();

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds, so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  // reusedHtml also stays true for a translated sidecar: it means "the parse source is a
  // persistent file the build lifecycle must never promote or delete", which holds for both
  // the cached unzipped HTML and the translator-owned .translated.html.
  bool htmlCached = usingTranslatedSidecar;
  std::string chapterHtmlPath;
  if (usingTranslatedSidecar) {
    LOG_DBG("SCT", "Using translated HTML: %s", translatedPath.c_str());
  } else if (!ensureChapterHtml(chapterHtmlPath, htmlCached)) {
    return false;
  }
  const bool reusedHtml = htmlCached;
  // Bind (no copy) to whichever source the parser will read.
  const std::string& parseSource = usingTranslatedSidecar ? translatedPath : chapterHtmlPath;

  // Resolve the translation presence only now: for a plugin-translated book the answer lives INSIDE
  // the chapter HTML, so it cannot be known before the file above exists. This is the value stamped
  // into the header and the one the layout is keyed on, so it has to be the authoritative one --
  // never hasTranslation()'s "not knowable yet" default.
  bool translatedSource;
  // The half of the answer that is immutable for this book file, memoized into the header so later
  // loads need no scan. False also means "this build did not look" (the sidecar path) -- see
  // writeSectionFileHeader and the load-side note on when that is safe to trust.
  bool embeddedTranslation = false;
  if (usingTranslatedSidecar) {
    translatedSource = true;  // a committed sidecar is bilingual by construction; no scan needed
  } else if (translationPresence_ != TranslationPresence::Unknown) {
    // Already resolved over this same file -- typically by the reader's fallback gate moments ago,
    // or by a previous build of this Section. Reuse it rather than scanning twice per chapter.
    translatedSource = translationPresence_ == TranslationPresence::Yes;
    embeddedTranslation = translatedSource;
  } else {
    embeddedTranslation = translationdetect::htmlHasTranslatedBlock(parseSource, epub->getLanguage());
    translatedSource = embeddedTranslation;
    translationPresence_ = translatedSource ? TranslationPresence::Yes : TranslationPresence::No;
  }
  translatedSource_ = translatedSource;

  // Per-chapter auto-fallback: a filtering layout on a chapter with no translated content would
  // filter for translated words that do not exist and render a blank chapter. Lay this chapter out
  // as Both so it still renders. This is a layout/cache-key decision only -- the persisted
  // display-mode setting is untouched (per-chapter), and the reader (which owns the user-facing
  // toast) has already decided whether to notify on entry. The downgrade goes through the SAME
  // effectiveLayout() that loadSectionFile() keys on, off the same translatedSource observation that
  // is stamped into the header, so build and lookup can never disagree.
  const PtLayout effectivePtLayout = effectiveLayout(spec.ptLayout, translatedSource);
  if (effectivePtLayout != spec.ptLayout) {
    LOG_DBG("SCT", "No translation for spine %d; laying out with the Both layout", spineIndex);
  }
  // The header cache-key and the parser both key on the effective (post-fallback) layout.
  ReaderRenderSpec effectiveSpec = spec;
  effectiveSpec.ptLayout = effectivePtLayout;
  // ... and the translation font is only keyed where translated words reach the page; see
  // keyedTranslationFontId(). loadSectionFile() normalizes the lookup the same way.
  effectiveSpec.translationFontId = keyedTranslationFontId(spec.translationFontId, effectivePtLayout);
  effectiveSpec.annotationFontId = keyedAnnotationFontId(spec.annotationFontId, effectivePtLayout);

  if (!Storage.openFileForWrite("SCT", binTmpPath(), file)) {
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  // Header is written with the incomplete-version sentinel; finalizeBuild() commits it. The
  // effective (post-fallback) layout plus whether the source carried translations is what actually
  // shaped these pages, and is what the next load keys on.
  writeSectionFileHeader(effectiveSpec, translatedSource, embeddedTranslation);

  auto ctx = makeUniqueNoThrow<BuildContext>();
  if (!ctx) {
    LOG_ERR("SCT", "OOM: BuildContext");
    file.close();
    Storage.remove(binTmpPath().c_str());
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  // htmlCached == "htmlPath is the live cache" (reused, or just promoted). finalizeBuild/abandonBuild
  // then leave the cached HTML alone; only an un-promoted temp (rename failed) is theirs to clean up.
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->parsePath = parseSource;

  // Derive the content base directory and image cache path prefix for the parser
  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (spec.embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser && !ctx->cssParser->loadFromCache()) {
      LOG_ERR("SCT", "Failed to load CSS from cache");
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  // The parser stores the path/contentBase/imageBasePath by reference, so they must
  // live in the BuildContext (which outlives the parser). The page-complete callback
  // captures the BuildContext pointer to append to its in-RAM LUT; build_ owns the
  // context for the parser's whole lifetime.
  BuildContext* ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub, ctxPtr->parsePath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      spec.focusReadingEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        ctxPtr->lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      spec.embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, spec.imageRendering, std::move(tocAnchors),
      popupFn, ctxPtr->cssParser, effectivePtLayout, epub->getLanguage(),
      // The NORMALIZED ids, the same ones the header is keyed on: the parser measures translated
      // lines and annotation rows with them, so anything that zeroes one for the key must zero it for
      // the layout too.
      effectiveSpec.translationFontId, effectiveSpec.annotationFontId,
      // Not keyed: the sentence aligner is a pure function of the two texts (see ReaderRenderSpec).
      spec.interlinearPairFn);
  if (!ctx->parser) {
    LOG_ERR("SCT", "OOM: ChapterHtmlSlimParser");
    if (ctx->cssParser) ctx->cssParser->clear();
    file.close();
    Storage.remove(binTmpPath().c_str());
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  Hyphenator::setPreferredLanguage(epub->getLanguage());
  // Clear any translated-language slot left by a previous build; the slim parser repopulates it
  // from each translated block's lang= as it parses, so it never leaks across books/sections.
  Hyphenator::setTranslatedLanguage("");
  build_ = std::move(ctx);

  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    abandonBuild();
    return false;
  }
  build_->totalBytes = build_->parser->parseTotalBytes();
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (;;) {
    const auto status = build_->parser->parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Parse error during incremental build");
      abandonBuild();
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      return finalizeBuild();
    }
    // ParseStatus::More: yield once we've laid out the requested number of pages.
    if (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const { return Storage.exists(getCachedHtmlPath().c_str()); }

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;
  for (const auto& [key, page] : build_->parser->getAnchors()) {
    if (key == anchor) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) {
    return page;
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getPageForAnchor(anchor);
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint64_t est = static_cast<uint64_t>(partialPageCount_) * partialTotalBytes_ / partialBytesConsumed_;
    if (est <= pageCount) return pageCount;
    return est > 60000 ? 60000 : static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation: scale the pages built so far by the fraction of HTML still unparsed. This
  // re-derives from a growing, non-uniform sample, so it jitters up and down as the build crosses
  // dense vs sparse regions of the chapter.
  const uint64_t raw = static_cast<uint64_t>(builtPageCount_) * total / consumed;

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > 60000 ? 60000 : static_cast<uint16_t>(est);
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the li LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  const bool asPartial = (version == SECTION_FILE_PARTIAL_VERSION);

  const auto failCommit = [this]() {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
    return false;
  };

  const uint32_t lutOffset = file.position();
  for (const auto& entry : build_->lut) {
    if (entry.fileOffset == 0) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      return failCommit();
    }
    serialization::writePod(file, entry.fileOffset);
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets). For a
  // partial, skip anchors that landed on the incomplete trailing page the suspend drops.
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  uint16_t anchorCount = 0;
  for (const auto& [anchor, page] : anchors) {
    if (!asPartial || page < builtPageCount_) anchorCount++;
  }
  serialization::writePod(file, anchorCount);
  for (const auto& [anchor, page] : anchors) {
    if (asPartial && page >= builtPageCount_) continue;
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(build_->lut.size()));
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  if (asPartial) {
    // Watermark trailer, located on load as liLutOffset + pageCount * sizeof(uint16_t).
    serialization::writePod(file, bytesConsumed);
    serialization::writePod(file, totalBytes);
  }

  // Patch header with the built page count and section offsets...
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(builtPageCount_));
  serialization::writePod(file, builtPageCount_);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  // ...then commit by overwriting the sentinel version with the real one. Writing the
  // version last makes it the commit point: a crash before here leaves version 0.
  file.seek(0);
  serialization::writePod(file, version);
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // Swap into place. A crash between remove and rename loses the old file but keeps a
  // fully-committed tmp; the next build just removes it and rebuilds.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(binTmpPath().c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to move built section into place");
    Storage.remove(binTmpPath().c_str());
    return false;
  }
  return true;
}

bool Section::finalizeBuild() {
  // Flush the trailing page (emits the last page via the completePageFn into the LUT).
  build_->parser->finishParse();

  if (!build_->reusedHtml) {
    // Parse succeeded: promote the freshly unzipped HTML to the persistent cache so future
    // rebuilds skip zip inflation. If promotion fails, drop the temp -- the build still succeeded.
    if (!Storage.rename(build_->tmpHtmlPath.c_str(), build_->htmlPath.c_str())) {
      LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
      Storage.remove(build_->tmpHtmlPath.c_str());
    }
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  if (build_->cssParser) build_->cssParser->clear();
  build_.reset();
  if (!committed) {
    // commitBuildFile removed filePath before the failed swap, so nothing valid remains.
    partial_ = false;
    partialPageCount_ = 0;
    pageCount = 0;
    builtPageCount_ = 0;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool worthKeeping = builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    }
  }

  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (!committed && file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lut.size()) || !file) {
    return nullptr;
  }
  const uint32_t pos = build_->lut[page].fileOffset;
  if (pos == 0) {
    return nullptr;
  }
  // The .bin is open O_RDWR for the build. Read the already-written page, then restore
  // the write cursor so the next onPageComplete keeps appending where it left off.
  const uint32_t writePos = file.position();
  file.seek(pos);
  auto p = Page::deserialize(file);
  file.seek(writePos);
  return p;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t lutOffset;
  serialization::readPod(f, lutOffset);
  f.seek(lutOffset + sizeof(uint32_t) * page);
  uint32_t pagePos;
  serialization::readPod(f, pagePos);
  f.seek(pagePos);

  return Page::deserialize(f);
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_ && page < static_cast<int>(build_->lut.size())) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPage(currentPage);
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        // Skip editorial rows the reader inserted itself. Under PtLayout::Interlinear an Annotation
        // row is a fragment of translated text, so including it would interleave two languages in
        // every consumer of this text (the QR page-text view, bookmark summaries). Scoped to Annotation
        // only: Translation rows ARE part of the chapter under the Both layout (Interleaved), and
        // dropping them would change that mode's behaviour.
        if (line.fontRole == LineFontRole::Annotation) continue;
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            if (!fullText.empty()) fullText += " ";
            fullText += block.wordText(i);
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  // Only a finalized section's count is the chapter total; a partial's count is just the
  // suspended build's watermark, which would skew progress mapping. Callers fall back to
  // their own estimates.
  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::findParagraphIndexForPage(const uint16_t page) const {
  if (build_ && page < build_->lut.size()) {
    return build_->lut[page].paragraphIndex;
  }
  // Not laid out by this build (or no build): the committed file may still cover it -- a finalized
  // section, or a partial from a previous session that a rebuild has not yet caught up to.
  return getParagraphIndexForPage(page);
}

std::optional<uint16_t> Section::paragraphAnchorForPage(const uint16_t page) const {
  // Page 0 needs no anchor: the chapter's first page is page 0 under EVERY pagination, so the saved
  // page number is already layout-independent. Anchoring it would be actively worse -- the only
  // index page 0 can offer is the last paragraph on it, which under a larger font starts on page 1.
  if (page == 0) return std::nullopt;
  const auto here = findParagraphIndexForPage(page);
  // 0 = the chapter has opened no <p> at all by this page (it marks its paragraphs with something
  // else, or this is still the front matter). Checked before the second read so a chapter that can
  // never be anchored costs one LUT lookup per page turn rather than two.
  if (!here || *here == 0) return std::nullopt;
  const auto prev = findParagraphIndexForPage(page - 1);
  if (!prev) return std::nullopt;
  // Preferred: the first paragraph this page opens. Resolves back to exactly this page under an
  // unchanged pagination, and after a re-layout lands at most one paragraph tail above the old top.
  if (const uint16_t opening = ParagraphAnchor::openingAnchor(*prev, *here)) return opening;
  // This page opens no paragraph of its own: it lies inside one that began earlier. Anchor on that
  // running paragraph -- landing at its start is a bounded re-read, never a skip -- but only when it
  // began recently enough.
  if (!ParagraphAnchor::guardNeeded(page)) return here;  // cannot have started before page 0
  const auto guard = findParagraphIndexForPage(ParagraphAnchor::guardPage(page));
  if (!guard) return std::nullopt;
  if (const uint16_t spanning = ParagraphAnchor::spanningAnchor(*here, *guard)) return spanning;
  return std::nullopt;
}

std::optional<uint16_t> Section::findPageForParagraphIndex(const uint16_t pIndex) const {
  if (build_) {
    // Stamped indices are non-decreasing across pages, so the last entry alone says whether the
    // paragraph has been reached -- this is polled once per build chunk and must stay cheap.
    if (build_->lut.empty() || build_->lut.back().paragraphIndex < pIndex) {
      return std::nullopt;  // not laid out yet: build more and ask again
    }
    for (size_t i = 0; i < build_->lut.size(); i++) {
      if (build_->lut[i].paragraphIndex >= pIndex) return static_cast<uint16_t>(i);
    }
    return std::nullopt;
  }
  // Strict, unlike getPageForParagraphIndex(), which clamps an index past the end of the chapter
  // onto the LAST page -- the right answer for the KOSync mapper (a synced position beyond this
  // chapter belongs at its end) and the wrong one for a reposition, where it would present as the
  // very teleport-to-the-end this anchor exists to prevent. Range-check first and report "not in
  // this chapter" instead, leaving the caller on its current page.
  if (pageCount == 0) return std::nullopt;
  const auto lastStamped = getParagraphIndexForPage(static_cast<uint16_t>(pageCount - 1));
  if (!lastStamped || *lastStamped < pIndex) return std::nullopt;
  return getPageForParagraphIndex(pIndex);
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
