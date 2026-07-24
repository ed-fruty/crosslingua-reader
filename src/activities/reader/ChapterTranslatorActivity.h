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
    // Post-success chooser: when at least one paragraph was translated, this replaces the
    // DONE screen so the user can enable a bilingual display mode without diving back into
    // the Bilingua submenu (a no-translation fallback leaves the mode at PT_NORMAL). Confirm
    // persists the highlighted mode; Back skips. Both exit via the normal returnToCaller().
    CHOOSE_DISPLAY_MODE,
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

  // Cursor into the CHOOSE_DISPLAY_MODE list (0..PT_MODE_COUNT-1). Seeded to the current
  // SETTINGS.translationDisplayMode when the chooser opens so it starts pre-highlighted.
  int displayModeSelection = 0;

  // ─── Translation task ──────────────────────────────────────────────────────
  TaskHandle_t taskHandle = nullptr;
  volatile bool cancelFlag = false;
  TranslatingHtmlRewriter::Result lastResult = {};
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};
  // Set by the worker when the run aborted specifically on low memory (heap
  // backpressure exhausted). Read on the main task in render() to show the
  // specific STR_TRANSLATION_LOW_MEMORY message instead of statusMsg (whose
  // 64-byte buffer cannot hold the translated text). Written before taskFailed
  // (volatile) is set, so the main task observes it once taskFailed is seen.
  bool lowMemoryAbort = false;

  // Progress tracking (mutex-free: writers update volatile ints, reader copies into locals).
  volatile int progressCurrent = 0;
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  // ─── Batch-boundary framebuffer handshake (periodic progress repaint) ──────
  // The framebuffer is freed for the whole chapter's network run, so progress would
  // otherwise be frozen the entire time. To repaint periodically we borrow
  // BookTranslator's worker/UI handshake: the rewriter invokes onBatchBoundary() on the
  // worker (chTranslate) task at each batch boundary (transients freed, progress
  // updated); when the repaint cadence is due the worker raises boundaryPending and
  // spins until the main task's loop() has restored the buffer, drawn progress, freed it
  // again, and set boundaryAck. All framebuffer/render work stays on the main task, so it
  // never races the render task or onExit(). The spin breaks on cancelFlag so onExit()
  // never blocks on it.
  volatile bool boundaryPending = false;
  volatile bool boundaryAck = false;
  // Worker-only cadence bookkeeping (touched solely in serviceBatchBoundary after the
  // main task seeds them in startTranslation; no cross-task sharing beyond that seed).
  int lastRepaintProgress = 0;
  unsigned long lastRepaintMillis = 0;

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

  // Batch-boundary repaint hook passed to the rewriter. The trampoline forwards the
  // rewriter's void* context to the instance method. serviceBatchBoundary() runs on the
  // worker task: it evaluates the repaint cadence and, when due, performs the
  // boundaryPending/boundaryAck handshake with the main task's loop().
  static void batchBoundaryTrampoline(void* ctx);
  void serviceBatchBoundary();

  // ─── Framebuffer lifecycle around the network run ──────────────────────────
  // The TLS handshake needs a >=10.5 KB contiguous block plus ~24 KB of churn, and
  // the translator context only has ~37 KB free otherwise, so the 48 KB heap
  // framebuffer is freed for the duration of the network run. Both helpers run ONLY
  // on the main task; while released the panel keeps showing the last flushed image
  // and render() is a no-op. See the shared design notes for the on-device numbers.
  void releaseFramebuffer();
  // Reallocates the framebuffer with retry, restarting as a last resort if it can
  // never be reclaimed. Idempotent. alreadyLocked=true when called from onExit(),
  // which already holds the RenderLock (the mutex is non-recursive). Used only on
  // paths that MUST draw (result screens, onExit) — never for a cosmetic repaint.
  void restoreFramebuffer(bool alreadyLocked = false);
  // Non-restarting restore for the periodic batch-boundary repaint. Returns true if
  // the 48 KB framebuffer is present after the call. A mid-chapter batch boundary can
  // momentarily lack a contiguous 48 KB hole (the keep-alive TLS session is still
  // open), so a failed restore just skips that one cosmetic repaint (the next boundary
  // retries) rather than rebooting mid-chapter. Mirrors BookTranslatorActivity.
  bool tryRestoreFramebuffer();

  // Display name for the current translation engine (e.g. "Google (Free) - New").
  const char* getEngineName() const;

  // Draws the CHOOSE_DISPLAY_MODE screen (header + the 8-mode list + button hints) via the
  // UITheme components. Self-contained: clears and flushes its own frame.
  void renderDisplayModeChooser();
};
