// CrossLingua: EpubReaderActivity members that drive the Lingua module. Kept out
// of EpubReaderActivity.cpp so the upstream reader carries only one-line hooks.
#include <Epub/Page.h>
#include <HalStorage.h>

#include <variant>

#include "CrossPointSettings.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/reader/EpubReaderActivity.h"
#include "modules/lingua/activities/BookTranslationActivity.h"
#include "modules/lingua/activities/ChapterTranslationActivity.h"
#include "modules/lingua/activities/LinguaSubmenuActivity.h"
#include "modules/lingua/activities/WordLensPacksActivity.h"

// Overlay-specific input state lives in LinguaReaderIntegration; returns true
// when Lingua consumed this loop tick.
bool EpubReaderActivity::handleLinguaInput() {
  switch (lingua.handleInput(mappedInput, section != nullptr)) {
    case LinguaReaderIntegration::InputAction::Consumed:
      return true;
    case LinguaReaderIntegration::InputAction::Render:
      requestUpdate();
      return true;
    case LinguaReaderIntegration::InputAction::PageBack:
      if (pageTurn(false)) requestUpdate();
      return true;
    case LinguaReaderIntegration::InputAction::PageForward:
      if (pageTurn(true)) requestUpdate();
      return true;
    case LinguaReaderIntegration::InputAction::OpenDefinition: {
      auto definition = lingua.takeWordLensDefinition();
      startActivityForResult(
          std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(definition.headword),
                                                         std::move(definition.text), definition.html),
          [this](const ActivityResult&) { requestUpdate(); });
      return true;
    }
    case LinguaReaderIntegration::InputAction::None:
      break;
  }
  return false;
}

// Memoize this chapter's translation pairs once, off the render path. Called only in idle frames,
// after backgroundBuildTick, and gated like it: input waiting to be handled goes first, then the
// RenderLock and heap check. A no-op in every non-overlay mode.
void EpubReaderActivity::tickLinguaPairCache(bool (*contactClosed)()) {
  if (contactClosed() || anyButtonPressed() || pendingManualTurn != 0) return;
  if (section && !section->isBuilding() && !lingua.overlayBlocksBackgroundWork() && !RenderLock::peek() &&
      buildTickHeapGate()) {
    RenderLock lock;
    lingua.buildPairCacheIfNeeded();
  }
}

void EpubReaderActivity::openLinguaMenu() {
  const ReaderRenderSpec before = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
  // The display MODE is tracked separately from the render spec: two modes can share one layout
  // (Tooltip and Page Translation do), so a mode change is invisible to layoutEquals() below and
  // still has to clear the overlay the previous mode left on screen.
  const uint8_t beforeMode = SETTINGS.translationDisplayMode;
  startActivityForResult(
      std::make_unique<LinguaSubmenuActivity>(renderer, mappedInput, epub, currentSpineIndex),
      [this, before, beforeMode](const ActivityResult& result) {
        LinguaResult translation = LinguaResult::NONE;
        if (const auto* menu = std::get_if<MenuResult>(&result.data)) {
          if (menu->action == static_cast<int>(LinguaResult::TRANSLATE_CHAPTER) ||
              menu->action == static_cast<int>(LinguaResult::TRANSLATE_BOOK) ||
              menu->action == static_cast<int>(LinguaResult::MANAGE_WORD_LENS_PACKS)) {
            translation = static_cast<LinguaResult>(menu->action);
          }
        }
        if (translation != LinguaResult::NONE) {
          launchTranslation(translation);
          return;
        }
        if (SETTINGS.translationDisplayMode != beforeMode) {
          lingua.onDisplayModeChanged();
        }
        if (!SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight).layoutEquals(before)) {
          RenderLock lock;
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section ? section->estimatedTotalPages() : 0;
          nextPageNumber = section ? section->currentPage : nextPageNumber;
          section.reset();
        }
        requestUpdate();
      });
}

void EpubReaderActivity::launchTranslation(const LinguaResult kind) {
  if (!epub) return;
  const std::string epubPath = epub->getPath();
  const int spineIndex = currentSpineIndex;
  const std::string translatedPath =
      epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".translated.html";
  const bool alreadyTranslated = Storage.exists(translatedPath.c_str());
  const int page = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  if (!saveProgress(spineIndex, page, totalPages)) {
    pendingSyncSaveError = true;
    requestUpdate();
    return;
  }

  // The book language decides which pack the manager offers first; read it before the Epub goes.
  const std::string bookLanguage = epub->getLanguage();
  {
    RenderLock lock;
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    // After the section: a build in progress holds the Word Lens pack this closes.
    lingua.onReaderExit();
    epub.reset();
  }
  if (kind == LinguaResult::MANAGE_WORD_LENS_PACKS) {
    activityManager.replaceActivity(
        std::make_unique<WordLensPacksActivity>(renderer, mappedInput, epubPath, bookLanguage));
  } else if (kind == LinguaResult::TRANSLATE_BOOK) {
    activityManager.replaceActivity(std::make_unique<BookTranslationActivity>(renderer, mappedInput, epubPath));
  } else {
    activityManager.replaceActivity(std::make_unique<ChapterTranslationActivity>(
        renderer, mappedInput, epubPath, spineIndex, translatedPath, alreadyTranslated));
  }
}
