#include "PageTranslatorActivity.h"

#include <GfxRenderer.h>
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
#include "translator/ParagraphTranslator.h"

// ─── lifecycle ────────────────────────────────────────────────────────────────

void PageTranslatorActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Pre-translated path: already have translated text, just display it
  if (state == DISPLAYING) {
    scrollOffset = 0;
    requestUpdate();
    return;
  }

  if (pageText.empty()) {
    snprintf(statusMsg, sizeof(statusMsg), "No text on this page");
    state = FAILED;
    requestUpdate();
    return;
  }

  // Fast path: reuse saved language if available
  if (SETTINGS.translationLanguage != 0xFF && SETTINGS.translationLanguage < LanguagePickerActivity::NUM_LANGUAGES) {
    targetLangCode = LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].code;
    targetLangName = LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].name;

    // Check WiFi
    WiFi.mode(WIFI_STA);
    if (WiFi.status() == WL_CONNECTED) {
      startTranslation();
      return;
    }
    // Need WiFi
    state = WIFI_SELECTION;
    requestUpdate();
    enterNewActivity(
        new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) { onWifiConnected(connected); }));
    return;
  }

  // No saved language — show target language picker
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
        auto cb = onDismiss;
        cb();
      },
      "Target Language"));
}

void PageTranslatorActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // Wait for translation task to finish if running
  if (taskHandle) {
    for (int i = 0; i < 50 && !taskDone && !taskFailed; i++) {
      delay(100);
    }
    taskHandle = nullptr;
  }
  // Only disconnect WiFi if we actually used it (not for pre-translated text display)
  if (!preTranslated) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }
}

// ─── state transitions ────────────────────────────────────────────────────────

void PageTranslatorActivity::onLangSelected(const char* code, const char* name) {
  exitActivity();  // exit language picker
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

void PageTranslatorActivity::onWifiConnected(bool success) {
  exitActivity();  // exit wifi picker
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

void PageTranslatorActivity::startTranslation() {
  state = TRANSLATING;
  taskDone = false;
  taskFailed = false;
  requestUpdate();

  LOG_DBG("PGT", "Starting page translation, text=%zu bytes, lang=%s", pageText.size(), targetLangCode.c_str());

  xTaskCreate(translationTask, "pgTranslate", 8192, this, 1, &taskHandle);
}

void PageTranslatorActivity::translationTask(void* param) {
  auto* self = static_cast<PageTranslatorActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

void PageTranslatorActivity::runTranslation() {
  // Configure Google DNS
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
  delay(500);  // Let network stack stabilize after config change

  const char* targetLang = targetLangCode.c_str();
  constexpr int maxRetries = 3;

  // Translate a piece of text with retries
  auto translateWithRetry = [&](const std::string& text, std::string& result) -> bool {
    std::string error;
    for (int attempt = 0; attempt < maxRetries; attempt++) {
      if (ParagraphTranslator::translate(text, targetLang, result, &error)) {
        return true;
      }
      LOG_DBG("PGT", "Attempt %d/%d failed: %s", attempt + 1, maxRetries, error.c_str());
      if (attempt + 1 < maxRetries) {
        delay(1000);
      }
    }
    snprintf(statusMsg, sizeof(statusMsg), "%.63s", error.empty() ? "Translation failed" : error.c_str());
    return false;
  };

  // If text fits in one request, do a single call
  if (pageText.size() <= ParagraphTranslator::MAX_TEXT_BYTES) {
    std::string result;
    if (translateWithRetry(pageText, result)) {
      translatedText = std::move(result);
      pageText.clear();
      pageText.shrink_to_fit();
      taskDone = true;
    } else {
      taskFailed = true;
    }
    return;
  }

  // Split at paragraph boundaries and translate chunks
  std::string combined;
  size_t pos = 0;
  while (pos < pageText.size()) {
    // Find next chunk boundary
    size_t chunkEnd = pos + ParagraphTranslator::MAX_TEXT_BYTES;
    if (chunkEnd >= pageText.size()) {
      chunkEnd = pageText.size();
    } else {
      // Try to break at paragraph boundary
      size_t breakAt = pageText.rfind("\n\n", chunkEnd);
      if (breakAt != std::string::npos && breakAt > pos) {
        chunkEnd = breakAt + 2;  // include the \n\n
      }
    }

    std::string chunk = pageText.substr(pos, chunkEnd - pos);
    std::string result;
    if (!translateWithRetry(chunk, result)) {
      taskFailed = true;
      return;
    }

    if (!combined.empty()) combined += "\n\n";
    combined += result;
    pos = chunkEnd;
  }

  translatedText = std::move(combined);
  pageText.clear();
  pageText.shrink_to_fit();
  taskDone = true;
}

// ─── loop / render ────────────────────────────────────────────────────────────

void PageTranslatorActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == TRANSLATING) {
    if (taskDone) {
      state = DISPLAYING;
      scrollOffset = 0;
      requestUpdate();
    } else if (taskFailed) {
      state = FAILED;
      requestUpdate();
    }
    return;
  }

  if (state == DISPLAYING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      auto cb = onDismiss;
      cb();
      return;
    }
    // Scroll down
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      scrollOffset++;
      requestUpdate();
      return;
    }
    // Scroll up
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (scrollOffset > 0) {
        scrollOffset--;
        requestUpdate();
      }
      return;
    }
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      auto cb = onDismiss;
      cb();
      return;
    }
    return;
  }
}

