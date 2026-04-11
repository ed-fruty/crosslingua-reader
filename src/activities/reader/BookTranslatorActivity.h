#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <string>

#include "../ActivityWithSubactivity.h"
#include "translator/TranslatingHtmlRewriter.h"

/**
 * Translates ALL chapters of an EPUB sequentially using the streaming translation engine.
 * Same blocking UI pattern as ChapterTranslatorActivity but loops through all spine items.
 * WiFi connects once and stays on for the entire book.
 *
 * Flow:
 *  1. Source language picker
 *  2. Target language picker
 *  3. Scan for already-translated chapters -> confirm re-translate or skip
 *  4. Connect to WiFi
 *  5. Loop: extract chapter -> count blocks -> translate file-to-file -> next chapter
 *  6. Show result -> call onComplete/onCancel
 */
class BookTranslatorActivity final : public ActivityWithSubactivity {
 public:
  enum State {
    SOURCE_LANG_SELECTION,
    LANG_SELECTION,
    CONFIRM_RETRANSLATE,
    WIFI_SELECTION,
    TRANSLATING,
    DONE,
    FAILED,
    CANCELLED,
  };

  explicit BookTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::shared_ptr<Epub>& epub, const std::function<void()>& onCancel,
                                  const std::function<void()>& onComplete)
      : ActivityWithSubactivity("BookTranslator", renderer, mappedInput),
        epub(epub),
        onCancel(onCancel),
        onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::shared_ptr<Epub> epub;
  State state = SOURCE_LANG_SELECTION;

  // Language selection
  std::string sourceLangCode = "auto";
  std::string sourceLangName = "Auto Detect";
  std::string targetLangCode;
  std::string targetLangName;

  // Re-translate confirmation
  bool skipTranslated = false;
  int alreadyTranslatedCount = 0;
  int totalChapters = 0;
  int confirmSelection = 0;  // 0=skip translated, 1=re-translate all

  // Translation task
  TaskHandle_t taskHandle = nullptr;
  volatile bool cancelFlag = false;
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};

  // Progress tracking (volatile for IPC between FreeRTOS task and main loop)
  volatile int currentChapter = 0;   // 0-based chapter being translated
  volatile int chaptersCompleted = 0;
  volatile int progressCurrent = 0;  // paragraph progress within current chapter
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  const std::function<void()> onCancel;
  const std::function<void()> onComplete;

  void launchSourcePicker();
  void launchTargetPicker();
  void onSourceLangSelected(const char* code, const char* name);
  void onLangSelected(const char* code, const char* name);
  void scanAlreadyTranslated();
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();
  const char* getEngineName() const;
};
