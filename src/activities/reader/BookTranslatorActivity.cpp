#include "BookTranslatorActivity.h"

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

// ─── epub (re)loading ───────────────────────────────────────────────────────

bool BookTranslatorActivity::ensureEpubLoaded() {
  if (epub) return true;
  LOG_DBG("BKT", "Loading lean epub (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS, and don't rebuild the cache if it is missing.
  if (!epub->load(false, true)) {
    LOG_ERR("BKT", "Failed to load epub: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  LOG_DBG("BKT", "Lean epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  return true;
}

void BookTranslatorActivity::returnToReader() { activityManager.goToReader(epubPath); }

// ─── lifecycle ────────────────────────────────────────────────────────────────

void BookTranslatorActivity::onEnter() {
  Activity::onEnter();

  if (!ensureEpubLoaded()) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    requestUpdate();
    return;
  }

  totalChapters = epub->getSpineItemsCount();
  launchSourcePicker();
}

void BookTranslatorActivity::onExit() {
  Activity::onExit();

  // Signal the worker to bail at the next batch boundary and wait briefly for it.
  // The 5-second cap is enough for a partially-translated chapter to abort cleanly;
  // longer waits would block UI navigation.
  cancelFlag = true;
  if (taskHandle) {
    for (int i = 0; i < 50 && !taskDone && !taskFailed; i++) {
      delay(100);
    }
    taskHandle = nullptr;
  }

  // Drop the WiFi link to free heap before the next activity. Mirrors ChapterTranslator.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// ─── language pickers ─────────────────────────────────────────────────────────

void BookTranslatorActivity::launchSourcePicker() {
  state = SOURCE_LANG_SELECTION;

  // Seed the picker with the user's last source-language choice (0xFF => "auto").
  const uint8_t initial = SETTINGS.sourceTranslationLanguage;
  startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                  /*includeAutoDetect=*/true,
                                                                  /*initialSelection=*/initial,
                                                                  /*customTitle=*/tr(STR_SOURCE_LANGUAGE)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToReader();
                             return;
                           }
                           const auto& menu = std::get<MenuResult>(result.data);
                           onSourceLangSelected(static_cast<uint8_t>(menu.action));
                         });
}

void BookTranslatorActivity::launchTargetPicker() {
  state = LANG_SELECTION;

  // Seed with the user's persisted target choice if any (0xFF => default to 0).
  const uint8_t initial = SETTINGS.translationLanguage == 0xFF ? 0 : SETTINGS.translationLanguage;
  startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                  /*includeAutoDetect=*/false,
                                                                  /*initialSelection=*/initial,
                                                                  /*customTitle=*/tr(STR_TARGET_LANGUAGE)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToReader();
                             return;
                           }
                           const auto& menu = std::get<MenuResult>(result.data);
                           onTargetLangSelected(static_cast<uint8_t>(menu.action));
                         });
}

void BookTranslatorActivity::onSourceLangSelected(uint8_t resultIndex) {
  if (resultIndex == AUTO_DETECT_SENTINEL) {
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
    SETTINGS.sourceTranslationLanguage = AUTO_DETECT_SENTINEL;
  } else if (resultIndex < LanguagePickerActivity::NUM_LANGUAGES) {
    sourceLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
    sourceLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
    SETTINGS.sourceTranslationLanguage = resultIndex;
  } else {
    // Defensive: out-of-range. Fall back to auto-detect.
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
  }
  SETTINGS.saveToFile();
  LOG_DBG("BKT", "Source language: %s (%s)", sourceLangName.c_str(), sourceLangCode.c_str());
  launchTargetPicker();
}

void BookTranslatorActivity::onTargetLangSelected(uint8_t resultIndex) {
  if (resultIndex >= LanguagePickerActivity::NUM_LANGUAGES) {
    // Defensive: out-of-range. Cancel rather than translate to an unknown code.
    LOG_ERR("BKT", "Target language out of range: %d", resultIndex);
    returnToReader();
    return;
  }
  targetLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
  targetLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
  SETTINGS.translationLanguage = resultIndex;
  SETTINGS.saveToFile();
  LOG_DBG("BKT", "Target language: %s (%s)", targetLangName.c_str(), targetLangCode.c_str());

  // Scan for already-translated chapters before connecting WiFi.
  scanAlreadyTranslated();
}

// ─── pre-translation scan ─────────────────────────────────────────────────────

