#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <string>

#include "../ActivityWithSubactivity.h"
#include "translator/TranslatingHtmlRewriter.h"

/**
 * Translates a single EPUB chapter in-place using a configurable translation engine.
 *
 * Flow:
 *  1. If already translated → show CONFIRM_RETRANSLATE screen
 *  2. Show language picker (subactivity) — skipped if language was previously saved
 *  3. Connect to WiFi (subactivity)
 *  4. Pre-scan file for block count (progress total)
 *  5. Run file-to-file translation in background FreeRTOS task, show progress bar
 *  6. Output goes to Section's .translated.html path
 *  7. Show result and wait for button press → call onComplete/onCancel
 */
class ChapterTranslatorActivity final : public ActivityWithSubactivity {
 public:
  enum State {
    CONFIRM_RETRANSLATE,
    LANG_SELECTION,
    WIFI_SELECTION,
    TRANSLATING,
    DONE,
    FAILED,
    CANCELLED,
  };

  explicit ChapterTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::shared_ptr<Epub>& epub, int spineIndex,
                                     const std::string& translatedHtmlPath, bool alreadyTranslated,
                                     const std::function<void()>& onCancel,
                                     const std::function<void()>& onComplete)
      : ActivityWithSubactivity("ChapterTranslator", renderer, mappedInput),
        epub(epub),
        spineIndex(spineIndex),
        translatedHtmlPath(translatedHtmlPath),
        alreadyTranslated(alreadyTranslated),
        onCancel(onCancel),
        onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::shared_ptr<Epub> epub;
  int spineIndex;
  std::string translatedHtmlPath;
  bool alreadyTranslated;
  std::string targetLangCode;
  std::string targetLangName;
  State state = LANG_SELECTION;

  // Translation task
  TaskHandle_t taskHandle = nullptr;
  volatile bool cancelFlag = false;
  TranslatingHtmlRewriter::Result lastResult = {};
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};

  // Progress tracking
  volatile int progressCurrent = 0;
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  const std::function<void()> onCancel;
  const std::function<void()> onComplete;

  void onLangSelected(const char* code, const char* name);
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();

  // Get display name for current translation engine
  const char* getEngineName() const;
};
