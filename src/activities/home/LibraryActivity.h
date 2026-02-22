#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct LibraryEntry {
  std::string filename;   // Original filename for sorting/display
  std::string fullPath;   // Full path to the file
  std::string title;      // From metadata (empty = use filename)
  std::string thumbPath;  // Path to cached thumb BMP (empty = no cover)
  bool isDirectory;
  bool coverProcessed;    // Whether metadata+thumb extraction was attempted
};

class LibraryActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  std::string basepath = "/";

  std::vector<LibraryEntry> entries;

  // Cover generation state
  bool coversLoaded = false;
  bool coversLoading = false;

  // Grid buffer cache for fast same-page navigation
  uint8_t* gridBuffer = nullptr;
  int gridBufferPage = -1;

  void storeGridBuffer(int page);
  void freeGridBuffer();

  // Grid layout constants
  static constexpr int GRID_COLS = 3;
  static constexpr int GRID_ROWS = 3;
  static constexpr int GRID_PAGE_ITEMS = GRID_COLS * GRID_ROWS;
  static constexpr int GRID_CELL_PADDING = 6;
  static constexpr int GRID_TITLE_AREA = 24;
  static constexpr unsigned long GO_HOME_MS = 1000;
  static constexpr unsigned long TOGGLE_DISPLAY_MS = 1000;

  bool showTitles = true;  // Toggle between metadata titles and filenames

  int thumbHeight = 0;  // Computed from actual grid layout for 1:1 cover rendering

  // Callbacks
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onGoHome;

  // Data loading
  void loadFiles();
  void generateCoversForPage();
  int getPageOffset() const;
  std::string getDisplayTitle(int index) const;
  size_t findEntry(const std::string& name) const;

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const std::function<void()>& onGoHome,
                           const std::function<void(const std::string& path)>& onSelectBook,
                           std::string initialPath = "/")
      : Activity("Library", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelectBook(onSelectBook),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
