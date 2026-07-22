#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "translator/TranslatingHtmlRewriter.h"

/**
 * Translates ALL chapters of an EPUB sequentially using the streaming translation engine.
 * Same blocking UI pattern as ChapterTranslatorActivity but iterates every spine item.
 * WiFi connects once and remains on for the entire book.
 *
 * Flow:
 *  1. Source language picker (LanguagePickerActivity in include-auto-detect mode).
 *  2. Target language picker (LanguagePickerActivity).
 *  3. Pre-scan cache for already-translated chapters. If any exist, show the
 *     "Skip Translated" vs "Re-translate All" inline 2-option picker before WiFi.
 *  4. WiFi pre-flight: if not connected, push WifiSelectionActivity.
 *  5. Loop: extract chapter -> count blocks -> file-to-file translate -> next chapter.
 *     Two progress bars are rendered: per-chapter paragraphs and overall chapters.
 *  6. Cancel preserves finished chapters; only the in-flight chapter's partial output
 *     is removed.
 *  7. Result screen (DONE/FAILED/CANCELLED) waits for any button -> finish().
 *
 * Returns no payload via ActivityResult; the launching activity refreshes its state
 * from disk after the activity finishes.
 */
class BookTranslatorActivity final : public Activity {
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

  explicit BookTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub)
      : Activity("BookTranslator", renderer, mappedInput), epub(std::move(epub)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Never sleep mid-translation or while picking a WiFi network.
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::shared_ptr<Epub> epub;
  State state = SOURCE_LANG_SELECTION;

  // Language selection. sourceLangCode "auto" => engine-side auto-detect.
  std::string sourceLangCode = "auto";
  std::string sourceLangName = "Auto Detect";
  std::string targetLangCode;
  std::string targetLangName;

  // Re-translate confirmation
  bool skipTranslated = false;
  int alreadyTranslatedCount = 0;
  int totalChapters = 0;
  int confirmSelection = 0;  // 0=skip translated, 1=re-translate all

  // ─── Translation task ──────────────────────────────────────────────────────
  TaskHandle_t taskHandle = nullptr;
  volatile bool cancelFlag = false;
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};

  // Progress tracking (mutex-free: writers update volatile ints, reader copies into locals).
  volatile int currentChapter = 0;     // 0-based chapter being translated
  volatile int chaptersCompleted = 0;  // chapters fully done (counted toward overall progress)
  volatile int progressCurrent = 0;    // paragraph progress within current chapter
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  void launchSourcePicker();
  void launchTargetPicker();
  void onSourceLangSelected(uint8_t resultIndex);
  void onTargetLangSelected(uint8_t resultIndex);
  void scanAlreadyTranslated();
  void launchWifiOrStart();
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();

  // Display name for the current translation engine (e.g. "Google (Free) - New").
  const char* getEngineName() const;
};
