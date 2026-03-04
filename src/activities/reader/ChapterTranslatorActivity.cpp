#include "ChapterTranslatorActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/translator/LanguagePickerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ─── lifecycle ────────────────────────────────────────────────────────────────

void ChapterTranslatorActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // If the chapter is already translated, show confirmation before proceeding
  if (alreadyTranslated) {
    state = CONFIRM_RETRANSLATE;
    requestUpdate();
    return;
  }

  // Always start with source language picker
  launchSourcePicker();
}

void ChapterTranslatorActivity::launchSourcePicker() {
  state = SOURCE_LANG_SELECTION;
  enterNewActivity(new LanguagePickerActivity(
      renderer, mappedInput,
      [this](const char* code) {
        if (strcmp(code, "auto") == 0) {
          onSourceLangSelected("auto", "Auto Detect");
        } else {
          for (int i = 0; i < LanguagePickerActivity::NUM_LANGUAGES; i++) {
            if (strcmp(LanguagePickerActivity::LANGUAGES[i].code, code) == 0) {
              onSourceLangSelected(code, LanguagePickerActivity::LANGUAGES[i].name);
              return;
            }
          }
          onSourceLangSelected(code, code);
        }
      },
      [this] {
        auto cb = onCancel;
        cb();
      },
      "Source Language", true));
}

void ChapterTranslatorActivity::launchTargetPicker() {
  state = LANG_SELECTION;
  enterNewActivity(new LanguagePickerActivity(
      renderer, mappedInput,
      [this](const char* code) {
        for (int i = 0; i < LanguagePickerActivity::NUM_LANGUAGES; i++) {
          if (strcmp(LanguagePickerActivity::LANGUAGES[i].code, code) == 0) {
            SETTINGS.translationLanguage = static_cast<uint8_t>(i);
            SETTINGS.saveToFile();
            onLangSelected(code, LanguagePickerActivity::LANGUAGES[i].name);
            return;
          }
        }
        onLangSelected(code, code);
      },
      [this] {
        auto cb = onCancel;
        cb();
      },
      "Target Language"));
}

void ChapterTranslatorActivity::onSourceLangSelected(const char* code, const char* name) {
  exitActivity();
  sourceLangCode = code;
  sourceLangName = name;
  LOG_DBG("CHT", "Source language: %s (%s)", name, code);
  launchTargetPicker();
}

void ChapterTranslatorActivity::onExit() {
  ActivityWithSubactivity::onExit();
  cancelFlag = true;
  // Wait for translation task to finish if running
  if (taskHandle) {
    for (int i = 0; i < 50 && !taskDone && !taskFailed; i++) {
      delay(100);
    }
    taskHandle = nullptr;
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// ─── state transitions ────────────────────────────────────────────────────────

void ChapterTranslatorActivity::onLangSelected(const char* code, const char* name) {
  exitActivity();  // exit language picker if it was launched
  targetLangCode = code;
  targetLangName = name;
  state = WIFI_SELECTION;
  requestUpdate();

  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected(true);
    return;
  }
  enterNewActivity(
      new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) { onWifiConnected(connected); }));
}

void ChapterTranslatorActivity::onWifiConnected(bool success) {
  exitActivity();  // exit wifi picker if it was launched
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

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

  xTaskCreate(translationTask, "chTranslate", 10240, this, 1, &taskHandle);
}

