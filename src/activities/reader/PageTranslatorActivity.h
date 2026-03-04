#pragma once
#include <functional>
#include <string>

#include "../ActivityWithSubactivity.h"

/**
 * Translates the text of a single page on-the-fly and displays the result.
 *
 * Flow:
 *  1. If saved target language + WiFi connected -> translate immediately
 *  2. Otherwise show language picker / WiFi selection as needed
 *  3. Translate via ParagraphTranslator (may split into chunks if large)
 *  4. Display word-wrapped translated text with scroll support
 *  5. Back button dismisses
 */
class PageTranslatorActivity final : public ActivityWithSubactivity {
 public:
  enum State {
    LANG_SELECTION,
    WIFI_SELECTION,
    TRANSLATING,
    DISPLAYING,
    FAILED,
  };

  explicit PageTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string pageText,
                                  const std::function<void()>& onDismiss)
      : ActivityWithSubactivity("PageTranslator", renderer, mappedInput),
        pageText(std::move(pageText)),
        onDismiss(onDismiss) {}

  // Constructor for pre-translated text: skips API call, goes straight to DISPLAYING
  PageTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string preTranslatedText,
                         const std::function<void()>& onDismiss, bool /*preTranslated*/)
      : ActivityWithSubactivity("PageTranslator", renderer, mappedInput),
        translatedText(std::move(preTranslatedText)),
        preTranslated(true),
        state(DISPLAYING),
        onDismiss(onDismiss) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::string pageText;
  std::string translatedText;
  std::string targetLangCode;
  std::string targetLangName;
  State state = LANG_SELECTION;

  // Translation task
  TaskHandle_t taskHandle = nullptr;
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};

  bool preTranslated = false;  // True when constructed with pre-translated text (no WiFi used)
  // Scroll state for DISPLAYING
  int scrollOffset = 0;

  const std::function<void()> onDismiss;

  void onLangSelected(const char* code, const char* name);
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();
};
