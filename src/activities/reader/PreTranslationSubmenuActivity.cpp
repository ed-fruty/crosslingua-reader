#include "PreTranslationSubmenuActivity.h"

#include <Arduino.h>  // for millis()
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PreTranslationModes.h"
#include "activities/ActivityResult.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/translator/LanguagePickerActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "translator/ParagraphTranslator.h"  // ParagraphTranslator::engineNeedsApiKey()

// Toast duration mirrors EpubReaderActivity's auto-fallback toast.
static constexpr unsigned long DEFAULT_TOAST_MS = ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS;

// Sentinel returned by LanguagePickerActivity for the "Auto-detect" entry and
// stored verbatim in SETTINGS.sourceTranslationLanguage / translationLanguage
// to mean "unset".
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

PreTranslationSubmenuActivity::PreTranslationSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::shared_ptr<Epub> epub, int currentSpineIndex)
    : Activity("PreTranslationSubmenu", renderer, mappedInput),
      epub(std::move(epub)),
      currentSpineIndex(currentSpineIndex) {}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void PreTranslationSubmenuActivity::onEnter() {
  Activity::onEnter();
  rebuildAfterReturn();
}

void PreTranslationSubmenuActivity::onExit() { Activity::onExit(); }

// ─── state scan + menu build ──────────────────────────────────────────────────

void PreTranslationSubmenuActivity::rebuildAfterReturn() {
  // Build the translated-html path directly rather than instantiating Section,
  // which would require the parser deps and an extra HalFile handle. Match
  // Section::getTranslatedHtmlPath()'s format exactly so probes stay in sync.
  chapterIsTranslated = false;
  if (epub && currentSpineIndex >= 0) {
    const std::string path =
        epub->getCachePath() + "/sections/" + std::to_string(currentSpineIndex) + ".translated.html";
    chapterIsTranslated = Storage.exists(path.c_str());
  }

  bookHasAnyTranslation = false;
  if (epub) {
    const int count = epub->getSpineItemsCount();
    for (int i = 0; i < count; i++) {
      const std::string p = epub->getCachePath() + "/sections/" + std::to_string(i) + ".translated.html";
      if (Storage.exists(p.c_str())) {
        bookHasAnyTranslation = true;
        break;
      }
    }
  }

  buildMenuItems();
  // Keep the cursor inside the (potentially shorter) list after a delete.
  if (selectedIndex >= static_cast<int>(menuItems.size())) {
    selectedIndex = static_cast<int>(menuItems.size()) - 1;
  }
  if (selectedIndex < 0) {
    selectedIndex = 0;
  }
  requestUpdate();
}

void PreTranslationSubmenuActivity::buildMenuItems() {
  menuItems.clear();
  menuItems.reserve(10);

  menuItems.push_back({Action::CYCLE_DISPLAY_MODE, StrId::STR_DISPLAY_MODE});
  appendModeChildren();

  // Translation Engine follows the mode block; the API-key row is only meaningful for engines
  // that authenticate with a user key — keyless engines (the Google variants ship a built-in
  // key) hide it entirely. The menu is rebuilt on every engine change (see CYCLE_ENGINE) so the
  // row appears/disappears immediately.
  menuItems.push_back({Action::CYCLE_ENGINE, StrId::STR_TRANSLATION_ENGINE});
  if (ParagraphTranslator::engineNeedsApiKey(SETTINGS.translationEngine)) {
    menuItems.push_back({Action::ENTER_API_KEY, StrId::STR_TRANSLATE_API_KEY});
  }

  menuItems.push_back(
      {Action::TRANSLATE_CHAPTER, chapterIsTranslated ? StrId::STR_RETRANSLATE_CHAPTER : StrId::STR_TRANSLATE_CHAPTER});

  menuItems.push_back(
      {Action::TRANSLATE_BOOK, bookHasAnyTranslation ? StrId::STR_RETRANSLATE_BOOK : StrId::STR_TRANSLATE_BOOK});

  if (bookHasAnyTranslation) {
    menuItems.push_back({Action::DELETE_TRANSLATIONS, StrId::STR_DELETE_TRANSLATIONS});
  }

  // Target/Source Language rows are intentionally hidden: the target and source languages are
  // chosen inline in the Translate Chapter / Translate Book flow (LanguagePickerActivity), so
  // exposing them here too is redundant. The PICK_TARGET_LANG / PICK_SOURCE_LANG actions, their
  // handlers in onActionSelected(), and the *LangLabel() helpers are deliberately kept intact so
  // the rows can be resurrected by simply re-adding the two push_back()s below.
  //   menuItems.push_back({Action::PICK_TARGET_LANG, StrId::STR_TARGET_LANGUAGE});
  //   menuItems.push_back({Action::PICK_SOURCE_LANG, StrId::STR_SOURCE_LANGUAGE});
}

