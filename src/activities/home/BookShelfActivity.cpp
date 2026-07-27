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

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;

// System/tool folders that are never books; the shelf hides them regardless of the
// hidden-files setting (matches the spirit of WebDAVHandler's HIDDEN_ITEMS).
constexpr const char* HIDDEN_ENTRIES[] = {"System Volume Information", "XTCache", "config"};

bool isHiddenSystemEntry(const char* name) {
  for (const auto* item : HIDDEN_ENTRIES) {
    if (strcmp(name, item) == 0) return true;
  }
  return false;
}

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
    // Dot-entries follow the browser's hidden-files setting; known system folders never show.
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') || isHiddenSystemEntry(fileNameBuffer.get())) {
      continue;
    }

    std::string filename(fileNameBuffer.get());
    std::string fullPath = basepath;
    if (fullPath.back() != '/') fullPath += "/";
    fullPath += filename;

    if (file.isDirectory()) {
      entries.push_back({filename, fullPath, "", "", true, true});
    } else if (isSupportedFile(filename)) {
      // coverProcessed starts true for formats that never carry a cover (txt/md) so they render as
      // a plain title-only cell immediately; epub/xtc start pending (false) until the worker (or a
      // pre-existing SD thumb) resolves them, which is what draws the loading placeholder.
      const bool coverEligible = FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename);
      entries.push_back({filename, fullPath, "", "", false, !coverEligible});
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

bool BookShelfActivity::pageHasPendingCovers() const {
  const int offset = getPageOffset();
  const int end = std::min(offset + GRID_PAGE_ITEMS, static_cast<int>(entries.size()));
  for (int i = offset; i < end; i++) {
    if (!entries[i].coverProcessed) return true;
  }
  return false;
}

// ─── render task: adopt thumbs the worker has produced ────────────────────────
// Runs inside render() (render task, under the RenderLock). It is the ONLY writer of entry cover
// fields, so drawCoverGrid's reads never race. For each still-pending, cover-eligible visible entry
// whose thumb the worker has already written to SD, it sets thumbPath/title and marks it processed;
// entries still awaiting the worker stay pending (their loading placeholder keeps showing). Cheap
// and idempotent: resolved entries are skipped, so after the initial fill it is a no-op scan.
void BookShelfActivity::resolveCachedCovers() {
  const int offset = getPageOffset();
  const int end = std::min(offset + GRID_PAGE_ITEMS, static_cast<int>(entries.size()));
  for (int i = offset; i < end; i++) {
    auto& entry = entries[i];
    // Live read of the volatile each iteration: never adopt a thumb the worker is mid-writing.
    if (entry.coverProcessed || i == workerActiveIndex) continue;

    const bool epub = FsHelpers::hasEpubExtension(entry.filename);
    const bool xtc = FsHelpers::hasXtcExtension(entry.filename);
    if (!epub && !xtc) {  // defensive: non-cover formats are pre-marked processed in loadFiles()
      entry.coverProcessed = true;
      continue;
    }

    std::string thumbPath;
    if (epub) {
      Epub probe(entry.fullPath, "/.crosspoint");
      thumbPath = probe.getThumbBmpPath(thumbHeight);
      if (!Storage.exists(thumbPath.c_str())) continue;  // worker hasn't reached it yet -> stay pending
      // buildIfMissing=false: never build on the render task -- this is a cheap cache read
      // (skipLoadingCss=true, title only). The thumb's existence implies the worker built a book.bin
      // at some point, but NOT that it is still readable: thumb_<h>.bmp is unversioned while book.bin
      // is not, so a BOOK_CACHE_VERSION bump orphans the title of every already-thumbnailed book.
      // load() then fails and getDisplayTitle() falls back to the filename until that book is next
      // opened in the reader (which rebuilds book.bin). Cosmetic only; the cover still draws.
      if (probe.load(false, true) && !probe.getTitle().empty()) entry.title = probe.getTitle();
    } else {
      Xtc probe(entry.fullPath, "/.crosspoint");
      thumbPath = probe.getThumbBmpPath(thumbHeight);
      if (!Storage.exists(thumbPath.c_str())) continue;
    }

    // Thumb exists: a >0-byte file is a real cover; a 0-byte file is the negative-cache sentinel
    // (no drawable cover) -> leave thumbPath empty so the cell falls back to a title-only cell.
    HalFile check;
    size_t thumbSize = 0;
    const bool opened = Storage.openFileForRead("BSHELF", thumbPath, check);
    if (opened) {
      thumbSize = check.size();
      check.close();
    }
    // Re-check AFTER reading the size: the worker sets workerActiveIndex before its first write,
    // so if it picked this entry between our top-of-loop check and the read, the size we saw may
    // be a just-created, still-empty file -- not the sentinel. Retry on the next pass.
    if (i == workerActiveIndex) continue;
    if (opened && thumbSize > 0) entry.thumbPath = thumbPath;
    entry.coverProcessed = true;
  }
}

