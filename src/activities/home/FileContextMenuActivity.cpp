#include "FileContextMenuActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

FileContextMenuActivity::FileContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string title, bool showPreTranslation)
    : Activity("FileContextMenu", renderer, mappedInput),
      title(std::move(title)),
      showPreTranslation(showPreTranslation) {}

void FileContextMenuActivity::onEnter() {
  Activity::onEnter();

  const int fontId = UI_12_FONT_ID;
  const int maxWidth = renderer.getScreenWidth() - 40;
  safeTitle = renderer.truncatedText(fontId, title.c_str(), maxWidth, EpdFontFamily::BOLD);

  // Options are added in Choice order: DELETE (always) then PRE_TRANSLATION (EPUB only),
  // so the selected popup index maps directly onto the Choice enum value.
  const char* options[2];
  int optionCount = 0;
  options[optionCount++] = I18N.get(StrId::STR_DELETE);
  if (showPreTranslation) {
    options[optionCount++] = I18N.get(StrId::STR_PRE_TRANSLATION);
  }

  menuPopup.show(safeTitle.c_str(), options, optionCount, 0, [this](int idx) {
    // Options were pushed in Choice order, so the popup index is the Choice value.
    MenuResult menuResult;
    menuResult.action = idx;
    ActivityResult res{menuResult};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void FileContextMenuActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  if (menuPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void FileContextMenuActivity::loop() {
  if (menuPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