void PreTranslationSubmenuActivity::appendModeChildren() {
  // Sub-settings of the SELECTED mode only, directly under the Display Mode row and indented, so it
  // reads as "these belong to that mode" — and so the menu is not cluttered with controls for six
  // modes the user is not in. buildMenuItems() re-runs on every mode cycle, so the block swaps
  // immediately.
  const auto child = [this](const Action action, const StrId label) {
    menuItems.push_back({action, label, /*isChild=*/true});
  };

  // Switch (no `default:`) over the mode enum rather than an if-chain: a mode added to
  // PRE_TRANSLATION_MODE then fails the build here until someone decides whether it has
  // sub-settings, instead of silently getting none. Same rationale as ptLayoutForDisplayMode().
  switch (static_cast<CrossPointSettings::PRE_TRANSLATION_MODE>(SETTINGS.translationDisplayMode)) {
    case CrossPointSettings::PT_INTERLEAVED:
      // Colour is Interleaved-only: it is the gray level the inline translated words are drawn at,
      // and no other mode draws translated text in the main flow.
      child(Action::CYCLE_TRANSLATION_COLOUR, StrId::STR_TRANSLATION_COLOUR);
      // Same label on all three Size rows, three DIFFERENT fields behind them: the row lives under
      // the mode it belongs to, so "Translation Size" already reads as that mode's size and needs no
      // per-mode wording (and so no new i18n keys).
      child(Action::CYCLE_INTERLEAVED_SIZE, StrId::STR_TRANSLATION_SIZE);
      return;
    case CrossPointSettings::PT_TOOLTIP:
      child(Action::CYCLE_TOOLTIP_BUTTONS, StrId::STR_TOOLTIP_BUTTONS);
      child(Action::CYCLE_TOOLTIP_BEHAVIOR, StrId::STR_TOOLTIP_NAV);
      child(Action::CYCLE_TOOLTIP_SIZE, StrId::STR_TRANSLATION_SIZE);
      return;
    case CrossPointSettings::PT_PAGE_TRANSLATION:
      child(Action::CYCLE_PAGE_TRANSLATION_BUTTONS, StrId::STR_PAGE_TRANSLATION_BUTTONS);
      child(Action::CYCLE_PAGE_TRANSLATION_SIZE, StrId::STR_TRANSLATION_SIZE);
      return;
    // No sub-settings. Normal is NOT "no translated text": it maps to PtLayout::Both exactly as
    // Interleaved does, so its pages are byte-identical and do carry the translation inline. What
    // makes it Normal is the gray level -- modeToGray() (src/main.cpp) hands the renderer 0 for every
    // mode except Interleaved, so translated words are drawn in plain black, indistinguishable from
    // the source. Presenting the two languages as one undifferentiated flow is the whole point of the
    // mode, so neither a shade nor a size row belongs on it. Side by Side and Translation Only show
    // the translation in the body font and colour by design (a dimmed or shrunken column would defeat
    // them). Interlinear fixes its annotation face at the small UI size in v1 — the one place that
    // choice lives is CrossPointSettings::getInterlinearAnnotationFontId(), so a future Annotation
    // Size row plugs in there and gets a child() line here. The retired holes are migrated away at
    // load and can never be the current mode.
    case CrossPointSettings::PT_NORMAL:
    case CrossPointSettings::PT_ORIGINAL_ONLY:
    case CrossPointSettings::PT_TRANSLATION_ONLY:
    case CrossPointSettings::PT_SIDE_BY_SIDE:
    case CrossPointSettings::PT_INTERLINEAR:
    case CrossPointSettings::PT_LEGACY_DIMMED:
    case CrossPointSettings::PT_LEGACY_DIMMED_LIGHT:
      return;
  }
}

