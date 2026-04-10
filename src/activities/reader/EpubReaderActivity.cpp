#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Check if a chapter's EPUB HTML contains embedded translations (Calibre plugin or previous CrossPoint).
// Extracts chapter to temp file, runs SAX scan, cleans up.
bool chapterHasEmbeddedTranslations(const std::shared_ptr<Epub>& epub, int spineIndex) {
  if (!epub) return false;
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) return false;

  const auto tmpPath = epub->getCachePath() + "/.tmp_detect_" + std::to_string(spineIndex) + ".html";
  FsFile tmpFile;
  if (!Storage.openFileForWrite("ERA", tmpPath, tmpFile)) return false;
  bool ok = epub->readItemContentsToStream(href, tmpFile, 1024);
  tmpFile.close();
  if (!ok) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  bool result = TranslatingHtmlRewriter::hasEmbeddedTranslations(tmpPath);
  Storage.remove(tmpPath.c_str());
  return result;
}

// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr int statusBarMargin = 19;
constexpr int progressBarMarginTop = 1;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// Apply the logical reader orientation to the renderer.
// This centralizes orientation mapping so we don't duplicate switch logic elsewhere.
void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  applyReaderOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  if (SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP) {
    tooltipOverlay = std::make_unique<TooltipOverlay>();
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  if (section && epub) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    subActivity->loop();
    // Deferred exit: process after subActivity->loop() returns to avoid use-after-free
    if (pendingSubactivityExit) {
      pendingSubactivityExit = false;
      exitActivity();
      requestUpdate();
      skipNextButtonCheck = true;  // Skip button processing to ignore stale events
    }
    // Deferred go home: process after subActivity->loop() returns to avoid race condition
    if (pendingGoHome) {
      pendingGoHome = false;
      exitActivity();
      if (onGoHome) {
        onGoHome();
      }
      return;  // Don't access 'this' after callback
    }
    // Deferred translate: launch ChapterTranslatorActivity after menu subactivity is gone
    if (pendingTranslateChapter) {
      pendingTranslateChapter = false;
      if (epub && section) {
        const bool alreadyTranslated = section->hasTranslatedHtml() || epub->hasCalibreTranslation() ||
                                       chapterHasEmbeddedTranslations(epub, currentSpineIndex);
        const auto translatedPath = section->getTranslatedHtmlPath();
        const int si = currentSpineIndex;
        enterNewActivity(new ChapterTranslatorActivity(
            renderer, mappedInput, epub, si, translatedPath, alreadyTranslated,
            [this] {
              // Cancel: just return to reader
              exitActivity();
              requestUpdate();
              skipNextButtonCheck = true;
            },
            [this] {
              // Complete: clear binary section cache so it rebuilds from translated HTML
              exitActivity();
              {
                RenderLock lock(*this);
                if (section) {
                  cachedSpineIndex = currentSpineIndex;
                  cachedChapterTotalPageCount = section->pageCount;
                  nextPageNumber = section->currentPage;
                  section->clearCache();  // Delete .bin so loadSectionFile() fails → createSectionFile() runs
                }
                section.reset();
              }
              requestUpdate();
              skipNextButtonCheck = true;
            }));
      }
      return;
    }
    // Deferred page translate: extract text from current page and launch PageTranslatorActivity
    if (pendingTranslatePage) {
      pendingTranslatePage = false;
      if (epub && section) {
        auto page = section->loadPageFromSectionFile();
        std::string text;
        if (page) {
          text = page->extractText();
          page.reset();
        }
        enterNewActivity(new PageTranslatorActivity(renderer, mappedInput, std::move(text), [this] {
          exitActivity();
          requestUpdate();
          skipNextButtonCheck = true;
        }));
      }
      return;
    }
    return;
  }

  // Handle pending page translate when no subactivity (e.g., from long press OK)
  if (pendingTranslatePage) {
    pendingTranslatePage = false;
    if (epub && section) {
      auto page = section->loadPageFromSectionFile();
      std::string text;
      if (page) {
        text = page->extractText();
        page.reset();
      }
      enterNewActivity(new PageTranslatorActivity(renderer, mappedInput, std::move(text), [this] {
        exitActivity();
        requestUpdate();
        skipNextButtonCheck = true;
      }));
    }
    return;
  }

  // Handle pending go home when no subactivity (e.g., from long press back)
  if (pendingGoHome) {
    pendingGoHome = false;
    if (onGoHome) {
      onGoHome();
    }
    return;  // Don't access 'this' after callback
  }

  // Skip button processing after returning from subactivity
  // This prevents stale button release events from triggering actions
  // We wait until: (1) all relevant buttons are released, AND (2) wasReleased events have been cleared
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    const bool pageCleared = !mappedInput.isPressed(MappedInputManager::Button::PageForward) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::PageForward) &&
                             !mappedInput.isPressed(MappedInputManager::Button::PageBack) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::PageBack);
    if (confirmCleared && backCleared && pageCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  // Tooltip mode: let overlay consume front/side button presses for sentence stepping
  if (tooltipOverlay && tooltipOverlay->handleInput(mappedInput)) {
    requestUpdate();
    return;
  }

  // Confirm button: short press opens menu, long press (>=700ms) triggers mode shortcuts
  constexpr unsigned long longPressMs = 700;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto heldTime = mappedInput.getHeldTime();
    LOG_DBG("ERS", "Confirm released, heldTime=%lu, longPressMs=%lu", heldTime, longPressMs);
    if (heldTime >= longPressMs && handleLongPressConfirm()) {
      return;
    }
    // Short press: open reader menu
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? section->pageCount : 0;
    float bookProgress = 0.0f;
    if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    const bool chapterIsTranslated =
        section && (section->hasTranslatedHtml() || epub->hasCalibreTranslation() ||
                    chapterHasEmbeddedTranslations(epub, currentSpineIndex));
    exitActivity();
    enterNewActivity(new EpubReaderMenuActivity(
        this->renderer, this->mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
        SETTINGS.orientation, SETTINGS.colorTextStyle, SETTINGS.fontFamily, SETTINGS.fontSize, SETTINGS.lineSpacing,
        chapterIsTranslated,
        [this](const uint8_t orientation, const uint8_t translationMode, const uint8_t fontFamily,
               const uint8_t fontSize, const uint8_t lineSpacing) {
          onReaderMenuBack(orientation, translationMode, fontFamily, fontSize, lineSpacing);
        },
        [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoBack();
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // When long-press behavior is None, turn pages on press instead of release.
  const bool usePressForPageTurn = SETTINGS.longPressChapterSkip == CrossPointSettings::LP_NONE;
  const bool isTooltipMode = SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP && tooltipOverlay;
  const bool tooltipUsesFront = isTooltipMode && SETTINGS.tooltipButtons == 0;
  const bool tooltipUsesSide = isTooltipMode && SETTINGS.tooltipButtons == 1;

  // Side buttons for page turn (skip if tooltip hijacks them)
  const bool sidePrev = !tooltipUsesSide && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::PageBack)
                                                                  : mappedInput.wasReleased(MappedInputManager::Button::PageBack));
  const bool sideNext = !tooltipUsesSide && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::PageForward)
                                                                  : mappedInput.wasReleased(MappedInputManager::Button::PageForward));
  // Front buttons for page turn (skip if tooltip hijacks them)
  const bool frontPrev = !tooltipUsesFront && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::Left)
                                                                    : mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool frontNext = !tooltipUsesFront && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::Right)
                                                                    : mappedInput.wasReleased(MappedInputManager::Button::Right));

  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool prevTriggered = sidePrev || frontPrev;
  const bool nextTriggered = sideNext || frontNext || powerPageTurn;

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // any botton press when at end of the book goes back to the last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = UINT16_MAX;
    requestUpdate();
    return;
  }

  const bool skipChapter =
      SETTINGS.longPressChapterSkip == CrossPointSettings::LP_CHAPTER_SKIP && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  // In Translation Modes, long-press side buttons switch orientation + translation mode
  if (SETTINGS.longPressChapterSkip == CrossPointSettings::LP_TRANSLATION_MODES &&
      mappedInput.getHeldTime() > skipChapterMs) {
    if (nextTriggered) {
      // Long-press PageForward: Landscape CCW + Side by Side
      applyOrientation(CrossPointSettings::ORIENTATION::LANDSCAPE_CCW);
      applyTranslationMode(CrossPointSettings::CT_SIDE_BY_SIDE);
    } else {
      // Long-press PageBack: Portrait + Original Only
      applyOrientation(CrossPointSettings::ORIENTATION::PORTRAIT);
      applyTranslationMode(CrossPointSettings::CT_NO_RENDER);
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  // Reset tooltip when changing pages
  if (tooltipOverlay && (prevTriggered || nextTriggered)) {
    tooltipOverlay->onPageChanged();
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
    requestUpdate();
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
    requestUpdate();
  }
}

void EpubReaderActivity::onReaderMenuBack(const uint8_t orientation, const uint8_t translationMode,
                                           const uint8_t fontFamily, const uint8_t fontSize,
                                           const uint8_t lineSpacing) {
  exitActivity();
  // Apply all user-selected settings when the menu is dismissed.
  // This ensures the menu can be navigated without immediately changing the reader.
  applyOrientation(orientation);
  applyTranslationMode(translationMode);
  applyFontFamily(fontFamily);
  applyFontSize(fontSize);
  applyLineSpacing(lineSpacing);
  requestUpdate();
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // Calculate values BEFORE we start destroying things
      const int currentP = section ? section->currentPage : 0;
      const int totalP = section ? section->pageCount : 0;
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();

      // 1. Close the menu
      exitActivity();

      // 2. Open the Chapter Selector
      enterNewActivity(new EpubReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, epub, path, spineIdx, currentP, totalP,
          [this] {
            exitActivity();
            requestUpdate();
          },
          [this](const int newSpineIndex) {
            if (currentSpineIndex != newSpineIndex) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = 0;
              section.reset();
            }
            exitActivity();
            requestUpdate();
          },
          [this](const int newSpineIndex, const int newPage) {
            if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
              currentSpineIndex = newSpineIndex;
              nextPageNumber = newPage;
              section.reset();
            }
            exitActivity();
            requestUpdate();
          }));

      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      // Launch the slider-based percent selector and return here on confirm/cancel.
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      exitActivity();
      enterNewActivity(new EpubReaderPercentSelectionActivity(
          renderer, mappedInput, initialPercent,
          [this](const int percent) {
            // Apply the new position and exit back to the reader.
            jumpToPercent(percent);
            exitActivity();
            requestUpdate();
          },
          [this]() {
            // Cancel selection and return to the reader.
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      // Defer go home to avoid race condition with display task
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub) {
          // 2. BACKUP: Read current progress
          // We use the current variables that track our position
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;

          section.reset();
          // 3. WIPE: Clear the cache directory
          if (!epub->clearCache()) {
            LOG_ERR("ERS", "clearCache failed - partial cleanup only");
          }

          // 4. RESTORE: Re-setup the directory and rewrite the progress file
          epub->setupCacheDir();

          saveProgress(backupSpine, backupPage, backupPageCount);
        }
      }
      // Defer go home to avoid race condition with display task
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRANSLATE_CHAPTER: {
      if (epub && section) {
        pendingTranslateChapter = true;
        exitActivity();  // close the menu subactivity; loop() will launch ChapterTranslatorActivity
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRANSLATE_PAGE: {
      if (epub && section) {
        pendingTranslatePage = true;
        exitActivity();  // close the menu; loop() will launch PageTranslatorActivity
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        exitActivity();
        enterNewActivity(new KOReaderSyncActivity(
            renderer, mappedInput, epub, epub->getPath(), currentSpineIndex, currentPage, totalPages,
            [this]() {
              // On cancel - defer exit to avoid use-after-free
              pendingSubactivityExit = true;
            },
            [this](int newSpineIndex, int newPage) {
              // On sync complete - update position and defer exit
              if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
                currentSpineIndex = newSpineIndex;
                nextPageNumber = newPage;
                section.reset();
              }
              pendingSubactivityExit = true;
            }));
      }
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    applyReaderOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::applyTranslationMode(const uint8_t translationMode) {
  if (SETTINGS.colorTextStyle == translationMode) {
    return;
  }

  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.colorTextStyle = translationMode;
    SETTINGS.saveToFile();

    // Create or destroy tooltip overlay based on mode
    if (translationMode == CrossPointSettings::CT_TOOLTIP) {
      tooltipOverlay = std::make_unique<TooltipOverlay>();
    } else {
      tooltipOverlay.reset();
    }

    // Reset section to force re-layout with the new translation mode.
    section.reset();
  }
}

void EpubReaderActivity::applyFontFamily(const uint8_t fontFamily) {
  if (SETTINGS.fontFamily == fontFamily) {
    return;
  }

  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.fontFamily = fontFamily;
    SETTINGS.saveToFile();

    section.reset();
  }
}

void EpubReaderActivity::applyFontSize(const uint8_t fontSize) {
  if (SETTINGS.fontSize == fontSize) {
    return;
  }

  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.fontSize = fontSize;
    SETTINGS.saveToFile();

    section.reset();
  }
}

