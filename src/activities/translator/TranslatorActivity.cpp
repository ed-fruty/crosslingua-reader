#include "TranslatorActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstring>

#include "LanguagePickerActivity.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ─── helpers ──────────────────────────────────────────────────────────────────

std::string TranslatorActivity::buildOutputPath() const {
  // "/path/to/MyBook.epub"  →  "/path/to/MyBook_uk.epub"
  std::string out = epubPath;
  const size_t dot = out.rfind('.');
  const std::string suffix = std::string("_") + targetLangCode;
  if (dot != std::string::npos) {
    out.insert(dot, suffix);
  } else {
    out += suffix;
  }
  return out;
}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void TranslatorActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state = LANG_SELECTION;

  // Show language picker immediately
  enterNewActivity(new LanguagePickerActivity(
      renderer, mappedInput,
      [this](const char* code) {
        // Find display name for the selected code
        for (int i = 0; i < LanguagePickerActivity::NUM_LANGUAGES; i++) {
          if (strcmp(LanguagePickerActivity::LANGUAGES[i].code, code) == 0) {
            onLangSelected(code, LanguagePickerActivity::LANGUAGES[i].name);
            return;
          }
        }
        onLangSelected(code, code);
      },
      [this] {
        // User cancelled language selection → leave translator
        auto cb = onDone;
        cb();
      }));
}

void TranslatorActivity::onExit() {
  ActivityWithSubactivity::onExit();
  translator.cancel();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// ─── state transitions ────────────────────────────────────────────────────────

void TranslatorActivity::onLangSelected(const char* code, const char* name) {
  exitActivity();  // exit the language picker subactivity
  targetLangCode = code;
  targetLangName = name;
  outputPath = buildOutputPath();
  state = WIFI_SELECTION;
  requestUpdate();

  // Turn on WiFi
  WiFi.mode(WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected(true);
    return;
  }
  enterNewActivity(
      new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) { onWifiConnected(connected); }));
}

void TranslatorActivity::onWifiConnected(bool success) {
  exitActivity();  // exit wifi picker if it was launched
  if (!success) {
    state = FAILED;
    snprintf(lastProgress.statusMsg, sizeof(lastProgress.statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

void TranslatorActivity::startTranslation() {
  state = TRANSLATING;
  lastProgress = {};
  requestUpdate();
  translator.start(epubPath, targetLangCode.c_str(), outputPath);
}

// ─── loop / render ────────────────────────────────────────────────────────────

void TranslatorActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == TRANSLATING) {
    EpubTranslator::Progress p = translator.getProgress();
    // Trigger re-render when progress changes
    if (p.currentChapter != lastProgress.currentChapter || p.done != lastProgress.done ||
        p.failed != lastProgress.failed || p.cancelled != lastProgress.cancelled) {
      lastProgress = p;
      if (p.done) state = DONE;
      else if (p.failed) state = FAILED;
      else if (p.cancelled) state = CANCELLED;
      requestUpdate();
    }
  }

  // Any button while showing result/error → leave
  if (state == DONE || state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      auto cb = onDone;
      cb();
      return;
    }
  }

  // Back during translation → cancel
  if (state == TRANSLATING && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    translator.cancel();
    // Will transition to CANCELLED when translator reports it
  }
}

void TranslatorActivity::render(RenderLock&&) {
  if (subActivity) return;

  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int cx = pageWidth / 2;

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_TRANSLATE_BOOK), true, EpdFontFamily::BOLD);

  // Language line
  if (!targetLangName.empty()) {
    std::string langLine = std::string(tr(STR_TRANSLATE_TO)) + " " + targetLangName;
    renderer.drawCenteredText(UI_10_FONT_ID, 50, langLine.c_str());
  }

  const int barY = 200;
  const int barH = 40;
  const int barW = pageWidth - 60;
  const int barX = 30;

  if (state == TRANSLATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_TRANSLATING_BOOK));

    // Progress bar
    renderer.drawRect(barX, barY, barW, barH);
    if (lastProgress.totalChapters > 0) {
      const float pct =
          static_cast<float>(lastProgress.currentChapter) / static_cast<float>(lastProgress.totalChapters);
      const int filled = static_cast<int>(pct * (barW - 4));
      if (filled > 0) renderer.fillRect(barX + 2, barY + 2, filled, barH - 4);
    }

    // Chapter counter
    char chap[64];
    snprintf(chap, sizeof(chap), "%s %d / %d", tr(STR_CHAPTER_PREFIX), lastProgress.currentChapter,
             lastProgress.totalChapters);
    renderer.drawCenteredText(UI_10_FONT_ID, barY + barH + 16, chap);

    // Cancel hint
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_BACK_TO_CANCEL));

  } else if (state == DONE) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_DONE), true, EpdFontFamily::BOLD);

    // Show output filename (basename only)
    const size_t slash = outputPath.rfind('/');
    const std::string fname = (slash != std::string::npos) ? outputPath.substr(slash + 1) : outputPath;
    renderer.drawCenteredText(UI_10_FONT_ID, 200, fname.c_str());

    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));

  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_FAILED), true, EpdFontFamily::BOLD);
    if (lastProgress.statusMsg[0]) {
      renderer.drawCenteredText(UI_10_FONT_ID, 200, lastProgress.statusMsg);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));

  } else if (state == CANCELLED) {
    renderer.drawCenteredText(UI_12_FONT_ID, 150, tr(STR_TRANSLATION_CANCELLED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 380, tr(STR_PRESS_ANY_CONTINUE));
  }

  const auto labels = mappedInput.mapLabels(state == TRANSLATING ? tr(STR_CANCEL) : tr(STR_BACK),
                                            state == TRANSLATING ? "" : tr(STR_OK_BUTTON), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