// ─── input ────────────────────────────────────────────────────────────────────

void PreTranslationSubmenuActivity::loop() {
  // Auto-dismiss the toast overlay once its duration elapses.
  if (showingToast && (millis() - toastShownAtMs) >= toastDurationMs) {
    showingToast = false;
    requestUpdate();
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(menuItems.size())) {
      onActionSelected(menuItems[selectedIndex].action);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{};
    setResult(std::move(result));
    finish();
    return;
  }
}

// ─── action dispatch ──────────────────────────────────────────────────────────

void PreTranslationSubmenuActivity::onActionSelected(Action a) {
  switch (a) {
    case Action::CYCLE_DISPLAY_MODE: {
      // Cycle through PT_SELECTABLE_MODES, not the raw value range: values 1 and 2 are retired
      // holes and must stay unreachable.
      const uint8_t newMode = ptNextSelectableMode(SETTINGS.translationDisplayMode);
      // Switching to any translation-display mode while no translated.html exists for
      // the current chapter would render a blank page; reject the change with a
      // toast and keep the mode at Normal.
      if (newMode != CrossPointSettings::PT_NORMAL && !chapterIsTranslated) {
        showToast(tr(STR_NO_TRANSLATION_SWITCH_NORMAL), DEFAULT_TOAST_MS);
        return;
      }
      SETTINGS.translationDisplayMode = newMode;
      SETTINGS.saveToFile();
      // Each mode carries its own child rows; rebuild so the indented block under Display Mode
      // swaps as the mode is cycled through. The cursor is on the Display Mode row (index 0), which
      // the rebuild preserves.
      buildMenuItems();
      if (selectedIndex >= static_cast<int>(menuItems.size())) {
        selectedIndex = static_cast<int>(menuItems.size()) - 1;
      }
      requestUpdate();
      return;
    }

    case Action::TRANSLATE_CHAPTER: {
      if (!epub) return;
      // Hand the request back to the reader rather than launching the translator
      // here: the reader must release its Epub + Section (freeing ~65KB) before
      // the TLS handshake, and replaceActivity() clears the whole stack, so the
      // teardown has to run while the reader is the current activity.
      ActivityResult result;
      result.data = MenuResult{static_cast<int>(PreTranslationResult::TRANSLATE_CHAPTER)};
      setResult(std::move(result));
      finish();
      return;
    }

    case Action::TRANSLATE_BOOK: {
      if (!epub) return;
      ActivityResult result;
      result.data = MenuResult{static_cast<int>(PreTranslationResult::TRANSLATE_BOOK)};
      setResult(std::move(result));
      finish();
      return;
    }

    case Action::DELETE_TRANSLATIONS: {
      if (!epub) return;
      // Wipe every translated.html plus every .bin so the next render is forced
      // through the layout pipeline against the original chapter HTML again.
      const int count = epub->getSpineItemsCount();
      for (int i = 0; i < count; i++) {
        const std::string translatedPath = epub->getCachePath() + "/sections/" + std::to_string(i) + ".translated.html";
        Storage.remove(translatedPath.c_str());
        const std::string binPath = epub->getCachePath() + "/sections/" + std::to_string(i) + ".bin";
        Storage.remove(binPath.c_str());
      }
      // Translations gone -> any non-Normal display mode would render blank pages.
      SETTINGS.translationDisplayMode = CrossPointSettings::PT_NORMAL;
      SETTINGS.saveToFile();
      LOG_INF("PTSUB", "Deleted all translated.html + section cache for book");
      rebuildAfterReturn();
      return;
    }

    case Action::PICK_TARGET_LANG: {
      const uint8_t initial = SETTINGS.translationLanguage == AUTO_DETECT_SENTINEL ? 0 : SETTINGS.translationLanguage;
      startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                      /*includeAutoDetect=*/false,
                                                                      /*initialSelection=*/initial,
                                                                      /*customTitle=*/tr(STR_TARGET_LANGUAGE)),
                             [this](const ActivityResult& res) {
                               if (!res.isCancelled) {
                                 const auto& mr = std::get<MenuResult>(res.data);
                                 SETTINGS.translationLanguage = static_cast<uint8_t>(mr.action);
                                 SETTINGS.saveToFile();
                               }
                               rebuildAfterReturn();
                             });
      return;
    }

    case Action::PICK_SOURCE_LANG: {
      const uint8_t initial = SETTINGS.sourceTranslationLanguage;
      startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                      /*includeAutoDetect=*/true,
                                                                      /*initialSelection=*/initial,
                                                                      /*customTitle=*/tr(STR_SOURCE_LANGUAGE)),
                             [this](const ActivityResult& res) {
                               if (!res.isCancelled) {
                                 const auto& mr = std::get<MenuResult>(res.data);
                                 SETTINGS.sourceTranslationLanguage = static_cast<uint8_t>(mr.action);
                                 SETTINGS.saveToFile();
                               }
                               rebuildAfterReturn();
                             });
      return;
    }

    case Action::CYCLE_ENGINE: {
      SETTINGS.translationEngine =
          static_cast<uint8_t>((SETTINGS.translationEngine + 1) % CrossPointSettings::TRANSLATION_ENGINE_COUNT);
      SETTINGS.saveToFile();
      // The API-key row is shown only for engines that need a key; rebuild so it appears/disappears
      // as the engine is cycled. The cursor is on the Engine row, which the rebuild preserves.
      buildMenuItems();
      if (selectedIndex >= static_cast<int>(menuItems.size())) {
        selectedIndex = static_cast<int>(menuItems.size()) - 1;
      }
      requestUpdate();
      return;
    }

    case Action::CYCLE_TOOLTIP_BUTTONS:
      SETTINGS.tooltipButtons =
          static_cast<uint8_t>((SETTINGS.tooltipButtons + 1) % CrossPointSettings::OVERLAY_BUTTONS_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_TOOLTIP_BEHAVIOR:
      SETTINGS.tooltipBehavior =
          static_cast<uint8_t>((SETTINGS.tooltipBehavior + 1) % CrossPointSettings::TOOLTIP_NAVIGATION_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_PAGE_TRANSLATION_BUTTONS:
      SETTINGS.pageTranslationButtons =
          static_cast<uint8_t>((SETTINGS.pageTranslationButtons + 1) % CrossPointSettings::OVERLAY_BUTTONS_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_TRANSLATION_COLOUR:
      // Drawing-only: main.cpp's loop() re-reads the shade into the renderer's translation gray
      // level every tick, so no section cache is invalidated and the reader shows it on its next
      // paint. Nothing to rebuild here beyond this row's value.
      SETTINGS.translationShade =
          static_cast<uint8_t>((SETTINGS.translationShade + 1) % CrossPointSettings::TRANSLATION_SHADE_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    // Three rows, three independent fields, one shared cycle rule. Only the Interleaved one changes
    // line breaking; the reader's result handler compares getInterleavedTranslationFontId() and so
    // re-lays out the section for that row alone, while the two overlay rows just repaint.
    case Action::CYCLE_INTERLEAVED_SIZE:
      cycleTranslationSize(SETTINGS.interleavedTranslationSize);
      return;

    case Action::CYCLE_TOOLTIP_SIZE:
      cycleTranslationSize(SETTINGS.tooltipTranslationSize);
      return;

    case Action::CYCLE_PAGE_TRANSLATION_SIZE:
      cycleTranslationSize(SETTINGS.pageTranslationSize);
      return;

    case Action::ENTER_API_KEY: {
      // Use the existing on-screen keyboard activity. Persist on confirm; ignore
      // on cancel. Password input mode masks the field while typing.
      std::string initial = SETTINGS.translateApiKey;
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string(tr(STR_TRANSLATE_API_KEY)),
                                                  std::move(initial), sizeof(SETTINGS.translateApiKey) - 1,
                                                  InputType::Password),
          [this](const ActivityResult& res) {
            if (!res.isCancelled) {
              const auto& kr = std::get<KeyboardResult>(res.data);
              std::strncpy(SETTINGS.translateApiKey, kr.text.c_str(), sizeof(SETTINGS.translateApiKey) - 1);
              SETTINGS.translateApiKey[sizeof(SETTINGS.translateApiKey) - 1] = '\0';
              SETTINGS.saveToFile();
            }
            rebuildAfterReturn();
          });
      return;
    }
  }
}