void EpubReaderActivity::applyLineSpacing(const uint8_t lineSpacing) {
  if (SETTINGS.lineSpacing == lineSpacing) {
    return;
  }

  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    SETTINGS.lineSpacing = lineSpacing;
    SETTINGS.saveToFile();

    section.reset();
  }
}

// TODO: Failure handling
void EpubReaderActivity::render(Activity::RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += SETTINGS.screenMargin;

  auto metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  if (SETTINGS.statusBar != CrossPointSettings::STATUS_BAR_MODE::NONE) {
    // Add additional margin for status bar if progress bar is shown
    const bool showProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR ||
                                 SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
    orientedMarginBottom += statusBarMargin - SETTINGS.screenMargin +
                            (showProgressBar ? (metrics.bookProgressBarHeight + progressBarMarginTop) : 0);
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

    // Fall back to Normal mode when this chapter has no translations, to avoid blank pages
    // when "Translation Only" or similar modes strip all untranslated text.
    const bool hasTranslation = section->hasTranslatedHtml() || epub->hasCalibreTranslation() ||
                                chapterHasEmbeddedTranslations(epub, currentSpineIndex);
    const uint8_t effectiveColorTextStyle =
        !hasTranslation                                                          ? CrossPointSettings::CT_NORMAL
        : SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP ? CrossPointSettings::CT_NO_RENDER
                                                                                 : SETTINGS.colorTextStyle;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  effectiveColorTextStyle)) {
      LOG_DBG("ERS", "Cache not found, building...");

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      effectiveColorTextStyle, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }

    // Set up tooltip data source: the HTML file that contains data-translation paragraphs.
    if (tooltipOverlay) {
      tooltipOverlay->onPageChanged();
      if (section->hasTranslatedHtml()) {
        tooltipOverlay->setTranslatedHtmlPath(section->getTranslatedHtmlPath());
      } else if (hasTranslation) {
        // Embedded/Calibre translations — extract chapter HTML from EPUB once.
        const auto tipPath =
            epub->getCachePath() + "/sections/" + std::to_string(currentSpineIndex) + ".tooltip.html";
        if (!Storage.exists(tipPath.c_str())) {
          const auto href = epub->getSpineItem(currentSpineIndex).href;
          FsFile tmpFile;
          if (Storage.openFileForWrite("TIP", tipPath, tmpFile)) {
            epub->readItemContentsToStream(href, tmpFile, 1024);
            tmpFile.close();
          }
        }
        tooltipOverlay->setTranslatedHtmlPath(tipPath);
      }
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
      // TODO: prevent infinite loop if the page keeps failing to load for some reason
      return;
    }
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = spineIndex & 0xFF;
    data[1] = (spineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
bool EpubReaderActivity::handleLongPressConfirm() {
  if (SETTINGS.longPressChapterSkip == CrossPointSettings::LP_TRANSLATION_MODES &&
      mappedInput.isPressed(MappedInputManager::Button::PageForward)) {
    // OK + PageForward: switch to Landscape CCW + Side by Side
    applyOrientation(CrossPointSettings::ORIENTATION::LANDSCAPE_CCW);
    applyTranslationMode(CrossPointSettings::CT_SIDE_BY_SIDE);
    skipNextButtonCheck = true;
    requestUpdate();
    return true;
  }
  if (SETTINGS.longPressChapterSkip == CrossPointSettings::LP_TRANSLATION_MODES &&
      mappedInput.isPressed(MappedInputManager::Button::PageBack)) {
    // OK + PageBack: switch to Portrait + Original Only
    applyOrientation(CrossPointSettings::ORIENTATION::PORTRAIT);
    applyTranslationMode(CrossPointSettings::CT_NO_RENDER);
    skipNextButtonCheck = true;
    requestUpdate();
    return true;
  }
  if (SETTINGS.longPressOk == CrossPointSettings::LP_OK_TRANSLATE_PAGE) {
    // Translate Page mode: launch page translator in Normal or Original Only
    if (SETTINGS.colorTextStyle == CrossPointSettings::CT_NORMAL ||
        SETTINGS.colorTextStyle == CrossPointSettings::CT_NO_RENDER) {
      pendingTranslatePage = true;
      return true;
    }
    return false;
  }

  // Default: Toggle Translation — switch Original ↔ Translation
  if (SETTINGS.colorTextStyle == CrossPointSettings::CT_NO_RENDER) {
    applyTranslationMode(CrossPointSettings::CT_INVERT);
    requestUpdate();
    return true;
  }
  if (SETTINGS.colorTextStyle == CrossPointSettings::CT_INVERT) {
    applyTranslationMode(CrossPointSettings::CT_NO_RENDER);
    requestUpdate();
    return true;
  }
  return false;
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  // Force full refresh for pages with images when anti-aliasing is on,
  // as grayscale tones require half refresh to display correctly
  bool forceFullRefresh = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  // Render tooltip overlay if active
  if (tooltipOverlay && tooltipOverlay->isActive()) {
    const int tooltipFontId = getTooltipFontId();
    const int vpWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int vpHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    tooltipOverlay->render(renderer, *page, SETTINGS.getReaderFontId(), tooltipFontId, orientedMarginLeft,
                           orientedMarginTop, vpWidth, vpHeight, section->currentPage, section->pageCount);
  }
  renderStatusBar(orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  if (forceFullRefresh || pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  // restore the bw data
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  // determine visible status bar elements
  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBookProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                   SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  // Position status bar near the bottom of the logical screen, regardless of orientation
  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  // Calculate progress in book
  const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Hide percentage when progress bar is shown to reduce clutter
    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", section->currentPage + 1, section->pageCount,
               bookProgress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", section->currentPage + 1, section->pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showBookProgressBar) {
    // Draw progress bar at the very bottom of the screen, from edge to edge of viewable area
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(bookProgress));
  }

  if (showChapterProgressBar) {
    // Draw chapter progress bar at the very bottom of the screen, from edge to edge of viewable area
    const float chapterProgress =
        (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100 : 0;
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(chapterProgress));
  }

  if (showBattery) {
    GUI.drawBatteryLeft(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
  }

  if (showChapterTitle) {
    // Centered chatper title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int titleMarginRight = progressTextWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

    std::string title;
    int titleWidth;
    if (tocIndex == -1) {
      title = tr(STR_UNNAMED);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        // Not enough space to center on the screen, center it within the remaining space instead
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}
