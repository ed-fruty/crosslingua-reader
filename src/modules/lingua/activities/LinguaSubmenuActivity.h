#pragma once
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <MappedInputManager.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Typed request the submenu hands back to the reader when the user picks a
// translate action. The submenu itself no longer launches the translator (that
// would keep the live reader + Section + Epub resident during the TLS handshake
// and starve wolfSSL of heap); instead it finishes with one of these and the
// reader runs launchTranslation() after releasing the Epub. Encoded in the
// returned ActivityResult's MenuResult::action field.
enum class LinguaResult : uint8_t {
  NONE = 0,
  TRANSLATE_CHAPTER = 1,
  TRANSLATE_BOOK = 2,
  // Word Lens pack download / delete. Leaves the reader for the same heap reason as translation.
  MANAGE_WORD_LENS_PACKS = 3,
};

/**
 * Consolidated submenu that aggregates every Lingua action and setting
 * into a single screen launched from the EPUB reader menu.
 *
 * Mirrors EpubReaderMenuActivity: a single FreeInkUI list screen (UiListActivity), so it gets
 * touch, swipe scrolling and button navigation from the same base every upstream list uses.
 * Selecting most items
 * spawns a child activity (translate / language picker / API-key keyboard);
 * cyclical items mutate SETTINGS and re-render in place.
 *
 * After any child activity returns, the menu re-scans the on-disk translation
 * state so labels like "Translate Chapter" flip to "Re-translate Chapter" and
 * the "Delete Translations" entry appears once any chapter has been translated.
 */
class LinguaSubmenuActivity final : public UiListActivity {
 public:
  enum class Action : uint8_t {
    CYCLE_DISPLAY_MODE,
    TRANSLATE_CHAPTER,
    TRANSLATE_BOOK,
    DELETE_TRANSLATIONS,
    PICK_TARGET_LANG,
    PICK_SOURCE_LANG,
    CYCLE_ENGINE,
    ENTER_API_KEY,
    // Tooltip-mode (LINGUA_TOOLTIP) controls. Persisted manually via CrossPointSettings toJson/fromJson
    // (NOT SettingsList.h) so they aren't double-written; edited only from this submenu.
    CYCLE_TOOLTIP_BUTTONS,
    CYCLE_TOOLTIP_BEHAVIOR,
    // Page Translation mode (LINGUA_PAGE_TRANSLATION) control: which button pair scrolls/closes the
    // open overlay.
    CYCLE_PAGE_TRANSLATION_BUTTONS,
    // Interleaved mode (LINGUA_INTERLEAVED) control: gray level translated words are drawn at.
    CYCLE_TRANSLATION_COLOUR,
    // Colour of the secondary text in the two modes that give it its own place on the page:
    // Interlinear's annotation rows and Side by Side's translation column. THREE colour actions,
    // three independent fields — the Interleaved one above cannot be reused, it has no Black and
    // its stored value is that mode's own (see CrossPointSettings::LINGUA_SHADE).
    CYCLE_INTERLINEAR_COLOUR,
    CYCLE_INTERLINEAR_TOGGLE_LONG_PRESS,
    CYCLE_INTERLINEAR_TOGGLE_BUTTONS,
    CYCLE_SIDE_BY_SIDE_COLOUR,
    // Translated-text type size relative to the body text. THREE actions, one per owning mode, each
    // bound to that mode's own stored field — not one action over a shared field, which made the
    // tooltip row silently retune the Interleaved layout (and invalidate its section cache).
    CYCLE_INTERLEAVED_SIZE,
    CYCLE_TOOLTIP_SIZE,
    CYCLE_PAGE_TRANSLATION_SIZE,
    // Interlinear's annotation row size. A FOURTH size action rather than a reuse of the three
    // above: its stored field is an absolute point size (INTERLINEAR_ANNOTATION_SIZE), not a
    // relation to the body text, so it neither shares their value space nor their availability rule.
    CYCLE_INTERLINEAR_SIZE,
    // Word Lens (LINGUA_WORD_LENS). Its strip colour and size are the two Interlinear rows above;
    // these are its own (the hint language is chosen apart from the translator's target).
    CYCLE_WORD_LENS_LEVEL,
    PICK_WORD_LENS_HINT,
    MANAGE_WORD_LENS_PACKS,
    CYCLE_WORD_LENS_PAGE_BUTTONS,
    CYCLE_WORD_LENS_NAVIGATION,
    CYCLE_WORD_LENS_EMPTY_STRIPS,
    CYCLE_WORD_LENS_TOGGLE_LONG_PRESS,
  };

