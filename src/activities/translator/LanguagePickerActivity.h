#pragma once
#include <functional>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Simple full-screen list that lets the user pick a translation target language.
 * Calls onSelect(langCode) when confirmed, or onCancel() on back press.
 */
class LanguagePickerActivity final : public Activity {
 public:
  struct Language {
    const char* name;  // Display name (English)
    const char* code;  // BCP-47 code passed to Google Translate
  };

  explicit LanguagePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void(const char*)>& onSelect,
                                  const std::function<void()>& onCancel,
                                  const char* customTitle = nullptr, bool includeAutoDetect = false)
      : Activity("LangPicker", renderer, mappedInput),
        onSelect(onSelect),
        onCancel(onCancel),
        customTitle(customTitle),
        includeAutoDetect(includeAutoDetect) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static const Language LANGUAGES[];
  static const int NUM_LANGUAGES;

 private:
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  const std::function<void(const char*)> onSelect;
  const std::function<void()> onCancel;
  const char* customTitle;
  bool includeAutoDetect;

  int itemCount() const { return includeAutoDetect ? NUM_LANGUAGES + 1 : NUM_LANGUAGES; }
  const char* itemName(int idx) const;
  const char* itemCode(int idx) const;
};
