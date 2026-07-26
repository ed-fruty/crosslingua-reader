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
 *  7. Result screen (DONE/FAILED/CANCELLED) waits for any button -> relaunches the reader.
 *
 * Takes the EPUB by PATH, not by shared_ptr: the reader tears down its own Epub/Section
 * before launching this activity (freeing ~65 KB for the TLS handshake). One lean,
 * metadata-only Epub is reopened lazily via ensureEpubLoaded() and stays open for the
 * whole book run. On exit the reader is relaunched from disk via
 * ActivityManager::goToReader() (no ActivityResult payload is passed back).
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
    // Post-success chooser: when at least one chapter was translated, this replaces the DONE
    // screen so the user can enable a bilingual display mode without diving back into the
    // Lingua submenu (a no-translation fallback leaves the mode at PT_NORMAL). Confirm
    // persists the highlighted mode; Back skips. Both exit via the normal returnToCaller().
    CHOOSE_DISPLAY_MODE,
    FAILED,
    CANCELLED,
  };

  explicit BookTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                  TranslatorReturnTarget returnTarget = TranslatorReturnTarget::READER)
      : Activity("BookTranslator", renderer, mappedInput), epubPath(std::move(epubPath)), returnTarget(returnTarget) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Never sleep mid-translation or while picking a WiFi network.
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::string epubPath;
  std::shared_ptr<Epub> epub;  // null until lazy-loaded (metadata-only) in ensureEpubLoaded()
  TranslatorReturnTarget returnTarget;
  State state = SOURCE_LANG_SELECTION;

  // Cursor into the CHOOSE_DISPLAY_MODE list (0..PT_MODE_COUNT-1). Seeded to the current
  // SETTINGS.translationDisplayMode when the chooser opens so it starts pre-highlighted.
  int displayModeSelection = 0;

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
  // Set by the worker when the run aborted specifically on low memory (heap
  // backpressure exhausted). Read on the main task in render() to show the specific
  // STR_TRANSLATION_LOW_MEMORY message instead of statusMsg (whose 64-byte buffer
  // cannot hold the translated text). Written before taskFailed (volatile) is set.
  bool lowMemoryAbort = false;

  // Progress tracking (mutex-free: writers update volatile ints, reader copies into locals).
  volatile int currentChapter = 0;     // 0-based chapter being translated
  volatile int chaptersCompleted = 0;  // chapters fully done (counted toward overall progress)
  volatile int progressCurrent = 0;    // paragraph progress within current chapter
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  // ─── Chapter- and batch-boundary framebuffer handshake ─────────────────────
  // The framebuffer must be freed during each chapter's TLS work but reallocated to
  // draw progress at boundaries. Two kinds of boundary drive the same handshake: the
  // per-chapter boundary in runTranslation() (updates the chapter counter + overall
  // bar) and the mid-chapter batch boundary in serviceBatchBoundary() (advances the
  // per-chapter paragraph bar during a single chapter's run). All framebuffer/render
  // work stays on the main task (never the worker), so it can never race the render
  // task or onExit()'s teardown. At a boundary the worker sets boundaryPending and
  // spins until the main task's loop() has restored the buffer, drawn progress, freed
  // it again, and set boundaryAck. The spin also breaks on cancelFlag so onExit() never
  // blocks on it.
  volatile bool boundaryPending = false;
  volatile bool boundaryAck = false;
  // Worker-only cadence bookkeeping for the periodic batch-boundary repaint (touched
  // solely in serviceBatchBoundary after the main task seeds them in startTranslation;
  // runTranslation resets lastRepaintProgress at each chapter because progressCurrent
  // restarts from 0 per chapter — no cross-task sharing beyond that seed/reset).
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
  void scanAlreadyTranslated();
  void launchWifiOrStart();
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();

  // Batch-boundary repaint hook passed to the rewriter (mirrors ChapterTranslatorActivity).
  // The trampoline forwards the rewriter's void* context to the instance method.
  // serviceBatchBoundary() runs on the worker task: it evaluates the repaint cadence and,
  // when due, performs the boundaryPending/boundaryAck handshake with the main task's loop()
  // so the per-chapter progress bar advances mid-chapter.
  static void batchBoundaryTrampoline(void* ctx);
  void serviceBatchBoundary();

  // ─── Framebuffer lifecycle (main task only) ────────────────────────────────
  // Frees / reallocates the 48 KB heap framebuffer around the network runs so the
  // TLS handshake has contiguous headroom (see the shared design notes for the
  // on-device numbers). While released the panel retains its last image and render()
  // is a no-op.
  void releaseFramebuffer();
  // Reallocates with retry, restarting as a last resort if it can never be reclaimed.
  // Idempotent. alreadyLocked=true when called from onExit(), which already holds the
  // RenderLock (the mutex is non-recursive).
  void restoreFramebuffer(bool alreadyLocked = false);
  // Non-restarting restore for the periodic batch-boundary repaint. Returns true if the
  // 48 KB framebuffer is present after the call. Unlike restoreFramebuffer() it NEVER
  // restarts on failure: a mid-chapter batch boundary can momentarily lack a contiguous
  // 48 KB hole (the keep-alive TLS session is still open), so a failed restore just skips
  // that one cosmetic repaint (the next boundary retries) rather than rebooting mid-book.
  bool tryRestoreFramebuffer();

  // Display name for the current translation engine (e.g. "Google (Free) - New").
  const char* getEngineName() const;

  // Draws the CHOOSE_DISPLAY_MODE screen (header + the 8-mode list + button hints) via the
  // UITheme components. Self-contained: clears and flushes its own frame.
  void renderDisplayModeChooser();
};