// ─── cover worker (background FreeRTOS task) ──────────────────────────────────
// Generates the visible page's missing thumbs on SD, one book at a time, following the user's page.
// It touches ONLY HalStorage and heap — never the renderer/framebuffer/display — so it can run while
// the main task navigates and the render task paints. entries[] is stable for its whole life: the
// main task join/cancels it before any loadFiles() (see joinCoverWorker), so reading immutable
// fields (fullPath/filename/size) is safe without a lock.

void BookShelfActivity::coverWorkerTrampoline(void* param) {
  auto* self = static_cast<BookShelfActivity*>(param);
  self->coverWorkerLoop();
  vTaskDelete(nullptr);
}

bool BookShelfActivity::generateOneCover(int index, const std::string& thumbPath) {
  const std::string path = entries[index].fullPath;  // immutable copy; vector is stable while we run
  const bool epub = FsHelpers::hasEpubExtension(entries[index].filename);

  const unsigned long t0 = millis();
  const size_t heapBefore = ESP.getFreeHeap();
  bool ok = false;

  if (epub) {
    Epub book(path, "/.crosspoint");
    // buildIfMissing=true: a never-opened book must still get metadata built; skipLoadingCss=true.
    if (book.load(true, true)) ok = book.generateThumbBmp(thumbHeight);
  } else {
    Xtc book(path, "/.crosspoint");
    if (book.load()) ok = book.generateThumbBmp(thumbHeight);
  }

  // Guarantee this entry never re-queues: if generation produced no file at all (e.g. the metadata
  // build failed so generateThumbBmp bailed before writing its own sentinel), drop a 0-byte negative
  // sentinel ourselves. The render task then reads it as "processed, no drawable cover".
  if (!thumbPath.empty() && !Storage.exists(thumbPath.c_str())) {
    HalFile sentinel;
    if (Storage.openFileForWrite("BSHELF", thumbPath, sentinel)) sentinel.close();
  }

  LOG_DBG("BSHELF", "cover[%d] gen %lums heap %u->%u ok=%d", index, millis() - t0, (unsigned)heapBefore,
          (unsigned)ESP.getFreeHeap(), ok);
  // Success for the QUEUE means "a file exists now" (real thumb or sentinel): the worker settles the
  // slot so it is never scanned again this page visit. Decode failure with a sentinel still counts;
  // only a failed sentinel write (SD full/error) reports false.
  return !thumbPath.empty() && Storage.exists(thumbPath.c_str());
}

