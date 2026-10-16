#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <string_view>

#include "modules/lingua/services/LinguaCacheKey.h"
#include "modules/lingua/services/TranslatedContentDetector.h"
std::string Section::getTranslatedHtmlPath() const {
  return epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".translated.html";
}

std::string Section::getTranslationPairCachePath() const {
  return epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".pairs";
}

std::string Section::getTranslationSourcePath() const {
  // The file that actually CONTAINS this chapter's translation. A reader-translated chapter has a
  // committed sidecar; a plugin-translated (Calibre) book has none and carries its translation inside
  // the chapter's own XHTML. The overlays used to be handed the sidecar path unconditionally, so on
  // an embedded-translation book they opened nothing and silently showed no translations at all.
  return hasTranslatedSidecar() ? getTranslatedHtmlPath() : getCachedHtmlPath();
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
    bool textless = false;
    const bool translated = lingua::content::htmlHasTranslatedBlock(htmlPath, epub->getLanguage(), &textless);
    translationPresence_ = translated ? TranslationPresence::Yes : TranslationPresence::No;
    textPresence_ = textless ? TextPresence::Textless : TextPresence::HasText;
    return translated;
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
  bool textless = false;
  translationPresence_ = lingua::content::htmlHasTranslatedBlock(parsePath, epub->getLanguage(), &textless)
                             ? TranslationPresence::Yes
                             : TranslationPresence::No;
  textPresence_ = textless ? TextPresence::Textless : TextPresence::HasText;
  if (!promoted) {
    // An un-promoted temp is nobody's to keep: startBuild() would re-inflate it under its own
    // ownership rules, and leaving it would strand a stale ".tmp_<n>.html" in the cache dir.
    Storage.remove(parsePath.c_str());
  }
}

bool Section::isTextless() const {
  // Both scan sites record textPresence_, so a caller that has already resolved translation
  // presence -- which every caller does, the fallback gate included -- answers from memory here.
  // Unknown means no scan has run (or the inflate failed): answer false, the safe direction.
  return textPresence_ == TextPresence::Textless;
}

LinguaLayout Section::effectiveLayout(const LinguaLayout requested, const bool translatedSource) {
  // With no translation in the source there are no translated words to drop, keep or pair, so EVERY
  // layout degrades to Both -- which on an untranslated chapter is just the plain original. Both is
  // therefore both the request and the fallback, which is exactly why it says nothing about the
  // source the pages came from and why the caller pairs this with the translatedSource flag in the
  // cache key. See the declaration comment in Section.h.
  return translatedSource ? requested : LinguaLayout::Both;
}

void Section::writeLinguaHeaderFields(const ReaderRenderSpec& spec, const bool translatedSource,
                                      const bool embeddedTranslation) {
  serialization::writePod(file, spec.translationFontId);  // translated-text font (cache key)
  // Annotation-row font (cache key). Non-zero only under LinguaLayout::Interlinear.
  serialization::writePod(file, spec.annotationFontId);
  serialization::writePod(file, static_cast<uint8_t>(spec.linguaLayout));  // page layout (cache key)
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
  // Which offset space the pages use (LinguaSourceKind.h). Not a cache key: it is a property of the
  // source file, which a translation commit replaces together with this .bin (both translation
  // activities delete the chapter's .bin when they commit a sidecar).
  serialization::writePod(file, linguaSourceKind_);
}

bool Section::readLinguaHeaderFields(const ReaderRenderSpec& spec) {
  int fileTranslationFontId;
  int fileAnnotationFontId;
  uint8_t fileLinguaLayout;
  bool fileTranslatedSource;
  bool fileEmbeddedTranslation;
  uint8_t fileSourceKind = 0;
  serialization::readPod(file, fileTranslationFontId);
  serialization::readPod(file, fileAnnotationFontId);
  serialization::readPod(file, fileLinguaLayout);
  serialization::readPod(file, fileTranslatedSource);
  serialization::readPod(file, fileEmbeddedTranslation);
  serialization::readPod(file, fileSourceKind);
  linguaSourceKind_ = fileSourceKind <= static_cast<uint8_t>(LinguaSourceKind::LegacySidecar)
                          ? static_cast<LinguaSourceKind>(fileSourceKind)
                          : LinguaSourceKind::LegacySidecar;  // unknown value: the conservative space
  linguaSourceKindKnown_ = true;

  // Match on the EFFECTIVE (post-fallback) layout: an untranslated chapter is laid out and its
  // header stamped with the Both layout by startBuild(), so it must also be looked up under Both
  // -- otherwise a filtering layout never matches the Both-stamped cache and rebuilds on every
  // visit. See effectiveLayout().
  //
  // The source flag is the other half of the Lingua key. Both is stamped by an
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
  const LinguaLayout layout = effectiveLayout(spec.linguaLayout, translatedSource);
  return lingua::keyedTranslationFontId(spec.translationFontId, layout) == fileTranslationFontId &&
         lingua::keyedAnnotationFontId(spec.annotationFontId, layout) == fileAnnotationFontId &&
         translatedSource == fileTranslatedSource && static_cast<uint8_t>(layout) == fileLinguaLayout;
}

