#pragma once
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <MappedInputManager.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Typed request the submenu hands back to the reader when the user picks a
// translate action. The submenu itself no longer launches the translator (that
// would keep the live reader + Section + Epub resident during the TLS handshake
// and starve wolfSSL of heap); instead it finishes with one of these and the
// reader runs launchTranslation() after releasing the Epub. Encoded in the
// returned ActivityResult's MenuResult::action field.
enum class PreTranslationResult : uint8_t {
  NONE = 0,
  TRANSLATE_CHAPTER = 1,
  TRANSLATE_BOOK = 2,
};

/**
 * Consolidated submenu that aggregates every Pre-Translation action and setting
 * into a single screen launched from the EPUB reader menu.
 *
 * Mirrors EpubReaderMenuActivity's vertical list pattern. Selecting most items
 * spawns a child activity (translate / language picker / API-key keyboard);
 * cyclical items mutate SETTINGS and re-render in place.
 *
 * After any child activity returns, the menu re-scans the on-disk translation
 * state so labels like "Translate Chapter" flip to "Re-translate Chapter" and
 * the "Delete Translations" entry appears once any chapter has been translated.
 */
class PreTranslationSubmenuActivity final : public Activity {
 public:
  enum class Action : uint8_t {
    CYCLE_DISPLAY_MODE,
    TRANSLATE_CHAPTER,
    TRANSLATE_BOOK,
    DELETE_TRANSLATIONS,
    PICK_TARGET_LANG,
    PICK_SOURCE_LANG,
    CYCLE_ENGINE,
    ENTER_API_KEY,
  };

  // Reader mode: launched from the EPUB reader menu. The submenu borrows the
  // reader's live Epub and current spine index and hands translate requests back
  // to the reader via a typed MenuResult (the reader tears itself down first).
  explicit PreTranslationSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::shared_ptr<Epub> epub, int currentSpineIndex);

  // Path mode: launched from the file browser with no reader open. The submenu
  // lazily loads its own lean, metadata-only Epub from epubPath and derives the
  // spine index from the book's saved progress. Translate requests are launched
  // directly (replaceActivity into a translator with FILE_BROWSER return target)
  // because there is no reader to tear anything down.
  explicit PreTranslationSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    Action action;
    StrId labelId;
  };

  std::shared_ptr<Epub> epub;
  int currentSpineIndex;
  // Path mode only: the book path the lean Epub is (re)loaded from. Empty in reader mode.
  std::string epubPath;
  bool pathMode = false;
  // Set true when the path-mode lean Epub fails to load; renders an error screen.
  bool epubLoadFailed = false;
  std::vector<MenuItem> menuItems;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  bool chapterIsTranslated = false;
  bool bookHasAnyTranslation = false;

  // Toast overlay (shown when user tries to switch display mode without translation).
  bool showingToast = false;
  unsigned long toastShownAtMs = 0UL;
  unsigned long toastDurationMs = 0UL;
  const char* toastMessage = nullptr;

  void buildMenuItems();
  // Re-scans on-disk translation state and rebuilds menu items. Called from
  // onEnter() and from each child-activity result handler.
  void rebuildAfterReturn();
  void onActionSelected(Action a);

  // Path mode only. Reopens a lean, metadata-only Epub from epubPath (same shape
  // as the translators' ensureEpubLoaded()). Idempotent; LOG_ERR + false on failure.
  bool ensureEpubLoaded();
  // Path mode only. Reads the book's saved progress.bin the same way the reader
  // restores it and returns the resume spine index, clamped to a valid chapter
  // (0 when there is no saved progress). Requires the lean Epub to be loaded.
  int loadSavedSpineIndex();
  // Launches the given translate action directly in path mode (replaceActivity into
  // the translator with FILE_BROWSER return target). Reader mode never calls this.
  void launchTranslatorFromPath(Action a);

  // Dynamic right-hand value labels for the list rows.
  const char* displayModeLabel() const;
  const char* engineLabel() const;
  const char* targetLangLabel() const;
  const char* sourceLangLabel() const;
  // Writes a masked representation of the API key into `out`.
  void maskedApiKey(char* out, size_t outSize) const;

  void showToast(const char* msg, unsigned long durationMs);
};
