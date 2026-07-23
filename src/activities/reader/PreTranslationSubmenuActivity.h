#pragma once
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <MappedInputManager.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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
    // Tooltip-mode (PT_TOOLTIP) controls. Persisted manually via CrossPointSettings toJson/fromJson
    // (NOT SettingsList.h) so they aren't double-written; edited only from this submenu.
    CYCLE_TOOLTIP_BUTTONS,
    CYCLE_TOOLTIP_BEHAVIOR,
  };

  explicit PreTranslationSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::shared_ptr<Epub> epub, int currentSpineIndex);

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

  // Dynamic right-hand value labels for the list rows.
  const char* displayModeLabel() const;
  const char* engineLabel() const;
  const char* targetLangLabel() const;
  const char* sourceLangLabel() const;
  const char* tooltipButtonsLabel() const;
  const char* tooltipBehaviorLabel() const;
  // Writes a masked representation of the API key into `out`.
  void maskedApiKey(char* out, size_t outSize) const;

  void showToast(const char* msg, unsigned long durationMs);
};