LinguaSourceKind Section::linguaSourceKind() const {
  if (build_ && build_->parser) return build_->parser->linguaSourceKind();
  if (linguaSourceKindKnown_) return linguaSourceKind_;
  linguaSourceKindKnown_ = true;
  linguaSourceKind_ = LinguaSourceKind::Original;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return linguaSourceKind_;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (!linguaIsReadableVersion(version) || !f.seek(linguaSourceKindHeaderPos())) return linguaSourceKind_;
  uint8_t kind = 0;
  serialization::readPod(f, kind);
  linguaSourceKind_ = kind <= static_cast<uint8_t>(LinguaSourceKind::LegacySidecar)
                          ? static_cast<LinguaSourceKind>(kind)
                          : LinguaSourceKind::LegacySidecar;
  return linguaSourceKind_;
}

bool Section::linguaAcceptsVisibleTextOffset(const uint32_t offset) const {
  return lingua::sourceAcceptsVisibleTextOffset(linguaSourceKind(), offset);
}

bool Section::linguaPageForVisibleTextOffset(const uint32_t offset, const bool preferFirstAtOffset,
                                             std::optional<uint16_t>& out) const {
  if (linguaResolvingPage_) return false;  // the nested lookups below are plain upstream ones
  const int hint = linguaTieBreakPage_;
  linguaTieBreakPage_ = -1;
  const LinguaSourceKind kind = linguaSourceKind();
  if (!lingua::sourceAcceptsVisibleTextOffset(kind, offset)) {
    out = std::nullopt;
    return true;
  }
  if (kind != LinguaSourceKind::SidecarV2) return false;

  // v2: pages inside one inserted translation share their start offset. Upstream resolves a tie
  // to the LAST page (or the first with preferFirstAtOffset); pick the caller's page when it is in
  // the run, else the first, so a reopen never jumps forward through the translation.
  linguaResolvingPage_ = true;
  const auto last = getPageForVisibleTextOffset(offset, false);
  out = last;
  if (last && getVisibleTextOffsetForPage(*last) == offset) {
    const auto first = getPageForVisibleTextOffset(offset, true);
    if (first) {
      out = lingua::pickTiePage(*first, *last, hint);
    }
  }
  // No exact start match: preferFirstAtOffset cannot change the upstream answer either.
  (void)preferFirstAtOffset;
  linguaResolvingPage_ = false;
  return true;
}

void Section::linguaFinalizeHeader(const uint32_t paragraphLutOffset) {
  linguaSourceKind_ = linguaSourceKind();  // the parser's final answer
  linguaSourceKindKnown_ = true;
  // The header patch that follows seeks explicitly, so the write position need not be restored.
  if (linguaSourceKind_ == LinguaSourceKind::LegacySidecar) {
    file.seek(paragraphLutOffset);
    serialization::writePod(file, static_cast<uint16_t>(0));
  }
  file.seek(linguaSourceKindHeaderPos());
  serialization::writePod(file, linguaSourceKind_);
}

bool Section::linguaSettleSourceKind() {
  linguaSettledToEnd_ = false;
  if (!build_ || !build_->parser) return true;
  ChapterHtmlSlimParser& parser = *build_->parser;
  // Only a sidecar's kind is provisional (LegacySidecar until its root element says v2).
  if (parser.linguaSourceKind() != LinguaSourceKind::LegacySidecar) return true;
  // The root element sits in the first parse buffer for any real file; pages laid out on the way
  // go to the LUT exactly as buildSomeMore() would put them there.
  while (!parser.linguaHasSeenRoot()) {
    const auto status = parser.parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Parse error while reading the sidecar's root element");
      abandonBuild();
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      linguaSettledToEnd_ = true;
      break;
    }
  }
  build_->bytesConsumed = parser.parseBytesConsumed();
  return true;
}
