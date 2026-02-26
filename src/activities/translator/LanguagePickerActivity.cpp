#include "LanguagePickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const LanguagePickerActivity::Language LanguagePickerActivity::LANGUAGES[] = {
    {"Ukrainian", "uk"},
    {"Russian", "ru"},
    {"English", "en"},
    {"Spanish", "es"},
    {"French", "fr"},
    {"German", "de"},
    {"Polish", "pl"},
    {"Czech", "cs"},
    {"Portuguese", "pt"},
    {"Italian", "it"},
    {"Chinese (Simplified)", "zh-CN"},
    {"Japanese", "ja"},
    {"Korean", "ko"},
    {"Arabic", "ar"},
    {"Turkish", "tr"},
    {"Dutch", "nl"},
    {"Swedish", "sv"},
    {"Norwegian", "no"},
    {"Finnish", "fi"},
    {"Romanian", "ro"},
    {"Hungarian", "hu"},
};
const int LanguagePickerActivity::NUM_LANGUAGES =
    static_cast<int>(sizeof(LANGUAGES) / sizeof(LANGUAGES[0]));

void LanguagePickerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void LanguagePickerActivity::onExit() { Activity::onExit(); }

void LanguagePickerActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, NUM_LANGUAGES);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, NUM_LANGUAGES);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    auto cb = onSelect;
    auto code = LANGUAGES[selectedIndex].code;
    cb(code);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    auto cb = onCancel;
    cb();
    return;
  }
}

void LanguagePickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_SELECT_TRANSLATE_LANG), true, EpdFontFamily::BOLD);

  // Scrolling list — show a window of entries centred on selection
  constexpr int LINE_H = 28;
  const int startY = 55;
  const int visibleRows = (pageHeight - startY - 40) / LINE_H;
  const int halfVisible = visibleRows / 2;

  int startIdx = selectedIndex - halfVisible;
  if (startIdx < 0) startIdx = 0;
  if (startIdx + visibleRows > NUM_LANGUAGES) startIdx = NUM_LANGUAGES - visibleRows;
  if (startIdx < 0) startIdx = 0;

  for (int row = 0; row < visibleRows && startIdx + row < NUM_LANGUAGES; row++) {
    const int idx = startIdx + row;
    const int y = startY + row * LINE_H;
    const bool sel = (idx == selectedIndex);

    if (sel) renderer.fillRect(0, y, pageWidth - 1, LINE_H, true);
    renderer.drawText(UI_10_FONT_ID, 20, y, LANGUAGES[idx].name, !sel);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
