#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Simple full-screen list that lets the user pick a translation target (or source) language.
 *
 * Returns the selected language as a `MenuResult{action = <index>}`:
 *   - When `includeAutoDetect == false`: `action` is an index into `LANGUAGES[]` (0..NUM_LANGUAGES-1).
 *   - When `includeAutoDetect == true`:
 *       * Selecting the "Auto-detect" entry returns `action = 0xFF` (sentinel matching
 *         `SETTINGS.sourceTranslationLanguage == 0xFF`).
 *       * Selecting any concrete language returns `action = LANGUAGES[]` index.
 *
 * Back button finishes with `result.isCancelled = true`.
 */
class LanguagePickerActivity final : public Activity {
 public:
  struct Language {
    const char* name;  // Display name (English; never translated, so users can find their language)
    const char* code;  // BCP-47 code passed to the translation engine
  };

  explicit LanguagePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  bool includeAutoDetect = false, uint8_t initialSelection = 0,
                                  const char* customTitle = nullptr)
      : Activity("LangPicker", renderer, mappedInput), customTitle(customTitle), includeAutoDetect(includeAutoDetect) {
    setInitialSelection(initialSelection);
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static const Language LANGUAGES[];
  static const int NUM_LANGUAGES;

 private:
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  const char* customTitle;
  bool includeAutoDetect;

  // Total visible item count (NUM_LANGUAGES, plus 1 if Auto-detect entry is shown).
  int itemCount() const { return includeAutoDetect ? NUM_LANGUAGES + 1 : NUM_LANGUAGES; }

  // UI label for the entry at `idx` in the visible list.
  const char* itemName(int idx) const;

  // Translate `initialSelection` (a LANGUAGES[] index, or 0xFF for auto) into the
  // visible-list index that should be highlighted on entry.
  void setInitialSelection(uint8_t initialSelection);

  // Translate a visible-list index into the result index (LANGUAGES[] index, or 0xFF for auto).
  int resultIndexFor(int visibleIdx) const;
};
