#include "BookShelfActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;

void sortEntries(std::vector<BookShelfEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const BookShelfEntry& a, const BookShelfEntry& b) {
    // Directories first, then numeric-aware, case-insensitive by filename
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    return FsHelpers::naturalLess(a.filename, b.filename);
  });
}

bool isSupportedFile(const std::string& filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename);
}
}  // namespace

void BookShelfActivity::loadFiles() {
  entries.clear();

  if (!fileNameBuffer) {
    LOG_ERR("BSHELF", "fileNameBuffer not allocated");
    return;
  }

  auto dir = Storage.open(basepath.c_str());
  if (!dir || !dir.isDirectory()) {
    return;
  }

  dir.rewindDirectory();
  entries.reserve(32);  // conservative; grows if the folder is larger
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    if (fileNameBuffer[0] == '.' || strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    std::string filename(fileNameBuffer.get());
    std::string fullPath = basepath;
    if (fullPath.back() != '/') fullPath += "/";
    fullPath += filename;

    if (file.isDirectory()) {
      entries.push_back({filename, fullPath, "", "", true, true});
    } else if (isSupportedFile(filename)) {
      entries.push_back({filename, fullPath, "", "", false, false});
    }
  }
  sortEntries(entries);
}

int BookShelfActivity::getPageOffset() const {
  return (static_cast<int>(selectorIndex) / GRID_PAGE_ITEMS) * GRID_PAGE_ITEMS;
}

std::string BookShelfActivity::getDisplayTitle(int index) const {
  if (index < 0 || index >= static_cast<int>(entries.size())) return "";
  const auto& entry = entries[index];
  if (entry.isDirectory) return entry.filename;
  if (showTitles && !entry.title.empty()) return entry.title;
  // Strip extension from filename
  const auto dotPos = entry.filename.rfind('.');
  if (dotPos != std::string::npos) return entry.filename.substr(0, dotPos);
  return entry.filename;
}

size_t BookShelfActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].filename == name) return i;
  }
  return 0;
}