  explicit LinguaSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                                 int currentSpineIndex);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    Action action;
    StrId labelId;
    // Sub-setting of the row above it (in practice: of the currently selected display mode). Drawn
    // indented so the ownership is visible; otherwise an ordinary, selectable row.
    bool isChild = false;
  };

  std::shared_ptr<Epub> epub;
  int currentSpineIndex;
  std::vector<MenuItem> menuItems;
  // FreeInkUI row mirror of menuItems, rebuilt whenever menuItems changes. Labels and values are
  // borrowed const char* (I18N tables, LANGUAGES[], apiKeyMasked) — nothing here owns a string.
  std::vector<freeink::ui::ListItem> rowItems;
  // Backing store for the ENTER_API_KEY row's value: ListItem::value borrows, so the masked key
  // needs storage that outlives buildScreen().
  char apiKeyMasked[32]{};
  // Backing store for a grey shade row's "needs AA" notice; ListItem::value borrows. One buffer is
  // enough: the Interlinear and Side by Side colour rows belong to different modes, so the menu
  // never holds both at once.
  char shadeLabelText[64]{};
  OptionPopup optionPopup;
  // The Word Lens hint languages the open popup offers, after its "Automatic" row.
  struct HintCode {
    char code[8];
  };
  std::vector<HintCode> hintCodes;
  // Same press-to-close / release-to-swallow bridge as EpubReaderMenuActivity.
  bool popupClosing = false;

  // A translation the READER produced for this chapter (`<spine>.translated.html` exists). Drives
  // the "Translate" vs "Re-translate Chapter" label only: a plugin-translated book has no sidecar,
  // and the reader can still translate it (into another language, or with its own engine).
  bool chapterIsTranslated = false;
  // Whether this chapter has a translation a bilingual display mode could show, from EITHER source
  // (sidecar, or embedded in the chapter's own XHTML). Gates the display-mode switch.
  bool chapterHasTranslation = false;
  // Any chapter of this book with a reader-produced sidecar. Drives the "Re-translate Book" label
  // and the "Delete Translations" entry, both of which act on sidecars only -- deliberately NOT
  // widened to embedded translations, which live in the book file and cannot be deleted.
  bool bookHasAnyTranslation = false;

  // Toast overlay (shown when user tries to switch display mode without translation).
  bool showingToast = false;
  unsigned long toastShownAtMs = 0UL;
  unsigned long toastDurationMs = 0UL;
  const char* toastMessage = nullptr;

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup and toast run before any button or touch handling.
  bool handleCustomInput() override;
  // Back returns a cancelled result to the reader; Confirm activates the selection.
  bool handleButtons() override;
  // Header inside the safe area, so the battery indicator is not clipped.
  void drawChrome() override;

  void buildMenuItems();
  // Mirrors menuItems into rowItems and keeps the selection inside the (possibly shorter) list.
  // Call after every buildMenuItems().
  void syncRowItems();
  // Right-hand value for a row, or nullptr for the plain command rows that have no value column.
  const char* rowValue(int index);
  // Sub-settings of the currently selected display mode, appended directly under the Display Mode
  // row. Empty for modes that have none.
  void appendModeChildren();
  // Re-scans on-disk translation state and rebuilds menu items. Called from
  // onEnter() and from each child-activity result handler.
  void rebuildAfterReturn();
  void onActionSelected(Action a);
  // Advances one of the three per-mode size fields, or does nothing when the active family ships no
  // smaller face. Shared so the availability rule and the SPIFFS-write guard exist in ONE place.
  void cycleTranslationSize(uint8_t& storedSize);
  // Advances one of the LINGUA_SHADE colour fields. Shared so the "drawing only, never a
  // re-layout" contract exists in ONE place.
  void cycleLinguaShade(uint8_t& storedShade);

  // Dynamic right-hand value labels for the list rows.
  const char* displayModeLabel() const;
  const char* engineLabel() const;
  const char* targetLangLabel() const;
  const char* sourceLangLabel() const;
  const char* tooltipButtonsLabel() const;
  const char* overlayButtonsLabel(uint8_t storedButtons) const;
  const char* tooltipBehaviorLabel() const;
  const char* pageTranslationButtonsLabel() const;
  const char* translationColourLabel();
  // Appends the "needs AA" notice to a grey colour name when Text Anti-Aliasing is off. Shared by
  // every colour row so they cannot disagree about warning; renders into shadeLabelText.
  const char* withAntiAliasingNotice(const char* name);
  // Value label for either LINGUA_SHADE row: pass the mode's own stored shade. Takes the value
  // rather than reading a field so the two rows cannot diverge in how they present it. Not const:
  // with Anti-Aliasing off a grey level renders into shadeLabelText with the "needs AA" notice.
  const char* linguaShadeLabel(uint8_t storedShade);
  // Value label for any of the three Size rows: pass the mode's own stored TRANSLATION_SIZE.
  const char* translationSizeLabel(uint8_t storedSize) const;
  // Value label for Interlinear's Size row. Not translationSizeLabel(): different value space, and
  // the point sizes are unit-suffixed numerals rather than translated words.
  const char* interlinearSizeLabel() const;
  // Value label of the Gloss Pack row: whether the pack for this book's language is on the card.
  const char* wordLensPackLabel() const;
  // Writes a masked representation of the API key into `out`.
  void maskedApiKey(char* out, size_t outSize) const;

  void showToast(const char* msg, unsigned long durationMs);
};