void PreTranslationSubmenuActivity::cycleTranslationSize(uint8_t& storedSize) {
  // Not cyclable when the active family has no smaller face (every SD family — SdCardFontSystem::
  // resolveFontId ignores its pointSize argument by design — and a built-in already at its smallest
  // point size): the row then reads Same permanently. Author's call — no "(n/a)" state, no toast, the
  // option simply is not there. Guarded before the write so a press on a locked row costs no SPIFFS
  // cycle. Availability is a property of the FAMILY, not of the mode, so all three rows share it.
  if (SETTINGS.smallerReaderFontId() == 0) return;
  storedSize = static_cast<uint8_t>((storedSize + 1) % CrossPointSettings::TRANSLATION_SIZE_COUNT);
  SETTINGS.saveToFile();
  requestUpdate();
}

// ─── value labels ─────────────────────────────────────────────────────────────

const char* PreTranslationSubmenuActivity::displayModeLabel() const {
  const uint8_t mode = SETTINGS.translationDisplayMode;
  if (mode >= CrossPointSettings::PT_MODE_COUNT) return I18N.get(StrId::STR_PT_NORMAL);
  return I18N.get(ptModeLabel(static_cast<CrossPointSettings::PRE_TRANSLATION_MODE>(mode)));
}

const char* PreTranslationSubmenuActivity::tooltipButtonsLabel() const {
  return I18N.get(SETTINGS.tooltipButtons == CrossPointSettings::OVERLAY_BUTTONS_SIDE ? StrId::STR_SIDE_BUTTONS
                                                                                      : StrId::STR_FRONT_BUTTONS);
}

