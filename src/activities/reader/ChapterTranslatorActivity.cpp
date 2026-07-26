#include "ChapterTranslatorActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/translator/LanguagePickerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Sentinel value LanguagePickerActivity returns for the synthetic "Auto-detect" entry.
// Matches CrossPointSettings::sourceTranslationLanguage's 0xFF sentinel.
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

// Display-mode label mapping for the post-success chooser, indexed by
// CrossPointSettings::translationDisplayMode (PT_NORMAL..PT_TOOLTIP). Mirrors
// PreTranslationSubmenuActivity::displayModeLabel() exactly; kept as a small local copy
// rather than extracted to avoid churning the submenu (see task notes). A static_assert
// guards it against PT_MODE_COUNT drifting out of sync.
static constexpr StrId DISPLAY_MODE_LABELS[] = {
    StrId::STR_PT_NORMAL,           StrId::STR_PT_DARK,         StrId::STR_PT_LIGHT, StrId::STR_PT_ORIGINAL_ONLY,
    StrId::STR_PT_TRANSLATION_ONLY, StrId::STR_PT_SIDE_BY_SIDE, StrId::STR_PT_MODAL, StrId::STR_PT_TOOLTIP,
};
static_assert(sizeof(DISPLAY_MODE_LABELS) / sizeof(DISPLAY_MODE_LABELS[0]) == CrossPointSettings::PT_MODE_COUNT,
              "DISPLAY_MODE_LABELS must cover every translationDisplayMode value");

// ─── epub (re)loading ───────────────────────────────────────────────────────

bool ChapterTranslatorActivity::ensureEpubLoaded() {
  if (epub) return true;
  LOG_DBG("CHT", "Loading lean epub (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS, and don't rebuild the cache if it is missing.
  if (!epub->load(false, true)) {
    LOG_ERR("CHT", "Failed to load epub: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  LOG_DBG("CHT", "Lean epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  return true;
}

void ChapterTranslatorActivity::returnToCaller() {
  if (returnTarget == TranslatorReturnTarget::FILE_BROWSER) {
    // Started from the file browser (no reader was open) — return to the book's folder.
    activityManager.goToFileBrowser(FsHelpers::extractFolderPath(epubPath));
    return;
  }
  activityManager.goToReader(epubPath);
}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void ChapterTranslatorActivity::onEnter() {
  Activity::onEnter();

  if (!ensureEpubLoaded()) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    requestUpdate();
    return;
  }

  // If the chapter is already translated, show confirmation before proceeding.
  if (alreadyTranslated) {
    state = CONFIRM_RETRANSLATE;
    requestUpdate();
    return;
  }

  launchSourcePicker();
}

void ChapterTranslatorActivity::onExit() {
  Activity::onExit();

  // Signal the worker to bail at the next batch boundary and wait briefly for it.
  // Setting cancelFlag also breaks any in-progress repaint spin in serviceBatchBoundary,
  // so the worker never stays parked waiting for a boundaryAck that loop() will no longer
  // deliver (loop() does not run while we are here on the same main task). The 5-second
  // cap is enough for a partially-translated chapter to abort cleanly; longer waits would
  // block UI navigation.
  cancelFlag = true;
  if (taskHandle) {
    for (int i = 0; i < 50 && !taskDone && !taskFailed; i++) {
      delay(100);
    }
    taskHandle = nullptr;
  }

  // Drop the WiFi link to free heap before the next activity. Mirrors fork behavior.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Always hand the framebuffer back before leaving: returnToCaller() relaunches an
  // activity that renders immediately. onExit() runs while ActivityManager holds the
  // RenderLock, so restore without taking it again. Idempotent — a no-op if we never
  // released or already restored in the completion path. Done after the WiFi teardown
  // so the realloc has the most free heap available.
  restoreFramebuffer(/*alreadyLocked=*/true);
}

// ─── language pickers ─────────────────────────────────────────────────────────

void ChapterTranslatorActivity::launchSourcePicker() {
  state = SOURCE_LANG_SELECTION;

  // Seed the picker with the user's last source-language choice (0xFF => "auto").
  const uint8_t initial = SETTINGS.sourceTranslationLanguage;
  startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                  /*includeAutoDetect=*/true,
                                                                  /*initialSelection=*/initial,
                                                                  /*customTitle=*/tr(STR_SOURCE_LANGUAGE)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToCaller();
                             return;
                           }
                           const auto& menu = std::get<MenuResult>(result.data);
                           onSourceLangSelected(static_cast<uint8_t>(menu.action));
                         });
}

