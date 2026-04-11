#pragma once
#include <Epub.h>
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
 public:
  // Menu actions available from the reader menu.
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
    TRANSLATE_BOOK
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const uint8_t currentTranslationMode,
                                  const uint8_t currentFontFamily, const uint8_t currentFontSize,
                                  const uint8_t currentLineSpacing, const bool chapterIsTranslated,
                                  const std::function<void(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t)>& onBack,
                                  const std::function<void(MenuAction)>& onAction)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        title(title),
        pendingOrientation(currentOrientation),
        pendingTranslationMode(currentTranslationMode),
        pendingFontFamily(currentFontFamily),
        pendingFontSize(currentFontSize),
        pendingLineSpacing(currentLineSpacing),
        currentPage(currentPage),
        totalPages(totalPages),
        bookProgressPercent(bookProgressPercent),
        onBack(onBack),
        onAction(onAction) {
    // Build menu items with dynamic translate label based on chapter translation state
    menuItems = {{MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER},
                 {MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION},
                 {MenuAction::CYCLE_FONT_FAMILY, StrId::STR_FONT_FAMILY},
                 {MenuAction::CYCLE_FONT_SIZE, StrId::STR_FONT_SIZE},
                 {MenuAction::CYCLE_LINE_SPACING, StrId::STR_LINE_SPACING},
                 {MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT},
                 {MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON},
                 {MenuAction::SYNC, StrId::STR_SYNC_PROGRESS},
                 {MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE},
                 {MenuAction::TRANSLATE_CHAPTER,
                  chapterIsTranslated ? StrId::STR_RETRANSLATE_CHAPTER : StrId::STR_TRANSLATE_CHAPTER},
                 {MenuAction::TRANSLATE_PAGE, StrId::STR_TRANSLATE_PAGE},
                 {MenuAction::TRANSLATE_BOOK, StrId::STR_TRANSLATE_BOOK},
                 {MenuAction::CYCLE_TRANSLATION_MODE, StrId::STR_TRANSLATION_MODE}};
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  // Menu layout (order matters for up/down navigation). Built in constructor.
  std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  uint8_t pendingTranslationMode = 0;
  const std::vector<StrId> translationModeLabels = {StrId::STR_NORMAL, StrId::STR_TRANSLATION_GREY,
                                                    StrId::STR_TRANSLATION_LIGHT_GREY, StrId::STR_NO_RENDER,
                                                    StrId::STR_INVERT_TRANSLATION, StrId::STR_SIDE_BY_SIDE,
                                                    StrId::STR_TOOLTIP};
  uint8_t pendingFontFamily = 0;
  const std::vector<StrId> fontFamilyLabels = {StrId::STR_BOOKERLY, StrId::STR_EDSLAB, StrId::STR_ALEGREYA,
                                                StrId::STR_GPRO};
  uint8_t pendingFontSize = 0;
  const std::vector<StrId> fontSizeLabels = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE,
                                             StrId::STR_X_LARGE};
  uint8_t pendingLineSpacing = 10;
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;

  const std::function<void(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t)> onBack;
  const std::function<void(MenuAction)> onAction;
};