const char* PreTranslationSubmenuActivity::tooltipBehaviorLabel() const {
  return I18N.get(SETTINGS.tooltipBehavior == CrossPointSettings::TOOLTIP_NAV_TURN_PAGE ? StrId::STR_PAGE_TURN
                                                                                        : StrId::STR_LOOP);
}

const char* PreTranslationSubmenuActivity::pageTranslationButtonsLabel() const {
  return I18N.get(SETTINGS.pageTranslationButtons == CrossPointSettings::OVERLAY_BUTTONS_SIDE
                      ? StrId::STR_SIDE_BUTTONS
                      : StrId::STR_FRONT_BUTTONS);
}

// Dedicated value keys, NOT the retired modes' STR_PT_DARK / STR_PT_LIGHT. Those two were written as
// MODE NAMES ("Dimmed" / "Dimmed Light"), and a mode name does not survive being moved into a value
// column: in inflected languages the adjective has to agree with that language's word for "colour"
// (Slovak "Farba" is feminine, so the masculine "Sivý" it inherited was simply wrong), and several
// languages had rendered the pair as an adverb phrase that is ungrammatical read as a colour
// ("Gedimmt hell", "Atténué clair"). The shade IS the renderer's gray level, so each language now
// names the colour outright — grey / light grey in its own standalone form. STR_PT_DARK /
// STR_PT_LIGHT stay: ptModeLabel() still maps the two retired PT_LEGACY_DIMMED* modes to them.
const char* PreTranslationSubmenuActivity::translationColourLabel() const {
  return I18N.get(SETTINGS.translationShade == CrossPointSettings::SHADE_DIMMED_LIGHT ? StrId::STR_SHADE_DIMMED_LIGHT
                                                                                      : StrId::STR_SHADE_DIMMED);
}