void ChapterTranslatorActivity::launchTargetPicker() {
  state = LANG_SELECTION;

  // Seed with the user's persisted target choice if any (0xFF => default to 0).
  const uint8_t initial = SETTINGS.translationLanguage == 0xFF ? 0 : SETTINGS.translationLanguage;
  startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                  /*includeAutoDetect=*/false,
                                                                  /*initialSelection=*/initial,
                                                                  /*customTitle=*/tr(STR_TARGET_LANGUAGE)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToCaller();
                             return;
                           }
                           const auto& menu = std::get<MenuResult>(result.data);
                           onTargetLangSelected(static_cast<uint8_t>(menu.action));
                         });
}

void ChapterTranslatorActivity::onSourceLangSelected(uint8_t resultIndex) {
  if (resultIndex == AUTO_DETECT_SENTINEL) {
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
    SETTINGS.sourceTranslationLanguage = AUTO_DETECT_SENTINEL;
  } else if (resultIndex < LanguagePickerActivity::NUM_LANGUAGES) {
    sourceLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
    sourceLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
    SETTINGS.sourceTranslationLanguage = resultIndex;
  } else {
    // Defensive: out-of-range index from picker. Fall back to auto-detect.
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
  }
  SETTINGS.saveToFile();
  LOG_DBG("CHT", "Source language: %s (%s)", sourceLangName.c_str(), sourceLangCode.c_str());
  launchTargetPicker();
}

void ChapterTranslatorActivity::onTargetLangSelected(uint8_t resultIndex) {
  if (resultIndex >= LanguagePickerActivity::NUM_LANGUAGES) {
    // Defensive: out-of-range. Cancel rather than translate to an unknown code.
    LOG_ERR("CHT", "Target language out of range: %d", resultIndex);
    returnToCaller();
    return;
  }
  targetLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
  targetLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
  SETTINGS.translationLanguage = resultIndex;
  SETTINGS.saveToFile();
  LOG_DBG("CHT", "Target language: %s (%s)", targetLangName.c_str(), targetLangCode.c_str());

  launchWifiOrStart();
}

// ─── WiFi gate ────────────────────────────────────────────────────────────────

void ChapterTranslatorActivity::launchWifiOrStart() {
  state = WIFI_SELECTION;
  requestUpdate();

  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiConnected(!result.isCancelled); });
}