void PageTranslatorActivity::render(RenderLock&&) {
  if (subActivity) return;

  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (state == TRANSLATING) {
    renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_PAGE), true, EpdFontFamily::BOLD);
    if (!targetLangName.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, 60, targetLangName.c_str());
    }
    renderer.drawCenteredText(UI_10_FONT_ID, 150, tr(STR_TRANSLATING_PAGE));
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_PAGE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_FAILED), true, EpdFontFamily::BOLD);
    if (statusMsg[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 200, statusMsg);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == DISPLAYING) {
    // Title
    renderer.drawCenteredText(UI_12_FONT_ID, 5, tr(STR_TRANSLATE_PAGE), true, EpdFontFamily::BOLD);

    // Word-wrap and render translated text using reader font/spacing
    const int fontId = SETTINGS.getReaderFontId();
    const int margin = SETTINGS.screenMargin;
    const int textStartY = 30;
    const int textAreaHeight = pageHeight - textStartY - 45;  // leave room for 40px button hints
    const int maxWidth = pageWidth - 2 * margin;

    const int baseLineHeight = renderer.getLineHeight(fontId);
    const int lineHeight = static_cast<int>(baseLineHeight * SETTINGS.getReaderLineCompression());
    const int spaceWidth = renderer.getSpaceWidth(fontId);
    const int linesPerPage = textAreaHeight / lineHeight;

    // Word-wrap into lines
    const int paraGap = lineHeight / 3;  // small gap between paragraphs
    struct Line {
      int startIdx;
      int count;
      bool paragraphStart;  // add paraGap before this line
    };
    std::vector<std::string> words;
    std::vector<Line> lines;

    // Tokenize: split on whitespace, track paragraph breaks
    std::vector<bool> wordStartsParagraph;
    {
      size_t i = 0;
      bool sawParaBreak = false;
      while (i < translatedText.size()) {
        if (translatedText[i] == '\n') {
          sawParaBreak = true;
          i++;
        } else if (translatedText[i] == ' ') {
          i++;
        } else {
          size_t j = i;
          while (j < translatedText.size() && translatedText[j] != ' ' && translatedText[j] != '\n') j++;
          words.push_back(translatedText.substr(i, j - i));
          wordStartsParagraph.push_back(sawParaBreak && !words.empty());
          sawParaBreak = false;
          i = j;
        }
      }
    }

    // Build lines
    {
      int currentWidth = 0;
      int lineStart = 0;
      int wordCount = 0;
      bool lineIsParagraphStart = false;
      for (int i = 0; i < static_cast<int>(words.size()); i++) {
        // Paragraph break forces a new line
        if (wordStartsParagraph[i] && wordCount > 0) {
          lines.push_back({lineStart, wordCount, lineIsParagraphStart});
          lineStart = i;
          wordCount = 0;
          currentWidth = 0;
          lineIsParagraphStart = true;
        }
        if (wordCount == 0) lineIsParagraphStart = wordStartsParagraph[i];
        int wordWidth = renderer.getTextWidth(fontId, words[i].c_str());
        int needed = (wordCount > 0) ? spaceWidth + wordWidth : wordWidth;
        if (currentWidth + needed > maxWidth && wordCount > 0) {
          lines.push_back({lineStart, wordCount, lineIsParagraphStart});
          lineStart = i;
          wordCount = 1;
          currentWidth = wordWidth;
          lineIsParagraphStart = false;
        } else {
          wordCount++;
          currentWidth += needed;
        }
      }
      if (wordCount > 0) {
        lines.push_back({lineStart, wordCount, lineIsParagraphStart});
      }
    }

    // Apply scroll offset (in pages)
    int firstLine = scrollOffset * linesPerPage;
    if (firstLine >= static_cast<int>(lines.size())) {
      firstLine = std::max(0, static_cast<int>(lines.size()) - linesPerPage);
      scrollOffset = firstLine / std::max(1, linesPerPage);
    }

    // Draw visible lines
    int y = textStartY;
    for (int li = firstLine; li < static_cast<int>(lines.size()) && y + lineHeight <= textStartY + textAreaHeight;
         li++) {
      const auto& line = lines[li];
      if (line.paragraphStart && li > firstLine) y += paraGap;
      int x = margin;
      for (int wi = 0; wi < line.count; wi++) {
        const auto& word = words[line.startIdx + wi];
        renderer.drawText(fontId, x, y, word.c_str());
        x += renderer.getTextWidth(fontId, word.c_str()) + spaceWidth;
      }
      y += lineHeight;
    }

    // Scroll indicator
    int totalPages = (static_cast<int>(lines.size()) + linesPerPage - 1) / std::max(1, linesPerPage);
    if (totalPages > 1) {
      char scrollStr[16];
      snprintf(scrollStr, sizeof(scrollStr), "%d/%d", scrollOffset + 1, totalPages);
      int sw = renderer.getTextWidth(SMALL_FONT_ID, scrollStr);
      renderer.drawText(SMALL_FONT_ID, pageWidth - margin - sw, pageHeight - 28, scrollStr);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
  }
}
