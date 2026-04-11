# Full Book Translation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Translate Book" menu item that translates all EPUB chapters sequentially using the existing streaming translation engine, for offline reading preparation.

**Architecture:** New `BookTranslatorActivity` mirrors `ChapterTranslatorActivity`'s state machine but loops through all spine items. Reuses `TranslatingHtmlRewriter::rewriteFromFile()` for each chapter. WiFi connects once. Memory stays flat (~7KB peak per chapter).

**Tech Stack:** C++20, PlatformIO/Arduino, ESP32-C3, expat XML parser, FreeRTOS tasks

---

### Task 1: Add I18n strings

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `lib/I18n/translations/czech.yaml`
- Modify: `lib/I18n/translations/french.yaml`
- Modify: `lib/I18n/translations/german.yaml`
- Modify: `lib/I18n/translations/portuguese.yaml`
- Modify: `lib/I18n/translations/russia.yaml`
- Modify: `lib/I18n/translations/spanish.yaml`
- Modify: `lib/I18n/translations/swedish.yaml`

- [ ] **Step 1: Add strings to english.yaml**

Append after the last line:

```yaml
STR_TRANSLATE_BOOK: "Translate Book"
STR_TRANSLATING_BOOK: "Translating Book"
STR_RETRANSLATE_BOOK: "Re-translate Book"
STR_SKIP_TRANSLATED: "Skip Translated"
STR_RETRANSLATE_ALL: "Re-translate All"
STR_CHAPTERS_ALREADY_TRANSLATED: "chapters already translated"
STR_BOOK_TRANSLATION_DONE: "Book translation complete"
STR_BOOK_TRANSLATION_CANCELLED: "Book translation cancelled"
```

- [ ] **Step 2: Add same strings to all other 7 language YAML files**

Append the same English strings to each file (they serve as fallbacks until translated):

```bash
for f in lib/I18n/translations/*.yaml; do
  if [ "$f" != "lib/I18n/translations/english.yaml" ]; then
    echo '' >> "$f"
    echo 'STR_TRANSLATE_BOOK: "Translate Book"' >> "$f"
    echo 'STR_TRANSLATING_BOOK: "Translating Book"' >> "$f"
    echo 'STR_RETRANSLATE_BOOK: "Re-translate Book"' >> "$f"
    echo 'STR_SKIP_TRANSLATED: "Skip Translated"' >> "$f"
    echo 'STR_RETRANSLATE_ALL: "Re-translate All"' >> "$f"
    echo 'STR_CHAPTERS_ALREADY_TRANSLATED: "chapters already translated"' >> "$f"
    echo 'STR_BOOK_TRANSLATION_DONE: "Book translation complete"' >> "$f"
    echo 'STR_BOOK_TRANSLATION_CANCELLED: "Book translation cancelled"' >> "$f"
  fi
done
```

- [ ] **Step 3: Regenerate I18n code**

```bash
python3 scripts/gen_i18n.py
```

Expected: `Generated: lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`

- [ ] **Step 4: Build to verify**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 5: Commit**

```bash
git add lib/I18n/
git commit -m "feat: add i18n strings for full book translation"
```

---

### Task 2: Create BookTranslatorActivity header

**Files:**
- Create: `src/activities/reader/BookTranslatorActivity.h`

- [ ] **Step 1: Create the header file**

