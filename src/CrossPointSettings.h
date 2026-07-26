#pragma once
#include <ArduinoJson.h>
#include <Epub/PageFontSet.h>
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

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName).
  // Persisted by index, so NOTOSANS must stay 1 and new families must be appended.
  // Slot 0 held Noto Serif until EdsLab replaced it as the built-in serif/slab
  // reading family; a settings.json that stores 0 therefore keeps working and now
  // resolves to EdsLab, which is the intended migration (Noto Serif is no longer in
  // the firmware, and EdsLab occupies the same niche and is the new default).
  enum FONT_FAMILY { EDSLAB = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_NOTOSERIF = 0;
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader font size is a point size, not an enum slot — see fontPointSize.
  // Legacy 1.4-and-earlier files stored a 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE
  // slot; fromJson() folds that range up (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
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
  // fork numbered its tooltip mode 6 (its enum ordered TOOLTIP before its Modal mode); v2 had
  // already shipped that mode as 6 — PT_PAGE_TRANSLATION — so tooltip appends as 7.
  //
  // PERMANENT HOLES: 1 and 2. They were the "Dimmed" / "Dimmed Light" modes, which are now ONE
  // mode (PT_INTERLEAVED) plus the translationShade colour sub-setting. The two values are retired,
  // NEVER selectable (they are absent from PT_SELECTABLE_MODES in PreTranslationModes.h) and
  // migrated to PT_INTERLEAVED + shade at load (see fromJson). They are kept as holes — never
  // reused, never renumbered — so an old settings.json is migrated rather than reinterpreted.
  enum PRE_TRANSLATION_MODE : uint8_t {
    PT_NORMAL = 0,
    PT_LEGACY_DIMMED = 1,        // retired hole -> PT_INTERLEAVED + SHADE_DIMMED
    PT_LEGACY_DIMMED_LIGHT = 2,  // retired hole -> PT_INTERLEAVED + SHADE_DIMMED_LIGHT
    PT_ORIGINAL_ONLY = 3,
    PT_TRANSLATION_ONLY = 4,
    PT_SIDE_BY_SIDE = 5,
    PT_PAGE_TRANSLATION = 6,
    PT_TOOLTIP = 7,
    PT_INTERLEAVED = 8,
    // Each sentence's translation on its own small line ABOVE the source line it starts on. The one
    // mode with a layout that is not shared with any other (PtLayout::Interlinear).
    PT_INTERLINEAR = 9,
  };
  // LOAD-TIME VALIDITY BOUND ONLY: fromJson() clamps a stored translationDisplayMode >= this to
  // PT_NORMAL. It is deliberately NOT an enumerator and NOT a UI iteration count — the retired
  // holes at 1 and 2 make the value range non-contiguous, so every UI list and cycle walks
  // PT_SELECTABLE_MODES instead (src/PreTranslationModes.h).
  static constexpr uint8_t PT_MODE_COUNT = PT_INTERLINEAR + 1;

  // Pre-Translation: colour of translated text in Interleaved mode (PT_INTERLEAVED). It selects the
  // renderer's gray level for words carrying the TRANSLATED style bit.
  // DRAWING ONLY: it never changes word measurement, line breaking or pagination, so it must NOT
  // enter the section.bin cache key (ReaderRenderSpec) — switching shade stays instant.
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum TRANSLATION_SHADE : uint8_t { SHADE_DIMMED = 0, SHADE_DIMMED_LIGHT = 1, TRANSLATION_SHADE_COUNT };

  // Pre-Translation: type size of the TRANSLATED text relative to the book's own text.
  // SIZE_SMALLER means one step DOWN the active family's point-size ladder, resolved by
  // smallerReaderFontId().
  // ONE ENUM, THREE INDEPENDENT FIELDS: the value space is shared, the choice is not. Each mode
  // that shows translated text owns its own stored size (interleavedTranslationSize,
  // tooltipTranslationSize, pageTranslationSize) because the three answer different questions and
  // have different costs — the Interleaved size is a LAYOUT difference (narrower glyphs re-break
  // lines) and so enters the section cache key, while the two overlay sizes are composited at view
  // time over an unchanged page and must NOT invalidate anything. Sharing one field made shrinking
  // the tooltip silently re-lay out the whole book.
  // AVAILABILITY: SIZE_SMALLER is only offered when the active family actually ships a smaller
  // face; where it does not, smallerReaderFontId() returns 0 and everything behaves as SIZE_SAME
  // without the stored value being rewritten (see smallerReaderFontId()).
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum TRANSLATION_SIZE : uint8_t { SIZE_SAME = 0, SIZE_SMALLER = 1, TRANSLATION_SIZE_COUNT };

  // Pre-Translation feature: translation backend selection
  // Values match upstream fork (crosspoint-reader) to keep JSON-stored indices stable.
  // VALUE STABILITY: persisted as an integer — append only, never renumber.
  enum TRANSLATION_ENGINE : uint8_t {
    ENGINE_GOOGLE_FREE = 0,
    ENGINE_DEEPL = 1,
    ENGINE_DEEPL_PRO = 2,
    ENGINE_OPENAI = 3,
    ENGINE_DEEPSEEK = 4,
    ENGINE_GEMINI = 5,
    ENGINE_GOOGLE_V2 = 6,
    ENGINE_GOOGLE_HTML = 7,
    // Microsoft's Edge-browser translator deployment (api-edge.cognitive.microsofttranslator.com),
    // authenticated with an anonymous short-lived token from edge.microsoft.com/translate/auth.
    // This is NOT the paid Azure Translator resource (api.cognitive.microsofttranslator.com),
    // which would need a subscription key + region — this endpoint is keyless, so no UI is needed.
    ENGINE_AZURE = 8,
    TRANSLATION_ENGINE_COUNT
  };

  // Which physical button pair drives a translation overlay (tooltip sentence stepping /
  // Page Translation scrolling). Shared by tooltipButtons and pageTranslationButtons.
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
  // Reader font settings. EdsLab is the default reading font for a fresh install;
  // fromJson() only falls back to this when the "fontFamily" key is absent, so a
  // user who already picked Noto Sans (or an SD family) keeps their choice.
  uint8_t fontFamily = EDSLAB;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
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
  // Interleaved-mode (PT_INTERLEAVED) translated-text colour. Drawing-only; see TRANSLATION_SHADE.
  uint8_t translationShade = SHADE_DIMMED;
  // Translated-text type size — ONE field per mode that shows translated text, never shared (see
  // TRANSLATION_SIZE). The defaults differ on purpose and each is the mode's own pre-existing
  // behaviour, so an upgrade changes nothing on screen:
  //  - Interleaved draws the translation in the main flow, where a smaller face would re-break every
  //    line; it has always matched the body text, so SIZE_SAME.
  //  - Tooltip and Page Translation composite an overlay over the page, and both have ALWAYS drawn it
  //    one step down the ladder (getTooltipFontId() called smallerReaderFontId() unconditionally
  //    before the row existed), so SIZE_SMALLER. Defaulting these to Same would have handed every
  //    existing user body-size overlays on upgrade.
  // These three defaults are mirrored in fromJson(); keep the pairs in sync.
  uint8_t interleavedTranslationSize = SIZE_SAME;
  uint8_t tooltipTranslationSize = SIZE_SMALLER;
  uint8_t pageTranslationSize = SIZE_SMALLER;
  // Tooltip display mode (PT_TOOLTIP) controls. Ported from the upstream fork.
  // tooltipButtons: which button pair steps through per-sentence tooltips (OVERLAY_BUTTONS).
  //   Default SIDE — the page-turn pair reads as the natural "next sentence" control.
  // tooltipBehavior: what stepping does at a page boundary (TOOLTIP_NAVIGATION).
  //   Default TURN_PAGE — stepping past the last sentence turns the page and continues.
  // Persisted manually in toJson/fromJson alongside the other Pre-Translation fields (they are
  // edited from the Lingua submenu, not the generic on-device Settings list).
  uint8_t tooltipButtons = OVERLAY_BUTTONS_SIDE;
  uint8_t tooltipBehavior = TOOLTIP_NAV_TURN_PAGE;
  // Page Translation display mode (PT_PAGE_TRANSLATION) control: which button pair scrolls/closes
  // the OPEN overlay (OVERLAY_BUTTONS). The overlay still OPENS on a side long-press regardless of
  // this setting. Default SIDE (same pair that opened it). Persisted manually in toJson/fromJson,
  // under the "pageTranslationButtons" key (legacy files stored it as "modalButtons"; fromJson
  // reads that as a fallback and resaves).
  uint8_t pageTranslationButtons = OVERLAY_BUTTONS_SIDE;

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

  // THE "one size smaller than the body text" resolver: the reader font id one step DOWN the active
  // family's point-size ladder, or 0 when the family has no smaller face. 0 is the single signal
  // that a smaller size is unavailable — callers either fall back to the body font (tooltip text) or
  // withhold the Smaller option from the UI (the Lingua submenu's Translation Size row).
  //
  // Runs from the page render loop and from the cache-key path, so it must not allocate: it walks
  // BUILTIN_READER_POINT_SIZES directly rather than building the readerFontPointSizes() vector.
  int smallerReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

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

  // Pre-Translation: which page LAYOUT a display mode implies. This is THE mode -> layout mapping;
  // the layout engine never sees the raw mode. Modes that produce byte-identical pages collapse
  // onto one PtLayout so switching between them reuses the cached section instead of re-laying the
  // chapter out. A switch with no `default:` case, so -Wswitch flags an unmapped future mode.
  static PtLayout ptLayoutForDisplayMode(uint8_t mode);

  // Pre-Translation: font id the TRANSLATED text is drawn in, or 0 for "same as the body font".
  // FOUR resolvers, one per owning mode — deliberately not one shared accessor, so that the two
  // layout-affecting choices and the two view-time ones cannot be confused at a call site.
  //
  // LAYOUT (Interleaved): one of the two that reach the cache. It feeds BOTH the section cache key
  // (ReaderRenderSpec::translationFontId) and the render-time font set (readerPageFontSet), so the
  // two can never disagree about what a cached page was measured with. Returns 0 whenever the active
  // mode is not Interleaved — the mode gate has to live here rather than in the layout engine,
  // which only ever sees the PtLayout that Normal and Interleaved share.
  int getInterleavedTranslationFontId() const;
  // LAYOUT (Interlinear): the other one that reaches the cache, and THE single place the small
  // annotation face is chosen — a future user-facing Annotation Size row, or a smaller face merging
  // from another branch, plugs in here and is picked up by the layout engine, the renderer and the
  // cache key at once. Returns 0 (= the body font) when the mode is not Interlinear, and also when the
  // annotation face cannot cover the selected target script, which degrades the rows to body size
  // instead of a page of replacement glyphs.
  int getInterlinearAnnotationFontId() const;
  // VIEW TIME (Tooltip / Page Translation): composited over an already-laid-out page, so neither may
  // appear in a ReaderRenderSpec — changing one must not invalidate a single cached chapter. Read
  // only by getTooltipFontId() / getPageTranslationFontId(), which turn 0 into the body font.
  int getTooltipTranslationFontId() const;
  int getPageTranslationOverlayFontId() const;

  // THE construction point for the reader's per-role font ids. Every path that draws a Page must
  // build its PageFontSet here, so pages are drawn with exactly the ids readerRenderSpec() keyed
  // the cache on. lib/Epub only stores roles; this is where they become concrete fonts.
  PageFontSet readerPageFontSet() const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;

 private:
  // THE single TRANSLATION_SIZE -> font id rule behind the three size accessors above: SIZE_SAME (and
  // a SIZE_SMALLER the active family cannot honour) resolve to 0, the "same as the body font" signal.
  int translationFontIdForSize(uint8_t sizeSetting) const;
  // True when the small annotation face can render the selected Pre-Translation target language's
  // script. See the definition for the exact coverage of the 8pt face and why an uncovered target
  // falls back to the body font rather than being blocked.
  bool interlinearAnnotationScriptSupported() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