void BookShelfActivity::coverWorkerLoop() {
  int scannedPage = -1;
  bool pageComplete = false;
  // Per-page cache, (re)computed once whenever the visible 9-item window changes: each slot's thumb
  // BMP path and whether the slot is already settled (a thumb/sentinel is on SD, or the entry is a
  // folder / non-cover format). This replaces the old per-pass O(page^2) rescan that reconstructed
  // an Epub/Xtc probe and Storage.exists()'d every earlier slot on every worker iteration.
  std::string slotThumbPath[GRID_PAGE_ITEMS];
  bool slotSettled[GRID_PAGE_ITEMS] = {};

  while (!coverCancelFlag) {
    const int offset = visiblePageOffset;
    if (offset != scannedPage) {  // user turned the page: chase it, recompute the slot cache once
      scannedPage = offset;
      pageComplete = false;
      const int end = std::min(offset + GRID_PAGE_ITEMS, static_cast<int>(entries.size()));
      for (int s = 0; s < GRID_PAGE_ITEMS; s++) {
        const int i = offset + s;
        slotThumbPath[s].clear();
        if (i >= end) {  // past the end of the listing
          slotSettled[s] = true;
          continue;
        }
        const auto& entry = entries[i];
        const bool epub = FsHelpers::hasEpubExtension(entry.filename);
        const bool xtc = FsHelpers::hasXtcExtension(entry.filename);
        if (entry.isDirectory || (!epub && !xtc)) {  // folders / non-cover formats never generate
          slotSettled[s] = true;
          continue;
        }
        // Probe ctors are cheap (they only hash the path); done once per page visit, not per pass.
        slotThumbPath[s] = epub ? Epub(entry.fullPath, "/.crosspoint").getThumbBmpPath(thumbHeight)
                                : Xtc(entry.fullPath, "/.crosspoint").getThumbBmpPath(thumbHeight);
        // A thumb/sentinel already on SD (prior session) settles the slot without decoding.
        slotSettled[s] = Storage.exists(slotThumbPath[s].c_str());
      }
    }

    coverPageActive = !pageComplete;  // keep the CPU clock high (skipLoopDelay) only while work remains
    if (pageComplete) {               // nothing left on this page: idle with zero SD/decode activity (battery)
      vTaskDelay(pdMS_TO_TICKS(COVER_IDLE_TICK_MS));
      continue;
    }

    // Heap gate: never decode into a starved heap — leave the floor for the UI and retry later.
    if (ESP.getFreeHeap() < COVER_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < COVER_MIN_MAX_ALLOC) {
      vTaskDelay(pdMS_TO_TICKS(COVER_GATE_DELAY_MS));
      continue;
    }

    int slot = -1;  // first unsettled slot: an in-memory scan, no SD I/O
    for (int s = 0; s < GRID_PAGE_ITEMS; s++) {
      if (!slotSettled[s]) {
        slot = s;
        break;
      }
    }
    if (slot < 0) {  // every visible cover exists (or failed this visit) -> idle until page change
      pageComplete = true;
      coverPageActive = false;
      continue;
    }

    const int target = offset + slot;
    workerActiveIndex = target;  // tell the render task to skip this thumb while we write it
    const bool fileProduced = generateOneCover(target, slotThumbPath[slot]);
    workerActiveIndex = -1;
    // Settle the slot either way so it is never re-scanned this visit. A produced file (real thumb
    // or 0-byte sentinel) is genuinely done. A hard SD failure (neither landed) is skipped for this
    // page visit and retried when the user revisits the page — the rescan above re-stats it and,
    // finding no file, marks it unsettled again. This subsumes the old failedMask.
    slotSettled[slot] = true;
    if (!fileProduced) {
      LOG_ERR("BSHELF", "cover[%d]: no file produced; skipping for this visit", target);
      vTaskDelay(pdMS_TO_TICKS(COVER_GATE_DELAY_MS));
      continue;
    }
    coverDirty = true;  // a new thumb is on SD; the main task will schedule a throttled repaint
  }

  coverPageActive = false;
  coverTaskDone = true;  // last touch of `this` before the trampoline vTaskDelete()s us
}

void BookShelfActivity::startCoverWorker() {
  joinCoverWorker();  // never double-launch
  if (entries.empty()) return;

  coverCancelFlag = false;
  coverTaskDone = false;
  coverDirty = false;
  workerActiveIndex = -1;
  coverPageActive = false;
  visiblePageOffset = getPageOffset();

#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t coverTaskCore = 1;
#else
  constexpr BaseType_t coverTaskCore = 0;
#endif
  // Stack 10240: Epub::load (expat XML parse + zip inflate) plus the streaming BMP converter mirror
  // the work the render task already carries on its 8192 stack. Priority 1 (same as the main loop
  // and the render task): the covers only decode at full 160 MHz while skipLoopDelay() holds the
  // clock high, and skipLoopDelay() makes main.cpp yield()-spin the priority-1 main loop — a
  // priority-0 worker would be starved to 0% CPU under that spin. WDT-safe at priority 1: the task
  // watchdog's idle-task check is disabled (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 unset) and the
  // only TWDT subscriber is the webserver task, so a CPU-bound priority-1 worker feeds and trips
  // nothing — exactly as the fork decoded covers on its priority-1 render task and as this reader
  // runs its priority-1 incremental section build for tens of seconds. SD reads go through
  // HalStorage, which blocks (yielding the core) on the SPI bus, so higher-priority UI work still
  // preempts promptly. (A dual-core target pins it to core 1 instead.)
  xTaskCreatePinnedToCore(&coverWorkerTrampoline, "bshelfCover", 10240, this, 1, &coverTaskHandle, coverTaskCore);
  if (!coverTaskHandle) LOG_ERR("BSHELF", "OOM: cover worker task");
}

