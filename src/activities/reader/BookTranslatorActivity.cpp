#include "BookTranslatorActivity.h"

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

// ── Lifecycle ────────────────────────────────────────────────────────────────

void BookTranslatorActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  totalChapters = epub->getSpineItemsCount();
  launchSourcePicker();
}

void BookTranslatorActivity::onExit() {
  ActivityWithSubactivity::onExit();
  cancelFlag = true;
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

// ── Language Selection ───────────────────────────────────────────────────────

void BookTranslatorActivity::launchSourcePicker() {
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

void BookTranslatorActivity::onSourceLangSelected(const char* code, const char* name) {
  exitActivity();
  sourceLangCode = code;
  sourceLangName = name;
  LOG_DBG("BKT", "Source language: %s (%s)", name, code);
  launchTargetPicker();
}

void BookTranslatorActivity::launchTargetPicker() {
  state = LANG_SELECTION;
  enterNewActivity(new LanguagePickerActivity(
      renderer, mappedInput,
      [this](const char* code) {
        for (int i = 0; i < LanguagePickerActivity::NUM_LANGUAGES; i++) {
          if (strcmp(LanguagePickerActivity::LANGUAGES[i].code, code) == 0) {
            SETTINGS.translationLanguage = static_cast<uint8_t>(i);
            onLangSelected(code, LanguagePickerActivity::LANGUAGES[i].name);
            return;
          }
        }
        onLangSelected(code, code);
      },
      [this] {
        auto cb = onCancel;
        cb();
      }));
}

void BookTranslatorActivity::onLangSelected(const char* code, const char* name) {
  exitActivity();
  targetLangCode = code;
  targetLangName = name;
  SETTINGS.saveToFile();
  LOG_DBG("BKT", "Target language: %s (%s)", name, code);

  // Scan for already-translated chapters before connecting WiFi
  scanAlreadyTranslated();
}

// ── Pre-translation scan ─────────────────────────────────────────────────────

void BookTranslatorActivity::scanAlreadyTranslated() {
  alreadyTranslatedCount = 0;
  const auto& cachePath = epub->getCachePath();
  for (int i = 0; i < totalChapters; i++) {
    std::string path = cachePath + "/sections/" + std::to_string(i) + ".translated.html";
    if (Storage.exists(path.c_str())) {
      alreadyTranslatedCount++;
    }
  }

  if (alreadyTranslatedCount > 0) {
    state = CONFIRM_RETRANSLATE;
    confirmSelection = 0;
    requestUpdate();
  } else {
    // No existing translations -- go straight to WiFi
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
}

// ── WiFi & Translation Start ─────────────────────────────────────────────────

void BookTranslatorActivity::onWifiConnected(bool success) {
  exitActivity();
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

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

  LOG_DBG("BKT", "Starting book translation: %d chapters, lang=%s, engine=%d", totalChapters,
          targetLangCode.c_str(), SETTINGS.translationEngine);

  xTaskCreate(translationTask, "bookTranslate", 10240, this, 1, &taskHandle);
}

void BookTranslatorActivity::translationTask(void* param) {
  auto* self = static_cast<BookTranslatorActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

// ── Core Translation Loop ────────────────────────────────────────────────────

void BookTranslatorActivity::runTranslation() {
  // Configure Google DNS
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
  delay(500);
  LOG_DBG("BKT", "DNS configured, starting chapter loop");

  const auto& cachePath = epub->getCachePath();

  for (int si = 0; si < totalChapters; si++) {
    if (cancelFlag) break;

    currentChapter = si;
    const std::string translatedPath = cachePath + "/sections/" + std::to_string(si) + ".translated.html";

    // Skip if already translated and user chose "Skip Translated"
    if (skipTranslated && Storage.exists(translatedPath.c_str())) {
      chaptersCompleted++;
      LOG_DBG("BKT", "Chapter %d/%d: skipped (already translated)", si + 1, totalChapters);
      continue;
    }

    // Extract chapter HTML from EPUB to temp file
    const auto& spineItem = epub->getSpineItem(si);
    const std::string tmpPath = cachePath + "/.tmp_book_" + std::to_string(si) + ".html";

    FsFile tmpFile;
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
    tmpFile.close();

    // Count blocks for progress bar
    progressTotal = TranslatingHtmlRewriter::countBlocksInFile(tmpPath);
    progressCurrent = 0;

    LOG_DBG("BKT", "Chapter %d/%d: %d blocks, translating...", si + 1, totalChapters, (int)progressTotal);

    // Delete old translation if exists
    if (Storage.exists(translatedPath.c_str())) {
      Storage.remove(translatedPath.c_str());
    }

    FsFile outFile;
    if (!Storage.openFileForWrite("BKT", translatedPath, outFile)) {
      Storage.remove(tmpPath.c_str());
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to create output", si + 1);
      taskFailed = true;
      return;
    }

    const char* srcLang = sourceLangCode.c_str();
    TranslatingHtmlRewriter rewriter;
    auto result = rewriter.rewriteFromFile(tmpPath, outFile, srcLang, targetLangCode.c_str(),
                                           SETTINGS.translationEngine, SETTINGS.translateApiKey, &cancelFlag,
                                           &progressCurrent);
    outFile.close();
    Storage.remove(tmpPath.c_str());

    if (cancelFlag || result.cancelled) {
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

    chaptersCompleted++;
    LOG_DBG("BKT", "Chapter %d/%d: done (%d translated, %d skipped)", si + 1, totalChapters,
            result.paragraphsTranslated, result.paragraphsSkipped);
  }

  taskDone = true;
}

// ── Main Loop (state polling) ────────────────────────────────────────────────

void BookTranslatorActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // CONFIRM_RETRANSLATE: two-option menu
  if (state == CONFIRM_RETRANSLATE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      confirmSelection = 1 - confirmSelection;  // toggle 0/1
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      skipTranslated = (confirmSelection == 0);
      state = WIFI_SELECTION;
      requestUpdate();
      WiFi.mode(WIFI_STA);
      if (WiFi.status() == WL_CONNECTED) {
        onWifiConnected(true);
        return;
      }
      enterNewActivity(
          new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) { onWifiConnected(connected); }));
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      auto cb = onCancel;
      cb();
      return;
    }
    return;
  }

  // TRANSLATING: poll task status
  if (state == TRANSLATING) {
    if (taskDone) {
      state = cancelFlag ? CANCELLED : DONE;
      requestUpdate();
    } else if (taskFailed) {
      state = FAILED;
      requestUpdate();
    } else {
      unsigned long now = millis();
      if (now - lastProgressUpdate >= 3000) {
        lastProgressUpdate = now;
        requestUpdate();
      }
    }
  }

  // Result screens: any button -> exit
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

  // Back during translation -> cancel
  if (state == TRANSLATING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelFlag = true;
  }
}

// ── Render ───────────────────────────────────────────────────────────────────

void BookTranslatorActivity::render(RenderLock&&) {
  if (subActivity) return;

  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_BOOK), true, EpdFontFamily::BOLD);

  if (state == CONFIRM_RETRANSLATE) {
    // "X / Y chapters already translated"
    char countStr[64];
    snprintf(countStr, sizeof(countStr), "%d / %d %s", alreadyTranslatedCount, totalChapters,
             tr(STR_CHAPTERS_ALREADY_TRANSLATED));
    renderer.drawCenteredText(UI_10_FONT_ID, 100, countStr);

    // Two options
    const int optY = 200;
    const int optH = 40;
    const char* opt0 = tr(STR_SKIP_TRANSLATED);
    const char* opt1 = tr(STR_RETRANSLATE_ALL);

    // Draw selection highlight
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
    // Language + engine
    if (!targetLangName.empty()) {
      std::string langLine = sourceLangName + " -> " + targetLangName;
      renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
    }
    char engineLine[48];
    snprintf(engineLine, sizeof(engineLine), "Engine: %s", getEngineName());
    renderer.drawCenteredText(UI_10_FONT_ID, 80, engineLine);

    // "Translating Book"
    renderer.drawCenteredText(UI_10_FONT_ID, 130, tr(STR_TRANSLATING_BOOK));

    // Chapter progress: "Chapter 3 / 47"
    int chapter = currentChapter + 1;
    char chapterStr[32];
    snprintf(chapterStr, sizeof(chapterStr), "Chapter %d / %d", chapter, totalChapters);
    renderer.drawCenteredText(UI_12_FONT_ID, 170, chapterStr, true, EpdFontFamily::BOLD);

    // Paragraph progress within chapter
    int total = progressTotal;
    int current = progressCurrent;
    if (total > 0) {
      char progressStr[32];
      snprintf(progressStr, sizeof(progressStr), "%d / %d", current, total);
      renderer.drawCenteredText(UI_10_FONT_ID, 210, progressStr);

      // Progress bar
      const int barX = 90;
      const int barY = 240;
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
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_BOOK_TRANSLATION_DONE), true, EpdFontFamily::BOLD);

    char doneStr[48];
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
    char doneStr[48];
    snprintf(doneStr, sizeof(doneStr), "%d / %d chapters completed", (int)chaptersCompleted, totalChapters);
    renderer.drawCenteredText(UI_10_FONT_ID, 240, doneStr);
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == CANCELLED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_BOOK_TRANSLATION_CANCELLED), true, EpdFontFamily::BOLD);
    char doneStr[48];
    snprintf(doneStr, sizeof(doneStr), "%d / %d chapters completed", (int)chaptersCompleted, totalChapters);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, doneStr);
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

// ── Helpers ──────────────────────────────────────────────────────────────────

const char* BookTranslatorActivity::getEngineName() const {
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
