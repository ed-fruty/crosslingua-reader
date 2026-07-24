#pragma once
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

  // Cover generation state
  bool coversLoaded = false;
  bool coversLoading = false;

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
  void generateCoversForPage();
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