const char* PreTranslationSubmenuActivity::translationSizeLabel(const uint8_t storedSize) const {
  // Report Same whenever no smaller face exists, whatever is stored: translationFontIdForSize() also
  // degrades to the body font there, so Same is what the reader actually does. The stored value is
  // left alone on purpose — switch back to a family that ships a smaller face and the user's
  // Smaller choice is still in effect, without this screen having spent an SPIFFS write to erase it.
  // Takes the value rather than reading a field, so the three per-mode rows cannot diverge in how
  // they present it.
  const bool smaller = storedSize == CrossPointSettings::SIZE_SMALLER && SETTINGS.smallerReaderFontId() != 0;
  return I18N.get(smaller ? StrId::STR_SIZE_SMALLER : StrId::STR_SIZE_SAME);
}

const char* PreTranslationSubmenuActivity::engineLabel() const {
  // Positional: index == CrossPointSettings::TRANSLATION_ENGINE value. Append only.
  static const StrId labels[] = {
      StrId::STR_ENGINE_GOOGLE_FREE, StrId::STR_ENGINE_DEEPL,       StrId::STR_ENGINE_DEEPL_PRO,
      StrId::STR_ENGINE_OPENAI,      StrId::STR_ENGINE_DEEPSEEK,    StrId::STR_ENGINE_GEMINI,
      StrId::STR_ENGINE_GOOGLE_V2,   StrId::STR_ENGINE_GOOGLE_HTML, StrId::STR_ENGINE_AZURE,
  };
  const uint8_t eng = SETTINGS.translationEngine;
  if (eng >= sizeof(labels) / sizeof(labels[0])) return I18N.get(StrId::STR_ENGINE_GOOGLE_V2);
  return I18N.get(labels[eng]);
}

const char* PreTranslationSubmenuActivity::targetLangLabel() const {
  // 0xFF == unset (never picked).
  if (SETTINGS.translationLanguage == AUTO_DETECT_SENTINEL) return "(unset)";
  if (SETTINGS.translationLanguage >= LanguagePickerActivity::NUM_LANGUAGES) return "(unset)";
  return LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].name;
}

const char* PreTranslationSubmenuActivity::sourceLangLabel() const {
  // 0xFF == auto-detect for source language (this is the documented sentinel).
  if (SETTINGS.sourceTranslationLanguage == AUTO_DETECT_SENTINEL) return I18N.get(StrId::STR_AUTO_DETECT);
  if (SETTINGS.sourceTranslationLanguage >= LanguagePickerActivity::NUM_LANGUAGES)
    return I18N.get(StrId::STR_AUTO_DETECT);
  return LanguagePickerActivity::LANGUAGES[SETTINGS.sourceTranslationLanguage].name;
}

void PreTranslationSubmenuActivity::maskedApiKey(char* out, size_t outSize) const {
  if (outSize == 0) return;
  const size_t keyLen = std::strlen(SETTINGS.translateApiKey);
  if (keyLen == 0) {
    std::strncpy(out, "(none)", outSize - 1);
    out[outSize - 1] = '\0';
    return;
  }
  // Render at most 8 bullets regardless of true key length to avoid leaking
  // approximate key size to over-the-shoulder observers.
  // U+25CF BLACK CIRCLE is encoded as a 3-byte UTF-8 sequence (E2 97 8F).
  const size_t bullets = keyLen < 8 ? keyLen : 8;
  size_t pos = 0;
  for (size_t i = 0; i < bullets && pos + 3 < outSize; i++) {
    out[pos++] = static_cast<char>(0xE2);
    out[pos++] = static_cast<char>(0x97);
    out[pos++] = static_cast<char>(0x8F);
  }
  out[pos < outSize ? pos : outSize - 1] = '\0';
}