void BookShelfActivity::generateCoversForPage() {
  // Free the stale page cache BEFORE the metadata/thumb heap spike (memory audit portRule): the
  // 48KB grid snapshot must not coexist with the metadata build + streaming thumb conversion.
  freeGridBuffer();

  const int offset = getPageOffset();
  const int end = std::min(offset + GRID_PAGE_ITEMS, static_cast<int>(entries.size()));

  // Quick pass: resolve already-cached entries without showing a popup
  int uncachedCount = 0;
  for (int i = offset; i < end; i++) {
    auto& entry = entries[i];
    if (entry.coverProcessed) continue;

    // Directories and text files have no cover
    if (entry.isDirectory || FsHelpers::hasTxtExtension(entry.filename) ||
        FsHelpers::hasMarkdownExtension(entry.filename)) {
      entry.coverProcessed = true;
      continue;
    }

    if (FsHelpers::hasEpubExtension(entry.filename)) {
      Epub probe(entry.fullPath, "/.crosspoint");
      const auto thumbPath = probe.getThumbBmpPath(thumbHeight);
      if (Storage.exists(thumbPath.c_str())) {
        HalFile check;
        if (Storage.openFileForRead("BSHELF", thumbPath, check)) {
          // A 0-byte file is develop's negative-cache sentinel (no cover) -> keep thumbPath empty
          if (check.size() > 0) entry.thumbPath = thumbPath;
          check.close();
        }
        // Load the cached title. buildIfMissing=true is safe/fast here: an existing thumb implies
        // book.bin was already built, so this loads from cache. skipLoadingCss=true (title only).
        if (probe.load(true, true) && !probe.getTitle().empty()) entry.title = probe.getTitle();
        entry.coverProcessed = true;
        continue;
      }
    } else if (FsHelpers::hasXtcExtension(entry.filename)) {
      Xtc probe(entry.fullPath, "/.crosspoint");
      const auto thumbPath = probe.getThumbBmpPath(thumbHeight);
      if (Storage.exists(thumbPath.c_str())) {
        HalFile check;
        if (Storage.openFileForRead("BSHELF", thumbPath, check)) {
          if (check.size() > 0) entry.thumbPath = thumbPath;
          check.close();
        }
        entry.coverProcessed = true;
        continue;
      }
    }

    uncachedCount++;
  }

  if (uncachedCount == 0) {
    coversLoaded = true;
    coversLoading = false;
    return;
  }

  // Progress popup only for genuinely uncached books (metadata build + thumb conversion)
  const auto popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));

  int processed = 0;
  for (int i = offset; i < end; i++) {
    auto& entry = entries[i];
    if (entry.coverProcessed) continue;

    if (FsHelpers::hasEpubExtension(entry.filename)) {
      Epub epub(entry.fullPath, "/.crosspoint");
      // buildIfMissing=true so never-opened books still get a title/cover (develop has no lightweight
      // metadata-only probe); skipLoadingCss=true since a title/thumbnail needs no CSS.
      if (epub.load(true, true)) {
        if (!epub.getTitle().empty()) entry.title = epub.getTitle();
        if (epub.generateThumbBmp(thumbHeight)) {
          const auto thumbPath = epub.getThumbBmpPath(thumbHeight);
          HalFile check;
          if (Storage.openFileForRead("BSHELF", thumbPath, check)) {
            if (check.size() > 0) entry.thumbPath = thumbPath;
            check.close();
          }
        }
      }
    } else if (FsHelpers::hasXtcExtension(entry.filename)) {
      Xtc xtc(entry.fullPath, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.getTitle().empty()) entry.title = xtc.getTitle();
        if (xtc.generateThumbBmp(thumbHeight)) {
          const auto thumbPath = xtc.getThumbBmpPath(thumbHeight);
          HalFile check;
          if (Storage.openFileForRead("BSHELF", thumbPath, check)) {
            if (check.size() > 0) entry.thumbPath = thumbPath;
            check.close();
          }
        }
      }
    }

    entry.coverProcessed = true;
    processed++;
    GUI.fillPopupProgress(renderer, popupRect, processed * 100 / uncachedCount);
  }

  coversLoaded = true;
  coversLoading = false;
}

void BookShelfActivity::onEnter() {
  Activity::onEnter();

  // Drain any in-flight async refresh inherited from the previous activity before we touch the
  // framebuffer (SSD1677 wedge guard; a documented no-op when nothing is pending).
  renderer.waitRefreshComplete();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("BSHELF", "OOM: name buffer");
    return;
  }

  // Compute thumb height from the actual grid layout so covers are generated at display size
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int cellHeight = contentHeight / GRID_ROWS;
  thumbHeight = cellHeight - GRID_CELL_PADDING * 2 - GRID_TITLE_AREA;

  loadFiles();
  selectorIndex = 0;
  coversLoaded = false;
  coversLoading = false;
  gridBufferPage = -1;

  // If Confirm was held while this activity opened (typical when launched from the menu), ignore
  // its release so we don't immediately auto-open entry 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  requestUpdate();
}

void BookShelfActivity::storeGridBuffer(int page) {
  freeGridBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  gridBuffer = makeUniqueNoThrow<uint8_t[]>(bufferSize);
  if (gridBuffer) {
    memcpy(gridBuffer.get(), renderer.getFrameBuffer(), bufferSize);
    gridBufferPage = page;
  } else {
    // OOM: no fast-path cache; render() degrades to a full redraw on every selection move.
    LOG_ERR("BSHELF", "OOM: grid page cache (%u bytes)", (unsigned)bufferSize);
    gridBufferPage = -1;
  }
}

void BookShelfActivity::freeGridBuffer() {
  gridBuffer.reset();
  gridBufferPage = -1;
}

void BookShelfActivity::onExit() {
  Activity::onExit();
  freeGridBuffer();
  entries.clear();
  fileNameBuffer.reset();
}