void ChapterTranslatorActivity::translationTask(void* param) {
  auto* self = static_cast<ChapterTranslatorActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

void ChapterTranslatorActivity::runTranslation() {
  // Configure Google DNS — ESP32's DHCP-provided DNS may not resolve all Google subdomains
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
  delay(500);  // Let network stack stabilize after config change
  LOG_DBG("CHT", "DNS set to 8.8.8.8 / 8.8.4.4");

  // Step 1: Extract chapter HTML from EPUB to a temp file
  const auto& spineItem = epub->getSpineItem(spineIndex);
  const auto tmpPath = epub->getCachePath() + "/.tmp_translate_" + std::to_string(spineIndex) + ".html";

  FsFile tmpFile;
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
  tmpFile.close();

  // Step 2: Pre-scan for block count (for progress bar)
  progressTotal = TranslatingHtmlRewriter::countBlocksInFile(tmpPath);
  LOG_DBG("CHT", "Translation started, total=%d blocks", (int)progressTotal);

  // Step 3: Run file-to-file translation — write directly to final path
  if (Storage.exists(translatedHtmlPath.c_str())) {
    Storage.remove(translatedHtmlPath.c_str());
  }

  FsFile outFile;
  if (!Storage.openFileForWrite("CHT", translatedHtmlPath, outFile)) {
    Storage.remove(tmpPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to create output file");
    taskFailed = true;
    return;
  }

  // Use user-selected source language
  const char* srcLang = sourceLangCode.c_str();
  LOG_DBG("CHT", "Using source=%s, target=%s", srcLang, targetLangCode.c_str());

  TranslatingHtmlRewriter rewriter;
  lastResult = rewriter.rewriteFromFile(tmpPath, outFile, srcLang, targetLangCode.c_str(),
                                         SETTINGS.translationEngine, SETTINGS.translateApiKey, &cancelFlag,
                                         &progressCurrent);
  outFile.close();

  // Clean up temp input file
  Storage.remove(tmpPath.c_str());

  if (cancelFlag || lastResult.cancelled) {
    // Delete incomplete output
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

// ─── engine name helper ─────────────────────────────────────────────────────

const char* ChapterTranslatorActivity::getEngineName() const {
  switch (SETTINGS.translationEngine) {
    case CrossPointSettings::ENGINE_GOOGLE_FREE: return "Google (Free) - Old";
    case CrossPointSettings::ENGINE_DEEPL: return "DeepL";
    case CrossPointSettings::ENGINE_DEEPL_PRO: return "DeepL Pro";
    case CrossPointSettings::ENGINE_OPENAI: return "OpenAI";
    case CrossPointSettings::ENGINE_DEEPSEEK: return "DeepSeek";
    case CrossPointSettings::ENGINE_GEMINI: return "Gemini";
    case CrossPointSettings::ENGINE_GOOGLE_V2: return "Google (Free) - New";
    case CrossPointSettings::ENGINE_GOOGLE_HTML: return "Google (Free) - HTML";
    default: return "Unknown";
  }
}

// ─── loop / render ────────────────────────────────────────────────────────────

void ChapterTranslatorActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // CONFIRM_RETRANSLATE: confirm or back
  if (state == CONFIRM_RETRANSLATE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // User confirmed re-translate — show source + target language pickers
      launchSourcePicker();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      auto cb = onCancel;
      cb();
      return;
    }
    return;
  }

  if (state == TRANSLATING) {
    if (taskDone) {
      if (cancelFlag || lastResult.cancelled) {
        state = CANCELLED;
      } else {
        state = DONE;
      }
      requestUpdate();
    } else if (taskFailed) {
      state = FAILED;
      requestUpdate();
    } else {
      // Periodic progress update (every 3 seconds for e-ink)
      unsigned long now = millis();
      if (now - lastProgressUpdate >= 3000) {
        lastProgressUpdate = now;
        requestUpdate();
      }
    }
  }

  // Any button while showing result/error → leave
  if (state == DONE || state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (state == DONE) {
        auto cb = onComplete;
        cb();
      } else {
        auto cb = onCancel;
        cb();
      }
      return;
    }
  }

  // Back during translation → cancel
  if (state == TRANSLATING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelFlag = true;
  }
}

void ChapterTranslatorActivity::render(RenderLock&&) {
  if (subActivity) return;

  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_CHAPTER), true, EpdFontFamily::BOLD);

  if (state == CONFIRM_RETRANSLATE) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_CHAPTER_ALREADY_TRANSLATED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, tr(STR_RETRANSLATE_CONFIRM));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == TRANSLATING) {
    // Language line
    if (!targetLangName.empty()) {
      std::string langLine = sourceLangName + " -> " + targetLangName;
      renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
    }

    // Engine name
    char engineLine[48];
    snprintf(engineLine, sizeof(engineLine), "Engine: %s", getEngineName());
    renderer.drawCenteredText(UI_10_FONT_ID, 80, engineLine);

    renderer.drawCenteredText(UI_10_FONT_ID, 130, tr(STR_TRANSLATING_CHAPTER));

    // Progress text: "8 / 435"
    int total = progressTotal;
    int current = progressCurrent;
    if (total > 0) {
      char progressStr[32];
      snprintf(progressStr, sizeof(progressStr), "%d / %d", current, total);
      renderer.drawCenteredText(UI_12_FONT_ID, 180, progressStr, true, EpdFontFamily::BOLD);

      // Progress bar
      const int barX = 90;
      const int barY = 220;
      const int barW = pageWidth - 180;
      const int barH = 12;
      renderer.drawRect(barX, barY, barW, barH, true);
      if (current > 0 && total > 0) {
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
    // Language line
    if (!targetLangName.empty()) {
      std::string langLine = sourceLangName + " -> " + targetLangName;
      renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
    }

    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_DONE), true, EpdFontFamily::BOLD);

    char doneStr[64];
    int total = lastResult.paragraphsTranslated + lastResult.paragraphsSkipped;
    snprintf(doneStr, sizeof(doneStr), "%d / %d paragraphs", lastResult.paragraphsTranslated, total);
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