void BookShelfActivity::joinCoverWorker() {
  if (!coverTaskHandle) return;
  coverCancelFlag = true;
  // Wait until the worker actually exits: abandoning it on a timeout would leave a live task
  // dereferencing a deleted `this` (use-after-free). The wait is naturally bounded by ONE
  // in-flight cover decode -- the worker re-checks coverCancelFlag right after generateOneCover()
  // and in every idle/gate tick. Never taps the RenderLock, so this is deadlock-free even when
  // onExit() holds it.
  for (int i = 1; !coverTaskDone; i++) {
    delay(20);
    if (i % 250 == 0) LOG_ERR("BSHELF", "cover worker still joining after %ds", i / 50);
  }
  coverTaskHandle = nullptr;
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
  const int cellHeight = contentHeight / covergrid::GRID_ROWS;
  thumbHeight = cellHeight - covergrid::GRID_CELL_PADDING * 2 - covergrid::GRID_TITLE_AREA;

  loadFiles();
  selectorIndex = 0;
  lastSelectorIndex = 0;
  lastInteractionMs = millis();
  gridBufferPage = -1;
  coverWorkerPending = true;  // loop() launches the worker once the first paint is done

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
  // Stop and join the cover worker BEFORE tearing down entries[] (its backing store) so no
  // background SD/heap work outlives this activity or races the reader we may be launching into.
  // The worker never takes the RenderLock, so this join is safe even though onExit() holds it.
  joinCoverWorker();
  freeGridBuffer();
  entries.clear();
  fileNameBuffer.reset();
}