void BookShelfActivity::loop() {
  // Long press BACK (1s+) jumps to the root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    coversLoaded = false;
    coversLoading = false;
    gridBufferPage = -1;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Swallow the release of the Confirm press that launched this activity from the home menu.
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (entries.empty()) return;

    // Long-press Confirm: toggle title/filename display
    if (mappedInput.getHeldTime() >= TOGGLE_DISPLAY_MS) {
      showTitles = !showTitles;
      gridBufferPage = -1;
      requestUpdate();
      return;
    }

    // Short press: open directory or book
    const auto& entry = entries[selectorIndex];
    if (entry.isDirectory) {
      basepath = entry.fullPath;
      loadFiles();
      selectorIndex = 0;
      coversLoaded = false;
      coversLoading = false;
      gridBufferPage = -1;
      requestUpdate();
    } else {
      onSelectBook(entry.fullPath);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if already at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        const auto lastSlash = basepath.find_last_of('/');
        basepath = (lastSlash == 0) ? "/" : basepath.substr(0, lastSlash);

        loadFiles();
        coversLoaded = false;
        coversLoading = false;
        gridBufferPage = -1;

        // Restore the selector to the directory we just left
        const auto dirName = oldPath.substr(oldPath.find_last_of('/') + 1);
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else {
        onGoHome();
      }
    }
    return;
  }

  const int listSize = static_cast<int>(entries.size());
  const int oldPage = getPageOffset();

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, GRID_PAGE_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, GRID_PAGE_ITEMS);
    requestUpdate();
  });

  // If the page changed, regenerate covers for the new page
  if (getPageOffset() != oldPage) {
    coversLoaded = false;
    coversLoading = false;
    gridBufferPage = -1;
  }
}

void BookShelfActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect gridRect{0, contentTop, pageWidth, contentHeight};
  const int pageOffset = getPageOffset();

  auto getTitle = [this](int index) { return getDisplayTitle(index); };
  auto getThumbPath = [this](int index) -> std::string {
    if (index < 0 || index >= static_cast<int>(entries.size())) return "";
    return entries[index].thumbPath;
  };
  auto getIsDirectory = [this](int index) -> bool {
    if (index < 0 || index >= static_cast<int>(entries.size())) return false;
    return entries[index].isDirectory;
  };

  // Generate covers/titles BEFORE drawing so titles are available on the first paint
  if (!coversLoaded && !coversLoading) {
    coversLoading = true;
    generateCoversForPage();
  }

  const bool samePage = (gridBufferPage == pageOffset && gridBuffer != nullptr);

  if (samePage && !entries.empty()) {
    // FAST PATH: restore the cached clean page, overlay only the selected cell (single refresh)
    memcpy(renderer.getFrameBuffer(), gridBuffer.get(), renderer.getBufferSize());
    GUI.drawCoverGridSelection(renderer, gridRect, static_cast<int>(entries.size()), static_cast<int>(selectorIndex),
                               pageOffset, getTitle, getThumbPath, getIsDirectory);
    renderer.displayBuffer();
    return;
  }

  // FULL PATH: clear + render everything (single refresh)
  renderer.clearScreen();

  // Own a std::string for the header so its c_str() outlives the drawHeader call
  std::string headerTitle =
      (basepath == "/") ? std::string(tr(STR_BOOKSHELF)) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle.c_str());

  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_BOOKS_FOUND));
  } else {
    // Draw the grid without a selection (selectedIndex = -1) to obtain a clean buffer
    GUI.drawCoverGrid(renderer, gridRect, static_cast<int>(entries.size()), -1, pageOffset, getTitle, getThumbPath,
                      getIsDirectory);
  }

  const auto labels = mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (!entries.empty()) {
    // Snapshot the clean buffer (before the selection overlay) for fast selection moves
    storeGridBuffer(pageOffset);
    GUI.drawCoverGridSelection(renderer, gridRect, static_cast<int>(entries.size()), static_cast<int>(selectorIndex),
                               pageOffset, getTitle, getThumbPath, getIsDirectory);
  }

  renderer.displayBuffer();
}
