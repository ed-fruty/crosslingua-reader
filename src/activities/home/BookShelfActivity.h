#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/CoverGridLayout.h"
#include "util/ButtonNavigator.h"

struct BookShelfEntry {
  std::string filename;   // Original filename for sorting/display
  std::string fullPath;   // Full path to the file
  std::string title;      // From metadata (empty = use filename)
  std::string thumbPath;  // Path to cached thumb BMP (empty = no cover)
  bool isDirectory;
  bool coverProcessed;  // Whether metadata+thumb extraction was attempted
};

// Ported from the fork's cover-grid browser: a 3x3 cover-thumbnail grid browser over the SD card
// (folders + epub/xtc/txt/md), with lazy per-page thumbnail generation, a full-framebuffer page
// cache for fast selection moves, long-press Confirm to toggle title/filename, long-press Back to
// jump to root, and folder navigation. Adapted to develop's ActivityManager navigation, HalFile
// storage, develop image/thumb pipeline, and nothrow heap discipline.
class BookShelfActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  std::string basepath = "/";

  std::vector<BookShelfEntry> entries;
  std::unique_ptr<char[]> fileNameBuffer;

  // ─── Background cover generation ────────────────────────────────────────────
  // Covers are decoded off the UI by a FreeRTOS worker so navigation never blocks. The worker
  // NEVER touches the renderer/framebuffer/display — it only produces thumb BMPs on the SD card
  // (via HalStorage) for the currently visible page. The RENDER task owns entries[] entirely: it
  // resolves each cell's thumbPath/title from the SD file the worker produced (resolveCachedCovers)
  // and draws. The SD thumb file's existence IS the per-entry completion signal, so no entry field
  // is ever written by both tasks. All cross-task state below is volatile scalars, single-writer.
  TaskHandle_t coverTaskHandle = nullptr;
  volatile bool coverCancelFlag = false;  // main -> worker: stop at the next book boundary
  volatile bool coverTaskDone = false;    // worker -> main: loop exited, safe to null the handle
  volatile bool coverDirty = false;       // worker -> main: a new thumb landed on SD; repaint due
  volatile int visiblePageOffset = 0;     // main -> worker: which 9-item window to chase
  volatile int workerActiveIndex = -1;    // worker -> render: entry whose thumb is mid-write; skip it
  bool coverWorkerPending = false;        // main: (re)launch the worker once the next paint is done

  // Progressive-fill repaint pacing (main task).
  unsigned long lastCoverRepaintMs = 0;
  unsigned long lastInteractionMs = 0;
  size_t lastSelectorIndex = 0;

  // Background floors: skip generation when the heap can't absorb the metadata build + streaming
  // thumb conversion, leaving headroom for the UI (mirrors EpubReaderActivity's background build).
  static constexpr size_t COVER_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t COVER_MIN_MAX_ALLOC = 16 * 1024;
  static constexpr unsigned long COVER_IDLE_TICK_MS = 150;          // wake latency on page change when idle
  static constexpr unsigned long COVER_GATE_DELAY_MS = 250;         // retry cadence while heap-gated
  static constexpr unsigned long COVER_REPAINT_IDLE_MS = 400;       // quiet window before a cover repaint
  static constexpr unsigned long COVER_REPAINT_THROTTLE_MS = 1500;  // min gap between cover repaints
  static constexpr int COVER_JOIN_TICKS = 100;                      // 100 * 100ms = 10s bounded join

  void startCoverWorker();
  void joinCoverWorker();
  static void coverWorkerTrampoline(void* param);
  void coverWorkerLoop();
  bool generateOneCover(int index);             // worker: build book.bin + thumb (or 0-byte sentinel) on SD
  int nextPendingCoverIndex(int offset) const;  // worker: first visible cover-eligible entry lacking a thumb
  void resolveCachedCovers();                   // render task: adopt thumbs the worker produced
  bool pageHasPendingCovers() const;            // any visible entry still awaiting a cover

  // True when this activity was entered while Confirm was still held (typical when launched from
  // the home menu); swallow that first release so we don't immediately open entry 0.
  bool lockNextConfirmRelease = false;

  // Grid buffer cache for fast same-page navigation (full framebuffer snapshot)
  std::unique_ptr<uint8_t[]> gridBuffer;
  int gridBufferPage = -1;

  void storeGridBuffer(int page);
  void freeGridBuffer();

  // Grid geometry lives in components/CoverGridLayout.h — the single source of truth shared
  // with the themes' drawCoverGrid, so thumbs are generated at exactly the display size.
  static constexpr int GRID_PAGE_ITEMS = covergrid::GRID_PAGE_ITEMS;
  static constexpr unsigned long GO_HOME_MS = 1000;
  static constexpr unsigned long TOGGLE_DISPLAY_MS = 1000;

  bool showTitles = true;  // Toggle between metadata titles and filenames

  int thumbHeight = 0;  // Computed from actual grid layout for 1:1 cover rendering

  // Data loading
  void loadFiles();
  int getPageOffset() const;
  std::string getDisplayTitle(int index) const;
  size_t findEntry(const std::string& name) const;

 public:
  explicit BookShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("BookShelf", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
