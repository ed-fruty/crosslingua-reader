#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: text decoration bits now include line-through in serialized wordStyles.
// v29: TextBlock word data stored as one flat arena (offset table + NUL-terminated
// text blob) instead of length-prefixed strings and per-field arrays.
// v30: Arabic shaping changed both drawing and measurement (getTextAdvanceX now
//      measures the shaped visual text); cached word positions from v29 no longer
//      match what drawText renders.
// v31: word continuation preserved when splitting CJK text on MAX_WORD_SIZE.
// v32: Pre-Translation — translationMode byte added to the header (cache key), and a
//      per-line paragraphIdx + per-page paragraph range added to Page serialization.
// v33: Per-block hyphenation language. Translated blocks (lang= differing from the book
//      language) now hyphenate with their own script's rules instead of the book-wide one,
//      so a bilingual (e.g. en->uk) book's translated text finally breaks. Hyphenated splits
//      are baked into the serialized pages, so sections cached under v32 (English-only splits
//      on Cyrillic text) must be regenerated.
// v35: Side by Side (translationMode 5) now lays original and translation into two half-width
//      columns (renderSideBySide) instead of full-width sequential blocks. The Page/PageLine
//      binary structure is unchanged — columns reuse the existing per-line xPos field — but a
//      mode-5 section cached under v34 carries an identical header key (translationMode still 5)
//      and would otherwise be served with the old single-column layout, so the version bump is
//      the only thing that forces those sections to rebuild.
// v36: Merge with upstream develop, which independently numbered its own "v32":
//      ImageBlock serializes the book-internal source href after the cache path
//      (lazy extraction: images are header-probed at build time and extracted on
//      first render). The two lines used 32..35 for different, mutually unreadable
//      layouts, so v36 supersedes both — the merged format carries the Pre-Translation
//      fields (v32-v35 above) AND the ImageBlock source href, and the bump forces a
//      rebuild of any section cached by either line.
// v37: Second merge with upstream develop, whose independent numbering had meanwhile
//      advanced to its own "v33" while this line stood at 36. That upstream v33 bundles
//      two cache-invalidating changes, both of which the merged format keeps: TextBlock
//      now serializes a per-word ruby annotation string after the word arena (native
//      <ruby>/<rt> support, <rp> skipped), and a closing tag now starts a fresh text
//      block so a closed block's style no longer leaks into following bare text, which
//      shifts the cached block boundaries. v37 supersedes both numbering lines: the
//      merged format carries the Pre-Translation fields (v32-v35), the ImageBlock source
//      href (upstream's "v32") AND the per-word ruby strings (upstream's "v33"), and the
//      bump forces a rebuild of any section cached by either line. The merge also
//      renumbers the persisted word style bit for ruby: upstream shipped
//      EpdFontFamily::RUBY_CONTINUE as 64, which is this line's TRANSLATED bit, so ruby
//      moves to bit 7 (128) and the never-written PT_TOOLTIP reservation on that bit is
//      retired. v37 rejects every cache written by either line, so no stored style byte
//      is ever reinterpreted under the new numbering.
// v38: The header no longer stores the raw Pre-Translation display mode. It stores the LAYOUT that
//      mode implies (PtLayout: Both / OriginalOnly / TranslationOnly / SideBySide), so modes whose
//      pages are byte-identical -- Normal vs Interleaved (a gray level, drawn not laid out),
//      Original Only vs Page Translation vs Tooltip (overlays composited at view time) -- now share
//      one cache entry and switching between them is instant instead of a full chapter re-layout.
//      The byte occupies the same header slot as the old mode byte but means something different,
//      so v37 files must be rejected rather than reinterpreted. v38 also adds a translationFontId int to the header:
//      laying translated text out in its own font changes word measurement and line breaking, so
//      unlike the drawing-only shade it IS part of the cache key. Finally, PageLine serializes a
//      LineFontRole byte after paragraphIdx so a page can mix body and smaller/annotation text;
//      every line the layout engine emits today writes Body, so a v38 rebuild is byte-equivalent
//      to v37 apart from the extra zero byte per line.
// v39: A translatedSource bool follows the PtLayout byte: it records WHICH source HTML the pages
//      were laid out from (the chapter's .translated.html, or the plain original). The layout byte
//      alone cannot say -- PtLayout::Both is what an untranslated chapter AND an inline-bilingual
//      chapter both stamp -- so without it a chapter laid out before its translation arrived stays
//      a cache HIT afterwards and silently serves untranslated pages in a bilingual mode (and vice
//      versa).
//      WHY THIS IS ITS OWN VERSION AND NOT PART OF v38: two different header layouts were written
//      under the number 38 during development -- first without the translatedSource byte, then with
//      it. The byte sits in the MIDDLE of the header, so a 38 written by the earlier layout passes
//      the version gate and then every field after the PtLayout byte is read shifted by one. The
//      mismatch check usually catches that as a stale key and rebuilds, but it is not guaranteed to
//      (the shifted bytes can compare equal), and the pageCount / LUT offsets that follow are read
//      before any such check can help. v38 was therefore never a stable, released layout: nothing
//      may claim to read it, and 39 exists so the shorter layout is rejected on its version alone.
constexpr uint8_t SECTION_FILE_VERSION = 39;
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
// Derived so the pairing can't be forgotten: 0xFE for v28, 0xFD for v29, ... 0xF3 for v39.
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
// The derivation only stays a distinct sentinel while the two ranges have not met; assert it rather
// than trusting a future bump to notice.
static_assert(SECTION_FILE_PARTIAL_VERSION > SECTION_FILE_VERSION &&
                  SECTION_FILE_PARTIAL_VERSION != SECTION_FILE_INCOMPLETE_VERSION,
              "Partial sentinel collides with a real version");