// ─── toast helper ─────────────────────────────────────────────────────────────

void PreTranslationSubmenuActivity::showToast(const char* msg, unsigned long durationMs) {
  toastMessage = msg;
  toastDurationMs = durationMs;
  toastShownAtMs = millis();
  showingToast = true;
  requestUpdate();
}

// ─── render ───────────────────────────────────────────────────────────────────

void PreTranslationSubmenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_PRE_TRANSLATION));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(menuItems.size()),
      selectedIndex, [this](int index) -> std::string { return I18N.get(menuItems[index].labelId); },
      /*rowSubtitle=*/nullptr,
      /*rowIcon=*/nullptr,
      [this](int index) -> std::string {
        const auto act = menuItems[index].action;
        switch (act) {
          case Action::CYCLE_DISPLAY_MODE:
            return displayModeLabel();
          case Action::CYCLE_ENGINE:
            return engineLabel();
          case Action::CYCLE_TOOLTIP_BUTTONS:
            return tooltipButtonsLabel();
          case Action::CYCLE_TOOLTIP_BEHAVIOR:
            return tooltipBehaviorLabel();
          case Action::CYCLE_PAGE_TRANSLATION_BUTTONS:
            return pageTranslationButtonsLabel();
          case Action::CYCLE_TRANSLATION_COLOUR:
            return translationColourLabel();
          case Action::CYCLE_INTERLEAVED_SIZE:
            return translationSizeLabel(SETTINGS.interleavedTranslationSize);
          case Action::CYCLE_TOOLTIP_SIZE:
            return translationSizeLabel(SETTINGS.tooltipTranslationSize);
          case Action::CYCLE_PAGE_TRANSLATION_SIZE:
            return translationSizeLabel(SETTINGS.pageTranslationSize);
          case Action::PICK_TARGET_LANG:
            return targetLangLabel();
          case Action::PICK_SOURCE_LANG:
            return sourceLangLabel();
          case Action::ENTER_API_KEY: {
            // Bullet-masked or "(none)"; bounded stack buffer keeps it cheap.
            char buf[32];
            maskedApiKey(buf, sizeof(buf));
            return std::string(buf);
          }
          // Plain command rows: no value column. Listed explicitly, with no `default:`, for the same
          // reason onActionSelected() has none — an Action added without deciding what its value
          // column shows must fail the build here (-Werror=switch), not silently render blank.
          case Action::TRANSLATE_CHAPTER:
          case Action::TRANSLATE_BOOK:
          case Action::DELETE_TRANSLATIONS:
            return "";
        }
        return "";  // unreachable: every enumerator returns above
      },
      /*highlightValue=*/true, /*rowDimmed=*/nullptr,
      // Sub-setting rows are indented by a DRAWING offset, not by leading spaces in the title: a
      // title goes through bidi before it is drawn, so for an Arabic or Hebrew label drawText
      // resolves an RTL paragraph and moves the leading run to the visual right -- the indent would
      // land on the wrong side, wedged against the right-aligned value column. See kListChildIndent.
      [this](int index) { return menuItems[index].isChild; });

  // Button hints follow EpubReaderMenuActivity's pattern (Back / Select / Up / Down).
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showingToast && toastMessage) {
    // Toasts here include STR_NO_TRANSLATION_SWITCH_NORMAL, which is long in many languages and
    // overflows GUI.drawPopup's single-line box; wrap it to the viewable area instead.
    GUI.drawWrappedPopup(renderer, toastMessage);
  }

  renderer.displayBuffer();
}