void ChapterTranslatorActivity::onWifiConnected(bool success) {
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

// ─── translation task ─────────────────────────────────────────────────────────

void ChapterTranslatorActivity::startTranslation() {
  state = TRANSLATING;
  cancelFlag = false;
  taskDone = false;
  taskFailed = false;
  lowMemoryAbort = false;
  lastResult = {};
  progressCurrent = 0;
  progressTotal = 0;
  lastProgressUpdate = 0;
  boundaryPending = false;
  boundaryAck = false;
  lastRepaintProgress = 0;
  // Seed the 20 s repaint clock at run start so the first time-based repaint fires ~20 s
  // in, not immediately at the first boundary.
  lastRepaintMillis = millis();

  // Flush the "Translating..." status screen to the panel BEFORE freeing the
  // framebuffer. requestUpdateAndWait() blocks until the render task has drawn and
  // displayed it; E-ink then retains that image with no buffer for the whole run.
  // Safe here: startTranslation() runs on the main task from a result handler, which
  // does not hold the RenderLock.
  requestUpdateAndWait();

  LOG_DBG("CHT", "State -> TRANSLATING, lang=%s, engine=%d", targetLangCode.c_str(), SETTINGS.translationEngine);

  // Free the 48 KB heap framebuffer so the TLS handshake has the contiguous headroom
  // it needs. Released for the entire chapter run; restored in the loop() completion
  // path (and unconditionally in onExit()) before anything draws again.
  releaseFramebuffer();

  // 10 KB stack: ParagraphTranslator can spike to ~6-8 KB during HTTP + JSON parse on
  // the larger engines (Gemini, OpenAI). Priority 1 keeps it below the render task.
  xTaskCreate(translationTask, "chTranslate", 10240, this, 1, &taskHandle);
}

// ─── framebuffer lifecycle ─────────────────────────────────────────────────────

void ChapterTranslatorActivity::releaseFramebuffer() {
  // RenderLock serialises against the render task: freeing the buffer while render()
  // is mid-draw would be a use-after-free. Both this and render() run on separate
  // tasks, so the lock is the guard even though render() also null-checks up front.
  RenderLock lock;
  if (!renderer.hasFrameBuffer()) return;  // already released
  if (!renderer.releaseFrameBufferForNetwork()) {
    LOG_ERR("CHT", "Framebuffer release failed");
    return;
  }
  LOG_DBG("MEM", "CT post-release: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void ChapterTranslatorActivity::restoreFramebuffer(bool alreadyLocked) {
  if (renderer.hasFrameBuffer()) return;  // idempotent: nothing to restore

  for (int attempt = 0; attempt < 5; attempt++) {
    bool ok;
    if (alreadyLocked) {
      // onExit() already holds the RenderLock via exitActivity(); taking the
      // non-recursive mutex again on the same task would deadlock.
      ok = renderer.restoreFrameBufferAfterNetwork();
    } else {
      RenderLock lock;
      if (renderer.hasFrameBuffer()) return;  // re-check under the lock
      ok = renderer.restoreFrameBufferAfterNetwork();
    }
    if (ok) {
      LOG_DBG("MEM", "CT post-restore: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    LOG_ERR("CHT", "Framebuffer realloc failed (attempt %d/5)", attempt + 1);
    delay(100);
  }

  // Practically unreachable: restore runs only after the rewriter/HTTP/expat
  // transients (~35+ KB) have been freed, so a clean 48 KB hole is available. If it
  // still fails the device has no buffer to draw on; restart to recover. The
  // translated HTML is already committed to SD and the reader re-reads it on relaunch.
  LOG_ERR("CHT", "Framebuffer realloc permanently failed; restarting");
  ESP.restart();
}

bool ChapterTranslatorActivity::tryRestoreFramebuffer() {
  if (renderer.hasFrameBuffer()) return true;  // already present
  RenderLock lock;
  if (renderer.hasFrameBuffer()) return true;  // re-check under the lock
  if (renderer.restoreFrameBufferAfterNetwork()) {
    LOG_DBG("MEM", "CT boundary restore: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return true;
  }
  // No contiguous 48 KB hole right now (the keep-alive TLS session is still open mid
  // chapter). Skip this cosmetic repaint; the next boundary retries. Never restart here —
  // a mid-chapter reboot would drop the in-flight chapter's work.
  LOG_DBG("CHT", "Boundary framebuffer restore unavailable; skipping repaint");
  return false;
}

void ChapterTranslatorActivity::translationTask(void* param) {
  auto* self = static_cast<ChapterTranslatorActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

void ChapterTranslatorActivity::runTranslation() {
  // ESP32 DHCP can hand back a DNS that doesn't resolve every Google subdomain
  // we hit (translate.google.com vs translate.googleapis.com). Force public DNS.
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
  delay(500);  // Let the lwIP stack pick up the new resolver list.
  LOG_DBG("CHT", "DNS set to 8.8.8.8 / 8.8.4.4");

  // Defensive: the lean Epub is normally loaded in onEnter, but guard the task entry too.
  if (!ensureEpubLoaded()) {
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    taskFailed = true;
    return;
  }

  // Step 1: extract this chapter's HTML out of the EPUB zip into a scratch file.
  const auto& spineItem = epub->getSpineItem(spineIndex);
  const auto tmpPath = epub->getCachePath() + "/.tmp_translate_" + std::to_string(spineIndex) + ".html";

  HalFile tmpFile;
  if (!Storage.openFileForWrite("CHT", tmpPath, tmpFile)) {
    snprintf(statusMsg, sizeof(statusMsg), "Failed to create temp file");
    taskFailed = true;
    return;
  }
  if (!epub->readItemContentsToStream(spineItem.href, tmpFile, 1024)) {
    tmpFile.close();
    Storage.remove(tmpPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to extract chapter");
    taskFailed = true;
    return;
  }
  tmpFile.close();  // Must close before re-opening the same path for read in step 2.

  // Step 2: pre-scan for the progress-bar denominator. countBlocksInFile is a
  // pure SAX pass with no translation work and is cheap relative to step 3.
  progressTotal = TranslatingHtmlRewriter::countBlocksInFile(tmpPath);
  LOG_DBG("CHT", "Translation started, total=%d blocks", (int)progressTotal);

  // Step 3: open the destination path and run the rewriter.
  // Section's createSectionFile() expects the `sections/` subdir to exist; the
  // path comes from Section::getTranslatedHtmlPath() so the dir may not have
  // been created yet if this chapter has never been rendered.
  const auto sectionsDir = epub->getCachePath() + "/sections";
  Storage.mkdir(sectionsDir.c_str());

  // Write to a ".part" file, then atomically rename it into place only on success. This
  // guarantees a power loss mid-translation never leaves a truncated file at the final path:
  // the final file appears solely via the post-completion rename, so hasTranslatedHtml()
  // (which trusts the final file's existence) can never observe a partial.
  const std::string partPath = translatedHtmlPath + ".part";
  Storage.remove(partPath.c_str());  // clear any stale partial from an interrupted run

  HalFile outFile;
  if (!Storage.openFileForWrite("CHT", partPath, outFile)) {
    Storage.remove(tmpPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to create output file");
    taskFailed = true;
    return;
  }

  const char* srcLang = sourceLangCode.c_str();
  LOG_DBG("CHT", "Using source=%s, target=%s", srcLang, targetLangCode.c_str());

  TranslatingHtmlRewriter rewriter;
  lastResult = rewriter.rewriteFromFile(tmpPath, outFile, srcLang, targetLangCode.c_str(), SETTINGS.translationEngine,
                                        SETTINGS.translateApiKey, &cancelFlag, &progressCurrent,
                                        &ChapterTranslatorActivity::batchBoundaryTrampoline, this);
  outFile.close();  // Flush and release before the rename/delete dance below.

  Storage.remove(tmpPath.c_str());

  if (cancelFlag || lastResult.cancelled) {
    // Partial output is unusable — section loader would render half-translated
    // content as if it were complete. Discard the ".part" and mark cancelled; any
    // prior committed translation at the final path is left untouched.
    Storage.remove(partPath.c_str());
    taskDone = true;
    return;
  }

  if (lastResult.abortedOnErrors) {
    Storage.remove(partPath.c_str());
    if (lastResult.abortedLowMemory) {
      // Specific low-memory abort: render() draws tr(STR_TRANSLATION_LOW_MEMORY)
      // directly (the translated text does not fit statusMsg's 64-byte buffer).
      lowMemoryAbort = true;
    } else if (lastResult.errorDetail[0]) {
      snprintf(statusMsg, sizeof(statusMsg), "%s", lastResult.errorDetail);
    } else {
      snprintf(statusMsg, sizeof(statusMsg), "Translation failed: too many errors");
    }
    taskFailed = true;
    return;
  }

  // Chapter-success semantics: zero translated paragraphs is only a FAILURE when the
  // chapter actually had translatable content that failed. A cover / image-only /
  // fully-already-translated chapter legitimately translates nothing — that passthrough
  // output is a valid result and is committed like any other.
  if (lastResult.paragraphsTranslated == 0 && (lastResult.translateFailures > 0 || lastResult.abortedOnErrors)) {
    Storage.remove(partPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "No paragraphs translated");
    taskFailed = true;
    return;
  }

  // Commit atomically: remove any prior final output, then promote ".part" -> final. The
  // rename is the commit point — a completed translation is simply "the final file exists";
  // a crash before the rename leaves only the ".part", which is ignored on the next run.
  Storage.remove(translatedHtmlPath.c_str());
  if (!Storage.rename(partPath.c_str(), translatedHtmlPath.c_str())) {
    Storage.remove(partPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to finalize output");
    taskFailed = true;
    return;
  }

  LOG_DBG("CHT", "Translation done: %d translated, %d skipped, %d failed", lastResult.paragraphsTranslated,
          lastResult.paragraphsSkipped, lastResult.translateFailures);
  taskDone = true;
}

// ─── periodic progress repaint (worker-side cadence + UI handshake) ────────────

void ChapterTranslatorActivity::batchBoundaryTrampoline(void* ctx) {
  static_cast<ChapterTranslatorActivity*>(ctx)->serviceBatchBoundary();
}

void ChapterTranslatorActivity::serviceBatchBoundary() {
  // Runs on the worker (chTranslate) task between batches: transients are freed and
  // progressCurrent is up to date. Bail immediately if we're cancelling so onExit()
  // (which sets cancelFlag then waits for the task to finish) never blocks on a spin.
  if (cancelFlag) return;

  const int current = progressCurrent;
  const int total = progressTotal;
  // Repaint every max(5, total/10) blocks -> ~10-15 repaints across a chapter.
  const int blockThreshold = (total / 10) > 5 ? (total / 10) : 5;
  const unsigned long now = millis();
  const bool progressHit = (current - lastRepaintProgress) >= blockThreshold;
  const bool timeHit = (now - lastRepaintMillis) >= 20000UL;  // millis(); no Date APIs
  if (!progressHit && !timeHit) return;

  // Hand off to the main task: its loop() restores the framebuffer, draws the updated
  // progress synchronously, frees it again, then sets boundaryAck. We own no
  // framebuffer/render state here, so the restore/redraw/release can never race the
  // render task or onExit()'s teardown. The spin also exits on cancelFlag.
  boundaryAck = false;
  boundaryPending = true;
  while (!boundaryAck && !cancelFlag) {
    delay(5);
  }
  boundaryPending = false;

  // Advance the cadence baselines even if a cancel broke the spin — the next boundary
  // (if any) then re-evaluates from here rather than firing again instantly.
  lastRepaintProgress = current;
  lastRepaintMillis = now;
}

// ─── engine name helper ──────────────────────────────────────────────────────

const char* ChapterTranslatorActivity::getEngineName() const {
  switch (SETTINGS.translationEngine) {
    case CrossPointSettings::ENGINE_GOOGLE_FREE:
      return tr(STR_ENGINE_GOOGLE_FREE);
    case CrossPointSettings::ENGINE_DEEPL:
      return tr(STR_ENGINE_DEEPL);
    case CrossPointSettings::ENGINE_DEEPL_PRO:
      return tr(STR_ENGINE_DEEPL_PRO);
    case CrossPointSettings::ENGINE_OPENAI:
      return tr(STR_ENGINE_OPENAI);
    case CrossPointSettings::ENGINE_DEEPSEEK:
      return tr(STR_ENGINE_DEEPSEEK);
    case CrossPointSettings::ENGINE_GEMINI:
      return tr(STR_ENGINE_GEMINI);
    case CrossPointSettings::ENGINE_GOOGLE_V2:
      return tr(STR_ENGINE_GOOGLE_V2);
    case CrossPointSettings::ENGINE_GOOGLE_HTML:
      return tr(STR_ENGINE_GOOGLE_HTML);
    case CrossPointSettings::ENGINE_AZURE:
      return tr(STR_ENGINE_AZURE);
    default:
      return "Unknown";
  }
}

// ─── loop / render ────────────────────────────────────────────────────────────

void ChapterTranslatorActivity::loop() {
  // CONFIRM_RETRANSLATE: confirm = re-translate, back = cancel.
  if (state == CONFIRM_RETRANSLATE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchSourcePicker();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToCaller();
      return;
    }
    return;
  }

  if (state == TRANSLATING) {
    // The worker is paused at a batch boundary asking for a progress repaint: restore
    // the buffer, draw the updated progress synchronously, free it again, then release
    // the worker. All on the main task, so it never races the render task; and since
    // onExit() runs on this same task it can never overlap this sequence. Claim
    // boundaryPending up front so a second loop() pass cannot re-run the cycle.
    if (boundaryPending) {
      boundaryPending = false;
      // A mid-chapter batch boundary can momentarily lack a contiguous 48 KB hole (the
      // keep-alive TLS session is still open). Waiting here is pointless: the worker is
      // parked spinning on boundaryAck with the TLS buffers held, so nothing can free
      // memory while we stall — a wait only freezes the UI with cancel unresponsive. Use
      // the NON-restarting restore directly: if the buffer can't be reclaimed, skip this
      // cosmetic repaint (the next boundary retries) rather than rebooting mid-chapter.
      if (tryRestoreFramebuffer()) {
        requestUpdateAndWait();
        releaseFramebuffer();
      }
      boundaryAck = true;
      return;
    }

    if (taskDone) {
      restoreFramebuffer();  // bring the buffer back BEFORE the result screen draws
      if (cancelFlag || lastResult.cancelled) {
        state = CANCELLED;
      } else if (lastResult.paragraphsTranslated > 0) {
        // Success with real content: offer the display-mode chooser so the user can enable a
        // bilingual mode straight away (a passthrough chapter that translated nothing keeps
        // the plain DONE screen). Pre-highlight the current mode.
        displayModeSelection = SETTINGS.translationDisplayMode < CrossPointSettings::PT_MODE_COUNT
                                   ? SETTINGS.translationDisplayMode
                                   : CrossPointSettings::PT_NORMAL;
        state = CHOOSE_DISPLAY_MODE;
      } else {
        state = DONE;
      }
      requestUpdate();
    } else if (taskFailed) {
      restoreFramebuffer();
      state = FAILED;
      requestUpdate();
    } else {
      // The framebuffer is released for the whole run, so hasFrameBuffer() is false
      // and no progress repaints are issued — the network run stays silent by design
      // (a request here would be a no-op render anyway). Gate on the buffer so we
      // resume normal throttled repaints if release ever failed.
      const unsigned long now = millis();
      if (renderer.hasFrameBuffer() && now - lastProgressUpdate >= 3000) {
        lastProgressUpdate = now;
        requestUpdate();
      }
    }
  }

  // Display-mode chooser: Up/Down move the highlight, Confirm persists the choice (guarded
  // save, mirroring the Bilingua submenu) and exits, Back skips and exits. Both exits use the
  // normal return path so the relaunched reader picks up the mode from settings.
  if (state == CHOOSE_DISPLAY_MODE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      displayModeSelection =
          (displayModeSelection + CrossPointSettings::PT_MODE_COUNT - 1) % CrossPointSettings::PT_MODE_COUNT;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      displayModeSelection = (displayModeSelection + 1) % CrossPointSettings::PT_MODE_COUNT;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const uint8_t chosen = static_cast<uint8_t>(displayModeSelection);
      if (SETTINGS.translationDisplayMode != chosen) {  // guard SPIFFS write on no-op selections
        SETTINGS.translationDisplayMode = chosen;
        SETTINGS.saveToFile();
        LOG_DBG("CHT", "Display mode set to %d after translation", (int)chosen);
      }
      returnToCaller();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToCaller();
      return;
    }
    return;
  }

  // Result screens: any of Confirm/Back leaves. The reader was torn down before this
  // activity launched, so we relaunch it from disk (it re-reads translation state).
  if (state == DONE || state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToCaller();
      return;
    }
  }

  // Mid-translation cancel: the worker checks cancelFlag between batches.
  if (state == TRANSLATING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelFlag = true;
  }
}

void ChapterTranslatorActivity::render(RenderLock&&) {
  // Backstop for the loop()-level suppression: while the framebuffer is freed for the
  // network run there is nothing to draw on (drawing would be a use-after-free). The
  // panel retains the last flushed "Translating..." image until restore.
  if (!renderer.hasFrameBuffer()) return;

  // The chooser owns a different, list-based layout via the UITheme components, so it draws
  // and flushes itself rather than sharing the raw-coordinate result-screen chrome below.
  if (state == CHOOSE_DISPLAY_MODE) {
    renderDisplayModeChooser();
    return;
  }

  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_CHAPTER), true, EpdFontFamily::BOLD);

  if (state == CONFIRM_RETRANSLATE) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_CHAPTER_ALREADY_TRANSLATED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, tr(STR_RETRANSLATE_CONFIRM));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == TRANSLATING) {
    // "Source -> Target" arrow uses ASCII to avoid font-coverage gaps; the
    // language picker stores names in English (see LanguagePickerActivity.cpp).
    if (!targetLangName.empty()) {
      std::string langLine = sourceLangName + " -> " + targetLangName;
      renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
    }

    char engineLine[80];
    snprintf(engineLine, sizeof(engineLine), tr(STR_ENGINE_LABEL_FORMAT), getEngineName());
    renderer.drawCenteredText(UI_10_FONT_ID, 80, engineLine);

    renderer.drawCenteredText(UI_10_FONT_ID, 130, tr(STR_TRANSLATING_CHAPTER));

    // Atomic-ish snapshot: copy volatile counters into locals so a worker write
    // mid-render cannot reshape the progress bar within a single frame.
    const int total = progressTotal;
    const int current = progressCurrent;
    if (total > 0) {
      char progressStr[32];
      snprintf(progressStr, sizeof(progressStr), "%d / %d", current, total);
      renderer.drawCenteredText(UI_12_FONT_ID, 180, progressStr, true, EpdFontFamily::BOLD);

      const int barX = 90;
      const int barY = 220;
      const int barW = pageWidth - 180;
      const int barH = 12;
      renderer.drawRect(barX, barY, barW, barH, true);
      if (current > 0) {
        int fillW = (barW - 2) * current / total;
        if (fillW > barW - 2) fillW = barW - 2;
        if (fillW > 0) {
          renderer.fillRect(barX + 1, barY + 1, fillW, barH - 2, true);
        }
      }
    }

    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_BACK_TO_CANCEL));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == DONE) {
    if (!targetLangName.empty()) {
      std::string langLine = sourceLangName + " -> " + targetLangName;
      renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
    }

    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_DONE), true, EpdFontFamily::BOLD);

    char doneStr[64];
    const int translated = lastResult.paragraphsTranslated;
    const int total = translated + lastResult.paragraphsSkipped;
    snprintf(doneStr, sizeof(doneStr), "%d / %d paragraphs", translated, total);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, doneStr);

    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_FAILED), true, EpdFontFamily::BOLD);
    if (lowMemoryAbort) {
      // Long translated message -> wrap across the content width (statusMsg's
      // 64-byte buffer can't hold the Ukrainian text, so it is bypassed here).
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_TRANSLATION_LOW_MEMORY), pageWidth - 80, 4);
      const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
      int y = 200;
      for (const auto& line : lines) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
        y += lineH;
      }
    } else if (statusMsg[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 200, statusMsg);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == CANCELLED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_CANCELLED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void ChapterTranslatorActivity::renderDisplayModeChooser() {
  renderer.clearScreen();

  // Same header + list + hints layout the Bilingua submenu uses, so it stays orientation-aware
  // in all 4 modes via the UITheme safe area / metrics (no hardcoded pixel coordinates).
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                               /*hasSideButtonHints=*/false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_CHOOSE_DISPLAY_MODE));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  // Captureless lambda -> no std::function heap allocation; the row string is built from the
  // static StrId table each frame (transient, like the submenu's list).
  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight},
               static_cast<int>(CrossPointSettings::PT_MODE_COUNT), displayModeSelection,
               [](int index) -> std::string { return I18N.get(DISPLAY_MODE_LABELS[index]); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
