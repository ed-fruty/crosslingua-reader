#include "LibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {

void sortEntries(std::vector<LibraryEntry>& entries) {
  std::sort(begin(entries), end(entries), [](const LibraryEntry& a, const LibraryEntry& b) {
    // Directories first
    if (a.isDirectory != b.isDirectory) return a.isDirectory;

    const char* s1 = a.filename.c_str();
    const char* s2 = b.filename.c_str();

    while (*s1 && *s2) {
      if (isdigit(*s1) && isdigit(*s2)) {
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        if (len1 != len2) return len1 < len2;

        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        s1 += len1;
        s2 += len2;
      } else {
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    return *s1 == '\0' && *s2 != '\0';
  });
}

bool isSupportedFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
         StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
         StringUtils::checkFileExtension(filename, ".md");
}

}  // namespace

void LibraryActivity::loadFiles() {
  entries.clear();

  auto dir = Storage.open(basepath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  dir.rewindDirectory();
  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    std::string fullPath = basepath;
    if (fullPath.back() != '/') fullPath += "/";
    fullPath += name;

    auto filename = std::string(name);

    if (file.isDirectory()) {
      entries.push_back({filename, fullPath, "", "", true, true});
    } else if (isSupportedFile(filename)) {
      entries.push_back({filename, fullPath, "", "", false, false});
    }
    file.close();
  }
  dir.close();
  sortEntries(entries);
}

int LibraryActivity::getPageOffset() const {
  return (static_cast<int>(selectorIndex) / GRID_PAGE_ITEMS) * GRID_PAGE_ITEMS;
}

std::string LibraryActivity::getDisplayTitle(int index) const {
  if (index < 0 || index >= static_cast<int>(entries.size())) return "";
  const auto& entry = entries[index];
  if (entry.isDirectory) return entry.filename;
  if (showTitles && !entry.title.empty()) return entry.title;
  // Strip extension from filename
  auto dotPos = entry.filename.rfind('.');
  if (dotPos != std::string::npos) {
    return entry.filename.substr(0, dotPos);
  }
  return entry.filename;
}

size_t LibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].filename == name) return i;
  }
  return 0;
}

