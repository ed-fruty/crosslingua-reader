#include "ChapterTranslatorActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/translator/LanguagePickerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Sentinel value LanguagePickerActivity returns for the synthetic "Auto-detect" entry.
// Matches CrossPointSettings::sourceTranslationLanguage's 0xFF sentinel.
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

// ─── lifecycle ────────────────────────────────────────────────────────────────

void ChapterTranslatorActivity::onEnter() {
  Activity::onEnter();

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
  // The 5-second cap is enough for a partially-translated chapter to abort cleanly;
  // longer waits would block UI navigation.
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
                             finish();
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
                             finish();
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
    finish();
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
  lastResult = {};
  progressCurrent = 0;
  progressTotal = 0;
  lastProgressUpdate = 0;
  requestUpdate();

  LOG_DBG("CHT", "State -> TRANSLATING, lang=%s, engine=%d", targetLangCode.c_str(), SETTINGS.translationEngine);

  // 10 KB stack: ParagraphTranslator can spike to ~6-8 KB during HTTP + JSON parse on
  // the larger engines (Gemini, OpenAI). Priority 1 keeps it below the render task.
  xTaskCreate(translationTask, "chTranslate", 10240, this, 1, &taskHandle);
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

  if (Storage.exists(translatedHtmlPath.c_str())) {
    Storage.remove(translatedHtmlPath.c_str());
  }

  HalFile outFile;
  if (!Storage.openFileForWrite("CHT", translatedHtmlPath, outFile)) {
    Storage.remove(tmpPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to create output file");
    taskFailed = true;
    return;
  }

  const char* srcLang = sourceLangCode.c_str();
  LOG_DBG("CHT", "Using source=%s, target=%s", srcLang, targetLangCode.c_str());

  TranslatingHtmlRewriter rewriter;
  lastResult = rewriter.rewriteFromFile(tmpPath, outFile, srcLang, targetLangCode.c_str(), SETTINGS.translationEngine,
                                        SETTINGS.translateApiKey, &cancelFlag, &progressCurrent);
  outFile.close();  // Flush and release before the rename/delete dance below.

  Storage.remove(tmpPath.c_str());

  if (cancelFlag || lastResult.cancelled) {
    // Partial output is unusable — section loader would render half-translated
    // content as if it were complete. Delete and mark cancelled.
    Storage.remove(translatedHtmlPath.c_str());
    taskDone = true;
    return;
  }

  if (lastResult.abortedOnErrors) {
    Storage.remove(translatedHtmlPath.c_str());
    if (lastResult.errorDetail[0]) {
      snprintf(statusMsg, sizeof(statusMsg), "%s", lastResult.errorDetail);
    } else {
      snprintf(statusMsg, sizeof(statusMsg), "Translation failed: too many errors");
    }
    taskFailed = true;
    return;
  }

  if (lastResult.paragraphsTranslated == 0) {
    Storage.remove(translatedHtmlPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "No paragraphs translated");
    taskFailed = true;
    return;
  }

  LOG_DBG("CHT", "Translation done: %d translated, %d skipped", lastResult.paragraphsTranslated,
          lastResult.paragraphsSkipped);
  taskDone = true;
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
      finish();
      return;
    }
    return;
  }

  if (state == TRANSLATING) {
    if (taskDone) {
      state = (cancelFlag || lastResult.cancelled) ? CANCELLED : DONE;
      requestUpdate();
    } else if (taskFailed) {
      state = FAILED;
      requestUpdate();
    } else {
      // E-ink refresh is expensive (FAST_REFRESH ~300ms) — throttle progress repaints.
      const unsigned long now = millis();
      if (now - lastProgressUpdate >= 3000) {
        lastProgressUpdate = now;
        requestUpdate();
      }
    }
  }

  // Result screens: any of Confirm/Back leaves. We do not pass any payload
  // upstream; the caller refreshes its translation state from disk.
  if (state == DONE || state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = (state != DONE);
      setResult(std::move(result));
      finish();
      return;
    }
  }

  // Mid-translation cancel: the worker checks cancelFlag between batches.
  if (state == TRANSLATING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelFlag = true;
  }
}

void ChapterTranslatorActivity::render(RenderLock&&) {
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
    snprintf(engineLine, sizeof(engineLine), "Engine: %s", getEngineName());
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
    if (statusMsg[0]) {
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
