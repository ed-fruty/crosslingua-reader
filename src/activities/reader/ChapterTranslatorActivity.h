#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "activities/reader/TranslatorReturnTarget.h"
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
 *  7. Result screen (DONE/FAILED/CANCELLED) waits for any button -> relaunches the reader.
 *
 * Takes the EPUB by PATH, not by shared_ptr: the reader tears down its own Epub/Section
 * before launching this activity (freeing ~65 KB for the TLS handshake). A lean,
 * metadata-only Epub is reopened lazily via ensureEpubLoaded(). On exit the reader is
 * relaunched from disk via ActivityManager::goToReader(), so translation state is picked
 * up fresh (no ActivityResult payload is passed back).
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

  explicit ChapterTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                     int spineIndex, std::string translatedHtmlPath, bool alreadyTranslated,
                                     TranslatorReturnTarget returnTarget = TranslatorReturnTarget::READER)
      : Activity("ChapterTranslator", renderer, mappedInput),
        epubPath(std::move(epubPath)),
        spineIndex(spineIndex),
        translatedHtmlPath(std::move(translatedHtmlPath)),
        alreadyTranslated(alreadyTranslated),
        returnTarget(returnTarget) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::string epubPath;
  std::shared_ptr<Epub> epub;  // null until lazy-loaded (metadata-only) in ensureEpubLoaded()
  int spineIndex;
  std::string translatedHtmlPath;
  bool alreadyTranslated;
  TranslatorReturnTarget returnTarget;

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

  // Reopens a lean, metadata-only Epub from epubPath (the reader released its own before
  // this activity launched). Idempotent; returns false + LOG_ERR if the load fails.
  bool ensureEpubLoaded();
  // Tears this activity down and hands control back to whoever launched translation
  // (the stack was cleared when the caller replaced itself with this activity, so
  // finish() cannot return there). returnTarget picks the destination: READER
  // relaunches the reader from disk via goToReader(epubPath); FILE_BROWSER returns to
  // the file browser at the book's parent directory via goToFileBrowser(dir).
  void returnToCaller();

  void launchSourcePicker();
  void launchTargetPicker();
  void onSourceLangSelected(uint8_t resultIndex);
  void onTargetLangSelected(uint8_t resultIndex);
  void launchWifiOrStart();
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();

  // ─── Framebuffer lifecycle around the network run ──────────────────────────
  // The TLS handshake needs a >=10.5 KB contiguous block plus ~24 KB of churn, and
  // the translator context only has ~37 KB free otherwise, so the 48 KB heap
  // framebuffer is freed for the duration of the network run. Both helpers run ONLY
  // on the main task; while released the panel keeps showing the last flushed image
  // and render() is a no-op. See the shared design notes for the on-device numbers.
  void releaseFramebuffer();
  // Reallocates the framebuffer with retry, restarting as a last resort if it can
  // never be reclaimed. Idempotent. alreadyLocked=true when called from onExit(),
  // which already holds the RenderLock (the mutex is non-recursive).
  void restoreFramebuffer(bool alreadyLocked = false);

  // Display name for the current translation engine (e.g. "Google (Free) - New").
  const char* getEngineName() const;
};