void LibraryActivity::generateCoversForPage() {
  const int offset = getPageOffset();
  const int end = std::min(offset + GRID_PAGE_ITEMS, static_cast<int>(entries.size()));

  // Quick pass: resolve already-cached entries without showing popup
  int uncachedCount = 0;
  for (int i = offset; i < end; i++) {
    auto& entry = entries[i];
    if (entry.coverProcessed) continue;

    // Directories and text files have no cover
    if (entry.isDirectory || StringUtils::checkFileExtension(entry.filename, ".txt") ||
        StringUtils::checkFileExtension(entry.filename, ".md")) {
      entry.coverProcessed = true;
      continue;
    }

    // Check if this book already has a cached thumb
    if (StringUtils::checkFileExtension(entry.filename, ".epub")) {
      Epub probe(entry.fullPath, "/.crosspoint");
      const auto thumbPath = probe.getThumbBmpPath(thumbHeight);
      if (Storage.exists(thumbPath.c_str())) {
        FsFile check;
        if (Storage.openFileForRead("LIB", thumbPath, check)) {
          if (check.size() > 0) {
            entry.thumbPath = thumbPath;
          }
          check.close();
        }
        // Also load title from cache
        if (probe.loadMetadataOnly()) {
          if (!probe.getTitle().empty()) entry.title = probe.getTitle();
        }
        entry.coverProcessed = true;
        continue;
      }
    } else if (StringUtils::checkFileExtension(entry.filename, ".xtch") ||
               StringUtils::checkFileExtension(entry.filename, ".xtc")) {
      Xtc probe(entry.fullPath, "/.crosspoint");
      const auto thumbPath = probe.getThumbBmpPath(thumbHeight);
      if (Storage.exists(thumbPath.c_str())) {
        FsFile check;
        if (Storage.openFileForRead("LIB", thumbPath, check)) {
          if (check.size() > 0) {
            entry.thumbPath = thumbPath;
          }
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

  // Show progress popup only for genuinely uncached books
  auto popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));

  int processed = 0;
  for (int i = offset; i < end; i++) {
    auto& entry = entries[i];
    if (entry.coverProcessed) continue;

    if (StringUtils::checkFileExtension(entry.filename, ".epub")) {
      Epub epub(entry.fullPath, "/.crosspoint");
      if (epub.loadMetadataOnly()) {
        if (!epub.getTitle().empty()) {
          entry.title = epub.getTitle();
        }
        if (epub.generateThumbBmp(thumbHeight)) {
          const auto thumbPath = epub.getThumbBmpPath(thumbHeight);
          if (Storage.exists(thumbPath.c_str())) {
            FsFile check;
            if (Storage.openFileForRead("LIB", thumbPath, check)) {
              if (check.size() > 0) {
                entry.thumbPath = thumbPath;
              }
              check.close();
            }
          }
        }
      }
    } else if (StringUtils::checkFileExtension(entry.filename, ".xtch") ||
               StringUtils::checkFileExtension(entry.filename, ".xtc")) {
      Xtc xtc(entry.fullPath, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.getTitle().empty()) {
          entry.title = xtc.getTitle();
        }
        if (xtc.generateThumbBmp(thumbHeight)) {
          const auto thumbPath = xtc.getThumbBmpPath(thumbHeight);
          if (Storage.exists(thumbPath.c_str())) {
            FsFile check;
            if (Storage.openFileForRead("LIB", thumbPath, check)) {
              if (check.size() > 0) {
                entry.thumbPath = thumbPath;
              }
              check.close();
            }
          }
        }
      }
    }

    entry.coverProcessed = true;
    processed++;

    int progress = processed * 100 / uncachedCount;
    GUI.fillPopupProgress(renderer, popupRect, progress);
  }

  coversLoaded = true;
  coversLoading = false;
  gridBufferPage = -1;  // Force fresh render with new covers
  requestUpdate();
}

void LibraryActivity::onEnter() {
  Activity::onEnter();

  // Compute thumb height from actual grid layout so covers are generated at display size (no scaling)
  auto metrics = UITheme::getInstance().getMetrics();
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

  requestUpdate();
}

void LibraryActivity::storeGridBuffer(int page) {
  freeGridBuffer();
  const size_t bufferSize = GfxRenderer::getBufferSize();
  gridBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (gridBuffer) {
    memcpy(gridBuffer, renderer.getFrameBuffer(), bufferSize);
    gridBufferPage = page;
  }
}

void LibraryActivity::freeGridBuffer() {
  if (gridBuffer) {
    free(gridBuffer);
    gridBuffer = nullptr;
  }
  gridBufferPage = -1;
}

void LibraryActivity::onExit() {
  Activity::onExit();
  freeGridBuffer();
  entries.clear();
}

void LibraryActivity::loop() {
  // Long press BACK (1s+) goes to root folder
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
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        auto lastSlash = basepath.find_last_of('/');
        basepath = (lastSlash == 0) ? "/" : basepath.substr(0, lastSlash);

        loadFiles();
        coversLoaded = false;
        coversLoading = false;
        gridBufferPage = -1;

        // Restore selector to the directory we just left
        const auto dirName = oldPath.substr(oldPath.find_last_of('/') + 1);
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else {
        onGoHome();
      }
    }
    return;
  }

  int listSize = static_cast<int>(entries.size());
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

  // If page changed, need to regenerate covers
  if (getPageOffset() != oldPage) {
    coversLoaded = false;
    coversLoading = false;
    gridBufferPage = -1;
  }
}

void LibraryActivity::render(Activity::RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

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

  // Load covers/titles BEFORE drawing so titles are available on first render
  if (!coversLoaded && !coversLoading) {
    coversLoading = true;
    generateCoversForPage();
  }

  const bool samePage = (gridBufferPage == pageOffset && gridBuffer != nullptr);

  if (samePage && !entries.empty()) {
    // FAST PATH: restore clean buffer, overlay selection only (1 BMP read)
    memcpy(renderer.getFrameBuffer(), gridBuffer, GfxRenderer::getBufferSize());
    GUI.drawCoverGridSelection(renderer, gridRect, static_cast<int>(entries.size()),
                               static_cast<int>(selectorIndex), pageOffset, getTitle, getThumbPath, getIsDirectory);
    renderer.displayBuffer();
  } else {
    // FULL PATH: clear + render everything
    renderer.clearScreen();

    auto headerTitle = basepath == "/" ? tr(STR_LIBRARY) : basepath.substr(basepath.rfind('/') + 1).c_str();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);

    if (entries.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_BOOKS_FOUND));
    } else {
      // Draw grid without selection (selectedIndex=-1) to get a clean buffer
      GUI.drawCoverGrid(renderer, gridRect, static_cast<int>(entries.size()), -1, pageOffset, getTitle, getThumbPath,
                        getIsDirectory);
    }

    const auto labels = mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), tr(STR_OPEN),
                                              tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    // Store clean buffer (before selection overlay)
    storeGridBuffer(pageOffset);

    // Now overlay selection
    if (!entries.empty()) {
      GUI.drawCoverGridSelection(renderer, gridRect, static_cast<int>(entries.size()),
                                 static_cast<int>(selectorIndex), pageOffset, getTitle, getThumbPath, getIsDirectory);
    }

    renderer.displayBuffer();
  }
}
