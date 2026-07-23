#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <PersistableStore.h>

#include <cstdint>

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE { STATUS_BAR_CLOCK_HIDE = 0, STATUS_BAR_CLOCK_RIGHT = 1, STATUS_BAR_CLOCK_LEFT = 2 };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTONS_DISABLED = 2, SIDE_BUTTON_LAYOUT_COUNT };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { NOTOSERIF = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };

  // Long-press Confirm action while reading an EPUB. The setting cycles through these values.
  // Persisted in settings.json by index: any new function (e.g. dictionary, bookmark) MUST use a
  // value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the
  // stored indices shift and existing saves are silently misinterpreted.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Pre-Translation feature: how translated text renders on e-ink.
  // VALUE STABILITY: translationDisplayMode persists as this integer in settings.json, so new
  // modes MUST be APPENDED at the end — never inserted or renumbered — or existing on-device
  // saves are silently reinterpreted. PT_TOOLTIP is therefore 7 here even though the upstream
  // fork numbered its tooltip mode 6 (its enum ordered TOOLTIP before MODAL); v2 already shipped
  // PT_MODAL = 6, so tooltip appends as 7.
  enum PRE_TRANSLATION_MODE : uint8_t {
    PT_NORMAL = 0,
    PT_DARK = 1,
    PT_LIGHT = 2,
    PT_ORIGINAL_ONLY = 3,
    PT_TRANSLATION_ONLY = 4,
    PT_SIDE_BY_SIDE = 5,
    PT_MODAL = 6,
    PT_TOOLTIP = 7,
    PT_MODE_COUNT
  };

  // Pre-Translation feature: translation backend selection
  // Values match upstream fork (crosspoint-reader) to keep JSON-stored indices stable.
  enum TRANSLATION_ENGINE : uint8_t {
    ENGINE_GOOGLE_FREE = 0,
    ENGINE_DEEPL = 1,
    ENGINE_DEEPL_PRO = 2,
    ENGINE_OPENAI = 3,
    ENGINE_DEEPSEEK = 4,
    ENGINE_GEMINI = 5,
    ENGINE_GOOGLE_V2 = 6,
    ENGINE_GOOGLE_HTML = 7,
    TRANSLATION_ENGINE_COUNT
  };

  // Which physical button pair drives a translation overlay (tooltip sentence stepping /
  // modal scrolling). Shared by tooltipButtons and modalButtons.
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum OVERLAY_BUTTONS : uint8_t {
    OVERLAY_BUTTONS_FRONT = 0,  // front pair (Left / Right)
    OVERLAY_BUTTONS_SIDE = 1,   // side pair (PageBack / PageForward)
    OVERLAY_BUTTONS_COUNT
  };

  // What tooltip stepping does when it reaches a page boundary (last/first sentence).
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum TOOLTIP_NAVIGATION : uint8_t {
    TOOLTIP_NAV_LOOP = 0,       // wrap to the first/last sentence, stay on the page
    TOOLTIP_NAV_TURN_PAGE = 1,  // turn the page and continue stepping on the next page
    TOOLTIP_NAVIGATION_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = NOTOSERIF;
  uint8_t fontSize = MEDIUM;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  // Pre-Translation feature
  // translationLanguage: index into LanguagePickerActivity::LANGUAGES[], 0xFF = unset
  uint8_t translationLanguage = 0xFF;
  // sourceTranslationLanguage: 0xFF = auto-detect, otherwise LANGUAGES[] index
  uint8_t sourceTranslationLanguage = 0xFF;
  uint8_t translationEngine = ENGINE_GOOGLE_V2;
  char translateApiKey[128] = "";
  uint8_t translationDisplayMode = PT_NORMAL;
  // Tooltip display mode (PT_TOOLTIP) controls. Ported from the upstream fork.
  // tooltipButtons: which button pair steps through per-sentence tooltips (OVERLAY_BUTTONS).
  //   Default SIDE — the page-turn pair reads as the natural "next sentence" control.
  // tooltipBehavior: what stepping does at a page boundary (TOOLTIP_NAVIGATION).
  //   Default TURN_PAGE — stepping past the last sentence turns the page and continues.
  // Persisted manually in toJson/fromJson alongside the other Pre-Translation fields (they are
  // edited from the Bilingua submenu, not the generic on-device Settings list).
  uint8_t tooltipButtons = OVERLAY_BUTTONS_SIDE;
  uint8_t tooltipBehavior = TOOLTIP_NAV_TURN_PAGE;
  // Modal display mode (PT_MODAL) control: which button pair scrolls/closes the OPEN modal
  // (OVERLAY_BUTTONS). The modal still OPENS on a side long-press regardless of this setting.
  // Default SIDE (same pair that opened it). Persisted manually in toJson/fromJson.
  uint8_t modalButtons = OVERLAY_BUTTONS_SIDE;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    uint8_t clockUtcOffsetQ = 48;             // 48 = UTC+0
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