```cpp
#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <string>

#include "../ActivityWithSubactivity.h"
#include "translator/TranslatingHtmlRewriter.h"

/**
 * Translates ALL chapters of an EPUB sequentially using the streaming translation engine.
 * Same blocking UI pattern as ChapterTranslatorActivity but loops through all spine items.
 * WiFi connects once and stays on for the entire book.
 *
 * Flow:
 *  1. Source language picker
 *  2. Target language picker
 *  3. Scan for already-translated chapters → confirm re-translate or skip
 *  4. Connect to WiFi
 *  5. Loop: extract chapter → count blocks → translate file-to-file → next chapter
 *  6. Show result → call onComplete/onCancel
 */
class BookTranslatorActivity final : public ActivityWithSubactivity {
 public:
  enum State {
    SOURCE_LANG_SELECTION,
    LANG_SELECTION,
    CONFIRM_RETRANSLATE,
    WIFI_SELECTION,
    TRANSLATING,
    DONE,
    FAILED,
    CANCELLED,
  };

  explicit BookTranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::shared_ptr<Epub>& epub,
                                  const std::function<void()>& onCancel,
                                  const std::function<void()>& onComplete)
      : ActivityWithSubactivity("BookTranslator", renderer, mappedInput),
        epub(epub),
        onCancel(onCancel),
        onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::shared_ptr<Epub> epub;
  State state = SOURCE_LANG_SELECTION;

  // Language selection
  std::string sourceLangCode = "auto";
  std::string sourceLangName = "Auto Detect";
  std::string targetLangCode;
  std::string targetLangName;

  // Re-translate confirmation
  bool skipTranslated = false;
  int alreadyTranslatedCount = 0;
  int totalChapters = 0;
  int confirmSelection = 0;  // 0=skip translated, 1=re-translate all

  // Translation task
  TaskHandle_t taskHandle = nullptr;
  volatile bool cancelFlag = false;
  volatile bool taskDone = false;
  volatile bool taskFailed = false;
  char statusMsg[64] = {};

  // Progress tracking (volatile for IPC between FreeRTOS task and main loop)
  volatile int currentChapter = 0;     // 0-based chapter being translated
  volatile int chaptersCompleted = 0;
  volatile int progressCurrent = 0;    // paragraph progress within current chapter
  volatile int progressTotal = 0;
  unsigned long lastProgressUpdate = 0;

  const std::function<void()> onCancel;
  const std::function<void()> onComplete;

  void launchSourcePicker();
  void launchTargetPicker();
  void onSourceLangSelected(const char* code, const char* name);
  void onLangSelected(const char* code, const char* name);
  void scanAlreadyTranslated();
  void onWifiConnected(bool success);
  void startTranslation();
  static void translationTask(void* param);
  void runTranslation();
  const char* getEngineName() const;
};
```

- [ ] **Step 2: Build to verify header compiles**

```bash
pio run
```

Expected: SUCCESS (header not yet included anywhere, but syntax checked if any other file pulls it in indirectly — otherwise just verify no typos)

- [ ] **Step 3: Commit**

```bash
git add src/activities/reader/BookTranslatorActivity.h
git commit -m "feat: add BookTranslatorActivity header"
```

---

### Task 3: Implement BookTranslatorActivity

**Files:**
- Create: `src/activities/reader/BookTranslatorActivity.cpp`

- [ ] **Step 1: Create the implementation file**

```cpp
#include "BookTranslatorActivity.h"

#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "../network/WifiSelectionActivity.h"
#include "../translator/LanguagePickerActivity.h"
#include "fontIds.h"
#include "gui/GUI.h"

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
  // If user previously selected a target language, pre-select it
  const char* preselect = nullptr;
  if (SETTINGS.translationLanguage < LanguagePickerActivity::NUM_LANGUAGES) {
    preselect = LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].code;
  }
  enterNewActivity(new LanguagePickerActivity(
      renderer, mappedInput,
      [this](const char* code) {
        for (int i = 0; i < LanguagePickerActivity::NUM_LANGUAGES; i++) {
          if (strcmp(LanguagePickerActivity::LANGUAGES[i].code, code) == 0) {
            SETTINGS.translationLanguage = i;
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
    // No existing translations — go straight to WiFi
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

  LOG_DBG("BKT", "Starting book translation: %d chapters, lang=%s, engine=%d",
          totalChapters, targetLangCode.c_str(), SETTINGS.translationEngine);

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
                                           SETTINGS.translationEngine, SETTINGS.translateApiKey,
                                           &cancelFlag, &progressCurrent);
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
    LOG_DBG("BKT", "Chapter %d/%d: done (%d translated, %d skipped)",
            si + 1, totalChapters, result.paragraphsTranslated, result.paragraphsSkipped);
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

  // Result screens: any button → exit
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
    case 0: return "Google Translate";
    case 1: return "DeepL";
    case 2: return "OpenAI";
    case 3: return "DeepSeek";
    case 4: return "Gemini";
    default: return "Unknown";
  }
}
```

- [ ] **Step 2: Build to verify**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 3: Commit**

```bash
git add src/activities/reader/BookTranslatorActivity.h src/activities/reader/BookTranslatorActivity.cpp
git commit -m "feat: implement BookTranslatorActivity"
```

---

### Task 4: Add TRANSLATE_BOOK menu item

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.h`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp` (if needed for menu item ordering)

- [ ] **Step 1: Add TRANSLATE_BOOK to the MenuAction enum**

In `src/activities/reader/EpubReaderMenuActivity.h`, add `TRANSLATE_BOOK` to the enum after `TRANSLATE_PAGE`:

```cpp
enum class MenuAction {
  SELECT_CHAPTER,
  GO_TO_PERCENT,
  ROTATE_SCREEN,
  CYCLE_TRANSLATION_MODE,
  CYCLE_FONT_FAMILY,
  CYCLE_FONT_SIZE,
  CYCLE_LINE_SPACING,
  GO_HOME,
  SYNC,
  DELETE_CACHE,
  TRANSLATE_CHAPTER,
  TRANSLATE_PAGE,
  TRANSLATE_BOOK,       // <-- add this
};
```

- [ ] **Step 2: Add menu item to the items list**

In the constructor's `menuItems` initialization (same file), add the new entry after TRANSLATE_PAGE:

```cpp
{MenuAction::TRANSLATE_PAGE, StrId::STR_TRANSLATE_PAGE},
{MenuAction::TRANSLATE_BOOK, StrId::STR_TRANSLATE_BOOK},  // <-- add this line
{MenuAction::CYCLE_TRANSLATION_MODE, StrId::STR_TRANSLATION_MODE}};
```

- [ ] **Step 3: Build to verify**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add src/activities/reader/EpubReaderMenuActivity.h
git commit -m "feat: add Translate Book menu item"
```

---

### Task 5: Wire up BookTranslatorActivity in EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [ ] **Step 1: Add pending flag and include**

In `src/activities/reader/EpubReaderActivity.h`, add near the existing `pendingTranslateChapter`:

```cpp
bool pendingTranslateBook = false;
```

- [ ] **Step 2: Handle TRANSLATE_BOOK menu action**

In `src/activities/reader/EpubReaderActivity.cpp`, find the `case EpubReaderMenuActivity::MenuAction::TRANSLATE_CHAPTER:` block and add a new case after it:

```cpp
case EpubReaderMenuActivity::MenuAction::TRANSLATE_BOOK: {
  if (epub) {
    pendingTranslateBook = true;
    exitActivity();  // close menu; loop() will launch BookTranslatorActivity
  }
  break;
}
```

- [ ] **Step 3: Add include for BookTranslatorActivity**

At the top of `src/activities/reader/EpubReaderActivity.cpp`, add:

```cpp
#include "BookTranslatorActivity.h"
```

- [ ] **Step 4: Add deferred launch logic**

In `src/activities/reader/EpubReaderActivity.cpp`, find the block that checks `pendingTranslateChapter` (around line 169). Add a similar block right after it for `pendingTranslateBook`:

```cpp
// Deferred book translate: launch BookTranslatorActivity after menu subactivity is gone
if (pendingTranslateBook) {
  pendingTranslateBook = false;
  if (epub) {
    enterNewActivity(new BookTranslatorActivity(
        renderer, mappedInput, epub,
        [this] {
          // Cancel: return to reader
          exitActivity();
          requestUpdate();
          skipNextButtonCheck = true;
        },
        [this] {
          // Complete: clear current chapter cache so it rebuilds from translated HTML
          exitActivity();
          {
            RenderLock lock(*this);
            if (section) {
              cachedSpineIndex = currentSpineIndex;
              cachedChapterTotalPageCount = section->pageCount;
              nextPageNumber = section->currentPage;
              section->clearCache();
            }
            section.reset();
          }
          requestUpdate();
          skipNextButtonCheck = true;
        }));
  }
  return;
}
```

- [ ] **Step 5: Build and verify**

```bash
pio run
```

Expected: SUCCESS

- [ ] **Step 6: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp
git commit -m "feat: wire BookTranslatorActivity into reader menu"
```

---

### Task 6: Final build and integration test

- [ ] **Step 1: Full clean build**

```bash
pio run
```

Expected: SUCCESS. Check RAM usage stays under 35%.

- [ ] **Step 2: Manual test checklist**

1. Open a book → reader menu → verify "Translate Book" appears
2. Select "Translate Book" → source language picker appears
3. Select source → target language picker appears
4. If chapters already translated → confirm screen with "Skip Translated" / "Re-translate All"
5. WiFi connection → translation starts
6. Progress shows "Chapter X/Y" + paragraph progress bar
7. Wait for completion → "Book translation complete. X/Y chapters"
8. Press button → returns to reader, current chapter re-renders with translations
9. Navigate to other chapters → they have translations too
10. Test cancel mid-translation → "X/Y chapters completed", completed ones preserved

- [ ] **Step 3: Commit with all changes**

```bash
git add -A
git commit -m "feat: full book translation - integration verified"
```
