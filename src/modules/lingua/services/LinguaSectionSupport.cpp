#include <Epub/Section.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

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
