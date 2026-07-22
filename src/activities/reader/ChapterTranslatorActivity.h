#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "translator/TranslatingHtmlRewriter.h"

/**
 * Translates a single EPUB chapter in-place using the configured translation engine.
 *
 * Flow:
 *  1. If already translated -> CONFIRM_RETRANSLATE screen (Confirm = re-translate, Back = cancel).
 *  2. Source language picker (LanguagePickerActivity in include-auto-detect mode).
 *  3. Target language picker (LanguagePickerActivity). Persists choice to SETTINGS.translationLanguage.
 *  4. WiFi pre-flight: if not connected, push WifiSelectionActivity.
 *  5. Pre-scan file for block count (progress total) and run file-to-file translation
 *     in a background FreeRTOS task; UI polls progress every ~3 s.
 *  6. Output is written to Section::getTranslatedHtmlPath() (the persistent bilingual HTML).
 *  7. Result screen (DONE/FAILED/CANCELLED) waits for any button -> finish().
 *
 * Returns no payload via ActivityResult; the launching activity refreshes its state
 * from disk (Section::hasTranslatedHtml()) after the activity finishes.
 */
class ChapterTranslatorActivity final : public Activity {
 public:
  enum State {
    CONFIRM_RETRANSLATE,
    SOURCE_LANG_SELECTION,
    LANG_SELECTION,
    WIFI_SELECTION,
    TRANSLATING,
    DONE,
    FAILED,
    CANCELLED,
  };

  explicit ChapterTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                                     int spineIndex, std::string translatedHtmlPath, bool alreadyTranslated)
      : Activity("ChapterTranslator", renderer, mappedInput),
        epub(std::move(epub)),
        spineIndex(spineIndex),
        translatedHtmlPath(std::move(translatedHtmlPath)),
        alreadyTranslated(alreadyTranslated) {}

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

  // Language selection state. sourceLangCode "auto" => engine-side auto-detect.
  std::string sourceLangCode = "auto";
  std::string sourceLangName = "Auto Detect";
  std::string targetLangCode;
  std::string targetLangName;

  State state = SOURCE_LANG_SELECTION;

  // ─── Translation task ──────────────────────────────────────────────────────
  TaskHandle_t taskHandle = nullptr;
  volatile bool cancelFlag = false;
  TranslatingHtmlRewriter::Result lastResult = {};
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};

  // Progress tracking (mutex-free: writers update volatile ints, reader copies into locals).
  volatile int progressCurrent = 0;
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  void launchSourcePicker();
  void launchTargetPicker();
  void onSourceLangSelected(uint8_t resultIndex);
  void onTargetLangSelected(uint8_t resultIndex);
  void launchWifiOrStart();
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();

  // Display name for the current translation engine (e.g. "Google (Free) - New").
  const char* getEngineName() const;
};