void BookTranslatorActivity::scanAlreadyTranslated() {
  alreadyTranslatedCount = 0;
  const auto& cachePath = epub->getCachePath();
  for (int i = 0; i < totalChapters; i++) {
    const std::string path = cachePath + "/sections/" + std::to_string(i) + ".translated.html";
    if (Storage.exists(path.c_str())) {
      alreadyTranslatedCount++;
    }
  }

  LOG_DBG("BKT", "Pre-scan: %d / %d chapters already translated", alreadyTranslatedCount, totalChapters);

  if (alreadyTranslatedCount > 0) {
    state = CONFIRM_RETRANSLATE;
    confirmSelection = 0;
    requestUpdate();
  } else {
    // No existing translations -- go straight to WiFi.
    launchWifiOrStart();
  }
}

// ─── WiFi gate ────────────────────────────────────────────────────────────────

void BookTranslatorActivity::launchWifiOrStart() {
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

void BookTranslatorActivity::onWifiConnected(bool success) {
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

// ─── translation task ─────────────────────────────────────────────────────────

void BookTranslatorActivity::startTranslation() {
  state = TRANSLATING;
  cancelFlag = false;
  taskDone = false;
  taskFailed = false;
  currentChapter = 0;
  chaptersCompleted = 0;
  progressCurrent = 0;
  progressTotal = 0;
  lastProgressUpdate = 0;
  statusMsg[0] = '\0';
  requestUpdate();

  LOG_DBG("BKT", "State -> TRANSLATING: %d chapters, lang=%s, engine=%d", totalChapters, targetLangCode.c_str(),
          SETTINGS.translationEngine);

  // 10 KB stack matches ChapterTranslator: ParagraphTranslator can spike to ~6-8 KB
  // during HTTP + JSON parse on the larger engines. Priority 1 keeps it below the
  // render task.
  xTaskCreate(translationTask, "bookTranslate", 10240, this, 1, &taskHandle);
}

void BookTranslatorActivity::translationTask(void* param) {
  auto* self = static_cast<BookTranslatorActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

void BookTranslatorActivity::runTranslation() {
  // Force public DNS so engine-specific subdomains resolve consistently.
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
  delay(500);
  LOG_DBG("BKT", "DNS set to 8.8.8.8 / 8.8.4.4");

  // Defensive: the lean Epub is normally loaded in onEnter, but guard the task entry too.
  // This one Epub stays open for the whole run (re-extracted per chapter below).
  if (!ensureEpubLoaded()) {
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    taskFailed = true;
    return;
  }

  const auto& cachePath = epub->getCachePath();

  // The sections/ subdir is normally created by Section::createSectionFile when the
  // user opens a chapter; chapters they have never visited won't have it yet.
  const std::string sectionsDir = cachePath + "/sections";
  Storage.mkdir(sectionsDir.c_str());

  for (int si = 0; si < totalChapters; si++) {
    if (cancelFlag) break;

    currentChapter = si;
    progressCurrent = 0;
    progressTotal = 0;

    const std::string translatedPath = cachePath + "/sections/" + std::to_string(si) + ".translated.html";

    // Skip if already translated and user chose "Skip Translated".
    if (skipTranslated && Storage.exists(translatedPath.c_str())) {
      chaptersCompleted = chaptersCompleted + 1;  // ++ on volatile is deprecated in C++20
      LOG_DBG("BKT", "Chapter %d/%d: skipped (already translated)", si + 1, totalChapters);
      continue;
    }

    // Re-translate path: remove the stale output BEFORE we start so a mid-flight
    // cancel cannot leave behind a half-stale file mixed with the previous run.
    if (Storage.exists(translatedPath.c_str())) {
      Storage.remove(translatedPath.c_str());
    }

    // Extract chapter HTML from the EPUB to a temp scratch file.
    const auto& spineItem = epub->getSpineItem(si);
    const std::string tmpPath = cachePath + "/.tmp_book_" + std::to_string(si) + ".html";

    HalFile tmpFile;
    if (!Storage.openFileForWrite("BKT", tmpPath, tmpFile)) {
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to create temp file", si + 1);
      taskFailed = true;
      return;
    }
    if (!epub->readItemContentsToStream(spineItem.href, tmpFile, 1024)) {
      tmpFile.close();
      Storage.remove(tmpPath.c_str());
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to extract HTML", si + 1);
      taskFailed = true;
      return;
    }
    tmpFile.close();  // Must close before re-opening for read in the rewriter step.

    // Pre-scan for the progress-bar denominator. countBlocksInFile is a pure SAX pass
    // and is cheap relative to the network-bound translation step.
    progressTotal = TranslatingHtmlRewriter::countBlocksInFile(tmpPath);

    LOG_DBG("BKT", "Ch %d/%d: href=%s, blocks=%d", si + 1, totalChapters, spineItem.href.c_str(), (int)progressTotal);

    HalFile outFile;
    if (!Storage.openFileForWrite("BKT", translatedPath, outFile)) {
      Storage.remove(tmpPath.c_str());
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to create output", si + 1);
      taskFailed = true;
      return;
    }

    const char* srcLang = sourceLangCode.c_str();
    TranslatingHtmlRewriter rewriter;
    const auto result =
        rewriter.rewriteFromFile(tmpPath, outFile, srcLang, targetLangCode.c_str(), SETTINGS.translationEngine,
                                 SETTINGS.translateApiKey, &cancelFlag, &progressCurrent);
    outFile.close();
    Storage.remove(tmpPath.c_str());

    LOG_DBG("BKT", "Ch %d/%d: translated=%d, skipped=%d, errors=%d, cancelled=%d", si + 1, totalChapters,
            result.paragraphsTranslated, result.paragraphsSkipped, result.abortedOnErrors ? 1 : 0,
            result.cancelled ? 1 : 0);

    if (cancelFlag || result.cancelled) {
      // Drop the partial output of the in-flight chapter. Finished chapters retain
      // their .translated.html on disk.
      Storage.remove(translatedPath.c_str());
      break;
    }

    if (result.abortedOnErrors) {
      Storage.remove(translatedPath.c_str());
      if (result.errorDetail[0]) {
        snprintf(statusMsg, sizeof(statusMsg), "Ch %d: %s", si + 1, result.errorDetail);
      } else {
        snprintf(statusMsg, sizeof(statusMsg), "Ch %d: too many API errors", si + 1);
      }
      taskFailed = true;
      return;
    }

    chaptersCompleted = chaptersCompleted + 1;  // ++ on volatile is deprecated in C++20
    LOG_DBG("BKT", "Ch %d/%d: done (%d translated, %d skipped), free=%d", si + 1, totalChapters,
            result.paragraphsTranslated, result.paragraphsSkipped, (int)ESP.getFreeHeap());

    // Let the heap recover between chapters — prevents fragmentation from accumulating
    // across long runs.
    delay(500);
  }

  taskDone = true;
}

// ─── engine name helper ──────────────────────────────────────────────────────

const char* BookTranslatorActivity::getEngineName() const {
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
    default:
      return "Unknown";
  }
}

// ─── loop / render ────────────────────────────────────────────────────────────

void BookTranslatorActivity::loop() {
  // CONFIRM_RETRANSLATE: Skip Translated vs Re-translate All two-option menu.
  if (state == CONFIRM_RETRANSLATE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      confirmSelection = 1 - confirmSelection;  // toggle 0 / 1
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      skipTranslated = (confirmSelection == 0);
      LOG_DBG("BKT", "Re-translate choice: %s", skipTranslated ? "Skip Translated" : "Re-translate All");
      launchWifiOrStart();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
      return;
    }
    return;
  }

  // TRANSLATING: poll task status with throttled repaint.
  if (state == TRANSLATING) {
    if (taskDone) {
      state = cancelFlag ? CANCELLED : DONE;
      requestUpdate();
    } else if (taskFailed) {
      state = FAILED;
      requestUpdate();
    } else {
      const unsigned long now = millis();
      if (now - lastProgressUpdate >= 3000) {
        lastProgressUpdate = now;
        requestUpdate();
      }
    }
  }

  // Result screens: any of Confirm/Back leaves. The reader was torn down before this
  // activity launched, so we relaunch it from disk (it re-reads translation state).
  if (state == DONE || state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
      return;
    }
  }

  // Mid-translation cancel: worker checks cancelFlag between batches and between
  // chapters. Finished chapters are preserved on disk.
  if (state == TRANSLATING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelFlag = true;
  }
}

void BookTranslatorActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_BOOK), true, EpdFontFamily::BOLD);

  if (state == CONFIRM_RETRANSLATE) {
    // "X / Y chapters already translated"
    char countStr[80];
    snprintf(countStr, sizeof(countStr), "%d / %d %s", alreadyTranslatedCount, totalChapters,
             tr(STR_CHAPTERS_ALREADY_TRANSLATED));
    renderer.drawCenteredText(UI_10_FONT_ID, 100, countStr);

    // Two-option selection: 0 = Skip Translated, 1 = Re-translate All.
    const int optY = 200;
    const int optH = 40;
    const char* opt0 = tr(STR_SKIP_TRANSLATED);
    const char* opt1 = tr(STR_RETRANSLATE_ALL);

    if (confirmSelection == 0) {
      renderer.fillRect(20, optY - 5, pageWidth - 40, optH, true);
      renderer.drawCenteredText(UI_12_FONT_ID, optY + 5, opt0, false, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_12_FONT_ID, optY + optH + 15, opt1, true);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, optY + 5, opt0, true);
      renderer.fillRect(20, optY + optH + 5, pageWidth - 40, optH, true);
      renderer.drawCenteredText(UI_12_FONT_ID, optY + optH + 15, opt1, false, EpdFontFamily::BOLD);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == TRANSLATING) {
    // Language + engine summary header.
    if (!targetLangName.empty()) {
      std::string langLine = sourceLangName + " -> " + targetLangName;
      renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
    }
    char engineLine[80];
    snprintf(engineLine, sizeof(engineLine), "Engine: %s", getEngineName());
    renderer.drawCenteredText(UI_10_FONT_ID, 80, engineLine);

    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_TRANSLATING_BOOK));

    // Atomic-ish snapshot: copy volatiles into locals.
    const int total = progressTotal;
    const int current = progressCurrent;
    const int chDone = chaptersCompleted;
    const int chIdx = currentChapter + 1;  // 1-based for display

    // Per-chapter section: "Chapter X of Y" + paragraph bar.
    char chapterStr[48];
    snprintf(chapterStr, sizeof(chapterStr), "Chapter %d of %d", chIdx, totalChapters);
    renderer.drawCenteredText(UI_12_FONT_ID, 160, chapterStr, true, EpdFontFamily::BOLD);

    const int barX = 90;
    const int barW = pageWidth - 180;
    const int barH = 12;

    // Per-chapter progress bar (paragraphs within current chapter).
    {
      const int barY = 200;
      renderer.drawRect(barX, barY, barW, barH, true);
      if (total > 0 && current > 0) {
        int fillW = (barW - 2) * current / total;
        if (fillW > barW - 2) fillW = barW - 2;
        if (fillW > 0) {
          renderer.fillRect(barX + 1, barY + 1, fillW, barH - 2, true);
        }
      }
      // Percentage label above the bar.
      if (total > 0) {
        char pctStr[16];
        const int pct = (int)((long)current * 100 / (total > 0 ? total : 1));
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        renderer.drawCenteredText(UI_10_FONT_ID, barY - 22, pctStr);
      }
    }

    // Overall progress label.
    renderer.drawCenteredText(UI_12_FONT_ID, 260, "Overall:", true, EpdFontFamily::BOLD);

    // Overall progress bar (chapters completed).
    {
      const int barY = 300;
      renderer.drawRect(barX, barY, barW, barH, true);
      if (totalChapters > 0 && chDone > 0) {
        int fillW = (barW - 2) * chDone / totalChapters;
        if (fillW > barW - 2) fillW = barW - 2;
        if (fillW > 0) {
          renderer.fillRect(barX + 1, barY + 1, fillW, barH - 2, true);
        }
      }
      if (totalChapters > 0) {
        char pctStr[16];
        const int pct = (int)((long)chDone * 100 / totalChapters);
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        renderer.drawCenteredText(UI_10_FONT_ID, barY - 22, pctStr);
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
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_BOOK_TRANSLATION_DONE), true, EpdFontFamily::BOLD);

    char doneStr[64];
    snprintf(doneStr, sizeof(doneStr), "%d / %d chapters", (int)chaptersCompleted, totalChapters);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, doneStr);

    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_FAILED), true, EpdFontFamily::BOLD);
    if (statusMsg[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 200, statusMsg);
    }
    char doneStr[64];
    snprintf(doneStr, sizeof(doneStr), "%d / %d chapters completed", (int)chaptersCompleted, totalChapters);
    renderer.drawCenteredText(UI_10_FONT_ID, 240, doneStr);
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == CANCELLED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_BOOK_TRANSLATION_CANCELLED), true, EpdFontFamily::BOLD);
    char doneStr[64];
    snprintf(doneStr, sizeof(doneStr), "%d / %d chapters completed", (int)chaptersCompleted, totalChapters);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, doneStr);
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