void BookShelfActivity::loop() {
  // Launch the cover worker only once the current listing's first paint is done (!peek()), so the
  // grid appears instantly with placeholders and the worker's first decode never contends with it.
  if (coverWorkerPending && !RenderLock::peek()) {
    coverWorkerPending = false;
    startCoverWorker();
  }

  // Long press BACK (1s+) jumps to the root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    joinCoverWorker();  // stop background gen before entries[] is rebuilt
    {
      // requestUpdate() is deferred, so a previous render can still be mid-flight on the render
      // task reading entries[]; rebuilding the vector under it is UB. Block it out for the rebuild.
      RenderLock lock;
      basepath = "/";
      loadFiles();
      selectorIndex = 0;
      gridBufferPage = -1;
    }
    coverWorkerPending = true;
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
      const std::string newPath = entry.fullPath;  // copy before the vector it points into is cleared
      joinCoverWorker();                           // stop background gen before entries[] is rebuilt
      {
        RenderLock lock;  // see go-home handler: no vector rebuild under an in-flight render
        basepath = newPath;
        loadFiles();
        selectorIndex = 0;
        gridBufferPage = -1;
      }
      coverWorkerPending = true;
      requestUpdate();
    } else {
      // Join first: the reader needs a clean heap, and no cover work must run while it allocates.
      joinCoverWorker();
      onSelectBook(entry.fullPath);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if already at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        joinCoverWorker();  // stop background gen before entries[] is rebuilt
        {
          RenderLock lock;  // see go-home handler: no vector rebuild under an in-flight render
          const auto lastSlash = basepath.find_last_of('/');
          basepath = (lastSlash == 0) ? "/" : basepath.substr(0, lastSlash);

          loadFiles();
          gridBufferPage = -1;

          // Restore the selector to the directory we just left
          const auto dirName = oldPath.substr(oldPath.find_last_of('/') + 1);
          selectorIndex = findEntry(dirName);
        }
        coverWorkerPending = true;
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

  // If the page changed, force a full repaint of the new window. The worker follows the visible
  // page on its own via visiblePageOffset (updated below) — no relaunch needed for a page turn.
  if (getPageOffset() != oldPage) {
    gridBufferPage = -1;
  }

  // Treat any selector movement as interaction: it holds off cover repaints (below) so rapid
  // scrolling stays crisp instead of contending with full cover redraws.
  if (selectorIndex != lastSelectorIndex) {
    lastSelectorIndex = selectorIndex;
    lastInteractionMs = millis();
  }

  // Point the worker at the window the user is actually looking at.
  visiblePageOffset = getPageOffset();

  // Progressive fill: when the worker has landed new thumbs and the user has paused, force a full
  // redraw so the freshly generated covers replace their placeholders. Throttled so a burst of
  // completions never floods the panel, and gated on an idle input window + a free render slot so a
  // cover repaint never delays a page turn.
  if (coverDirty && !RenderLock::peek()) {
    const unsigned long now = millis();
    if (now - lastInteractionMs >= COVER_REPAINT_IDLE_MS && now - lastCoverRepaintMs >= COVER_REPAINT_THROTTLE_MS) {
      coverDirty = false;
      lastCoverRepaintMs = now;
      gridBufferPage = -1;  // force the full path so resolveCachedCovers() adopts the new thumbs
      requestUpdate();
    }
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
  // Pending = a cover-bearing entry whose thumb the worker hasn't produced yet -> loading placeholder
  // (visually distinct from a processed book that simply has no cover, which draws as a title-only cell).
  auto getIsPending = [this](int index) -> bool {
    if (index < 0 || index >= static_cast<int>(entries.size())) return false;
    const auto& entry = entries[index];
    return !entry.isDirectory && !entry.coverProcessed;
  };

  // Adopt any thumbs the background worker has finished (render task owns entries[] cover fields).
  resolveCachedCovers();
  const bool pageComplete = !pageHasPendingCovers();

  const bool samePage = (gridBufferPage == pageOffset && gridBuffer != nullptr);

  if (samePage && !entries.empty()) {
    // FAST PATH: restore the cached clean page, overlay only the selected cell (single refresh).
    // Only reachable once the page is fully generated (we snapshot solely when complete), so the
    // cached buffer never captures a stale placeholder.
    memcpy(renderer.getFrameBuffer(), gridBuffer.get(), renderer.getBufferSize());
    GUI.drawCoverGridSelection(renderer, gridRect, static_cast<int>(entries.size()), static_cast<int>(selectorIndex),
                               pageOffset, getTitle, getThumbPath, getIsDirectory, getIsPending);
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
                      getIsDirectory, getIsPending);

    // Unobtrusive global progress caption in the free left of the header while covers still generate;
    // it disappears once the visible page is complete. Numbers are counts, "Loading" is translated.
    if (!pageComplete) {
      const int end = std::min(pageOffset + GRID_PAGE_ITEMS, static_cast<int>(entries.size()));
      int coverTotal = 0, coverDone = 0;
      for (int i = pageOffset; i < end; i++) {
        if (entries[i].isDirectory) continue;
        if (!FsHelpers::hasEpubExtension(entries[i].filename) && !FsHelpers::hasXtcExtension(entries[i].filename)) {
          continue;
        }
        coverTotal++;
        if (entries[i].coverProcessed) coverDone++;
      }
      if (coverTotal > 0) {
        char progressBuf[40];
        snprintf(progressBuf, sizeof(progressBuf), "%s %d/%d", tr(STR_LOADING), coverDone, coverTotal);
        renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, metrics.topPadding + 6, progressBuf);
      }
    }
  }

  const auto labels = mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (!entries.empty()) {
    if (pageComplete) {
      // Snapshot the clean buffer (before the selection overlay) for fast selection moves.
      storeGridBuffer(pageOffset);
    } else {
      // Covers still generating: release the 48KB snapshot so the worker's metadata build + streaming
      // thumb conversion has heap headroom. Selection moves fall back to full redraws until complete.
      freeGridBuffer();
    }
    GUI.drawCoverGridSelection(renderer, gridRect, static_cast<int>(entries.size()), static_cast<int>(selectorIndex),
                               pageOffset, getTitle, getThumbPath, getIsDirectory, getIsPending);
  }

  renderer.displayBuffer();
}