// The second sizeof(int) is the Pre-Translation translationFontId; the extra sizeof(uint8_t) after
// the two bools is the Pre-Translation PtLayout byte, and the sizeof(bool) right after it is the
// translated-source flag.
constexpr uint32_t HEADER_SIZE =
    sizeof(uint8_t) + sizeof(int) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) +
    sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
    sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

// The translation font belongs in the cache key only where translated words are actually laid out.
// Under the OriginalOnly layout they are all dropped, so not one line in the resulting pages was
// measured in that font and no value of it can move a line break. Stamping the real id there would
// make a change to the Interleaved mode's Translation Size invalidate every cached chapter of the
// book, including the chapters of a mode that lays out no translated text at all. (The two modes that
// MAP to OriginalOnly, Tooltip and Page Translation, own SEPARATE size settings which are composited
// at view time and by design never reach a ReaderRenderSpec -- see CrossPointSettings::
// TRANSLATION_SIZE. This normalization covers the id that does reach one.)
//
// Keyed off the EFFECTIVE (post-fallback) layout, not the requested one, and applied to both the
// header write and the lookup, so build and lookup can never disagree: a chapter with no committed
// translation is laid out as Both even in Tooltip mode, keeps the real id, and so stays a cache hit
// for the inline modes that share that layout.
constexpr int keyedTranslationFontId(const int translationFontId, const PtLayout effectiveLayout) {
  return effectiveLayout == PtLayout::OriginalOnly ? 0 : translationFontId;
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

void Section::writeSectionFileHeader(const ReaderRenderSpec& spec, const bool translatedSource) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.translationFontId) +
                                   sizeof(spec.lineCompression) + sizeof(spec.extraParagraphSpacing) +
                                   sizeof(spec.paragraphAlignment) + sizeof(spec.viewportWidth) +
                                   sizeof(spec.viewportHeight) + sizeof(pageCount) + sizeof(spec.hyphenationEnabled) +
                                   sizeof(spec.embeddedStyle) + sizeof(uint8_t) /* PtLayout */ +
                                   sizeof(translatedSource) + sizeof(spec.imageRendering) +
                                   sizeof(spec.focusReadingEnabled) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  // Written as the incomplete sentinel; finalizeBuild() patches it to
  // SECTION_FILE_VERSION as the last step, committing the file.
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, spec.fontId);
  serialization::writePod(file, spec.translationFontId);  // Pre-Translation translated-text font (cache key)
  serialization::writePod(file, spec.lineCompression);
  serialization::writePod(file, spec.extraParagraphSpacing);
  serialization::writePod(file, spec.paragraphAlignment);
  serialization::writePod(file, spec.viewportWidth);
  serialization::writePod(file, spec.viewportHeight);
  serialization::writePod(file, spec.hyphenationEnabled);
  serialization::writePod(file, spec.embeddedStyle);
  serialization::writePod(file, static_cast<uint8_t>(spec.ptLayout));  // Pre-Translation page layout (cache key)
  // Which source HTML these pages were laid out from (cache key). The layout byte cannot express
  // it: Both is stamped both by an untranslated chapter and by an inline-bilingual one.
  serialization::writePod(file, translatedSource);
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
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t filePtLayout;
    bool fileTranslatedSource;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileTranslationFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, filePtLayout);
    serialization::readPod(file, fileTranslatedSource);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);

    // Match on the EFFECTIVE (post-fallback) layout: an untranslated chapter is laid out and its
    // header stamped with the Both layout by startBuild(), so it must also be looked up under Both
    // -- otherwise a filtering layout never matches the Both-stamped cache and rebuilds on every
    // visit. See effectiveLayout().
    //
    // The source flag is the other half of the Pre-Translation key. Both is stamped by an
    // untranslated chapter AND by an inline-bilingual one, so without this compare a chapter laid
    // out before its translation was downloaded would stay a cache HIT afterwards and serve
    // untranslated pages in a bilingual mode (and, symmetrically, a translated cache would survive
    // the translation being deleted). One SD stat, shared with the layout resolution below.
    const bool translatedSource = hasTranslatedHtml();
    const PtLayout layout = effectiveLayout(spec.ptLayout, translatedSource);
    if (spec.fontId != fileFontId || keyedTranslationFontId(spec.translationFontId, layout) != fileTranslationFontId ||
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

bool Section::hasTranslatedHtml() const {
  // The translated HTML is committed atomically: it is written to a ".part" file and only
  // renamed into place after a clean, complete write. So a finished translation is exactly
  // "the final file exists" — a power loss mid-translation leaves only a ".part", never a
  // truncated final file. This also keeps pre-existing translated caches (written before the
  // atomic-commit change) valid, so users are never forced to re-translate. The stale ".part"
  // itself is reclaimed by clearCache() on the next .bin invalidation.
  return Storage.exists(getTranslatedHtmlPath().c_str());
}

PtLayout Section::effectiveLayout(const PtLayout requested, const bool translatedSource) {
  // With no committed translation there are no translated words to drop, keep or pair, so EVERY
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
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";

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
  const bool usingTranslatedSource = hasTranslatedHtml();

  // Per-chapter auto-fallback: a filtering layout with no translated HTML would filter for
  // translated words that do not exist and render a blank chapter. Lay this chapter out as Both so
  // it still renders. This is a layout/cache-key decision only -- the persisted display-mode setting
  // is untouched (per-chapter), and the reader (which owns the user-facing toast) has already decided
  // whether to notify on entry. The downgrade goes through the SAME effectiveLayout() that
  // loadSectionFile() keys on, off the same usingTranslatedSource observation that is stamped into
  // the header, so build and lookup can never disagree.
  const PtLayout effectivePtLayout = effectiveLayout(spec.ptLayout, usingTranslatedSource);
  if (effectivePtLayout != spec.ptLayout) {
    LOG_DBG("SCT", "No translation for spine %d; laying out with the Both layout", spineIndex);
  }
  // The header cache-key and the parser both key on the effective (post-fallback) layout.
  ReaderRenderSpec effectiveSpec = spec;
  effectiveSpec.ptLayout = effectivePtLayout;
  // ... and the translation font is only keyed where translated words reach the page; see
  // keyedTranslationFontId(). loadSectionFile() normalizes the lookup the same way.
  effectiveSpec.translationFontId = keyedTranslationFontId(spec.translationFontId, effectivePtLayout);

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  // reusedHtml also stays true for a translated source: it means "the parse source is a
  // persistent file the build lifecycle must never promote or delete", which holds for both
  // the cached unzipped HTML and the translator-owned .translated.html.
  const bool reusedHtml = usingTranslatedSource || Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  if (usingTranslatedSource) {
    LOG_DBG("SCT", "Using translated HTML: %s", translatedPath.c_str());
  } else if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
  } else {
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
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, 8192);
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
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }
  // Bind (no copy) to whichever source the parser will read.
  const std::string& parseSource = usingTranslatedSource ? translatedPath : (htmlCached ? htmlPath : tmpHtmlPath);

  if (!Storage.openFileForWrite("SCT", binTmpPath(), file)) {
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  // Header is written with the incomplete-version sentinel; finalizeBuild() commits it. The
  // effective (post-fallback) layout plus the source it was laid out from is what actually shaped
  // these pages, and is what the next load keys on.
  writeSectionFileHeader(effectiveSpec, usingTranslatedSource);

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
      popupFn, ctxPtr->cssParser, effectivePtLayout, epub->getLanguage());
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

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

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
