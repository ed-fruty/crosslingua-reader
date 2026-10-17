#include "modules/lingua/activities/LinguaSubmenuActivity.h"

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
#include "activities/ActivityResult.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/WrappedPopup.h"
#include "fontIds.h"
#include "modules/lingua/LinguaModeCatalog.h"
#include "modules/lingua/activities/LanguagePickerActivity.h"
#include "modules/lingua/engines/ParagraphTranslator.h"  // ParagraphTranslator::engineNeedsApiKey()
#include "modules/lingua/services/TranslatedContentDetector.h"
#include "modules/lingua/wordlens/WordLensLevels.h"
#include "modules/lingua/wordlens/WordLensService.h"

namespace fui = freeink::ui;

// Toast duration mirrors EpubReaderActivity's auto-fallback toast.
static constexpr unsigned long DEFAULT_TOAST_MS = ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS;

// Sentinel returned by LanguagePickerActivity for the "Auto-detect" entry and
// stored verbatim in SETTINGS.sourceTranslationLanguage / translationLanguage
// to mean "unset".
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

LinguaSubmenuActivity::LinguaSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::shared_ptr<Epub> epub, int currentSpineIndex)
    : UiListActivity("LinguaSubmenu", renderer, mappedInput),
      epub(std::move(epub)),
      currentSpineIndex(currentSpineIndex) {}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void LinguaSubmenuActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildAfterReturn();
}

void LinguaSubmenuActivity::onExit() { Activity::onExit(); }

// ─── state scan + menu build ──────────────────────────────────────────────────

void LinguaSubmenuActivity::rebuildAfterReturn() {
  // Build the translated-html path directly rather than instantiating Section,
  // which would require the parser deps and an extra HalFile handle. Match
  // Section::getTranslatedHtmlPath()'s format exactly so probes stay in sync.
  chapterIsTranslated = false;
  chapterHasTranslation = false;
  if (epub && currentSpineIndex >= 0) {
    const std::string path =
        epub->getCachePath() + "/sections/" + std::to_string(currentSpineIndex) + ".translated.html";
    chapterIsTranslated = Storage.exists(path.c_str());
    // Whether a bilingual display mode has anything to show is a broader question than whether the
    // READER produced the translation: a Calibre-plugin book embeds its translated paragraphs in the
    // chapter's own XHTML. Probing only the sidecar is what refused the author every display mode on
    // such a book. Same rule the layout engine renders by (lingua::content), so the mode this
    // unlocks is a mode that will actually render something.
    //
    // The chapter HTML is unzipped by the build that is showing the page this menu was opened over,
    // so the scan reads a local file and stops at the first translated block. One scan per menu
    // (re)build, never per frame.
    chapterHasTranslation =
        chapterIsTranslated ||
        lingua::content::htmlHasTranslatedBlock(
            epub->getCachePath() + "/html/" + std::to_string(currentSpineIndex) + ".html", epub->getLanguage());
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
  syncRowItems();
  requestUpdate();
}

void LinguaSubmenuActivity::buildMenuItems() {
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

void LinguaSubmenuActivity::appendModeChildren() {
  // Sub-settings of the SELECTED mode only, directly under the Display Mode row and indented, so it
  // reads as "these belong to that mode" — and so the menu is not cluttered with controls for six
  // modes the user is not in. buildMenuItems() re-runs on every mode cycle, so the block swaps
  // immediately.
  const auto child = [this](const Action action, const StrId label) {
    menuItems.push_back({action, label, /*isChild=*/true});
  };

  // Switch (no `default:`) over the mode enum rather than an if-chain: a mode added to
  // LINGUA_MODE then fails the build here until someone decides whether it has
  // sub-settings, instead of silently getting none. Same rationale as linguaLayoutForDisplayMode().
  switch (static_cast<CrossPointSettings::LINGUA_MODE>(SETTINGS.translationDisplayMode)) {
    case CrossPointSettings::LINGUA_INTERLEAVED:
      // Colour is Interleaved-only: it is the gray level the inline translated words are drawn at,
      // and no other mode draws translated text in the main flow.
      child(Action::CYCLE_TRANSLATION_COLOUR, StrId::STR_TRANSLATION_COLOUR);
      // Same label on all three Size rows, three DIFFERENT fields behind them: the row lives under
      // the mode it belongs to, so "Translation Size" already reads as that mode's size and needs no
      // per-mode wording (and so no new i18n keys).
      child(Action::CYCLE_INTERLEAVED_SIZE, StrId::STR_TRANSLATION_SIZE);
      return;
    case CrossPointSettings::LINGUA_TOOLTIP:
      child(Action::CYCLE_TOOLTIP_BUTTONS, StrId::STR_TOOLTIP_BUTTONS);
      child(Action::CYCLE_TOOLTIP_BEHAVIOR, StrId::STR_TOOLTIP_NAV);
      child(Action::CYCLE_TOOLTIP_SIZE, StrId::STR_TRANSLATION_SIZE);
      return;
    case CrossPointSettings::LINGUA_PAGE_TRANSLATION:
      child(Action::CYCLE_PAGE_TRANSLATION_BUTTONS, StrId::STR_PAGE_TRANSLATION_BUTTONS);
      child(Action::CYCLE_PAGE_TRANSLATION_SIZE, StrId::STR_TRANSLATION_SIZE);
      return;
    case CrossPointSettings::LINGUA_SIDE_BY_SIDE:
      // Colours the TRANSLATION COLUMN only, never the source: the two columns must stay
      // distinguishable as source and translation, which a symmetric shade would defeat. Its own
      // field, and its own row under its own mode -- nothing here is shared with Interleaved's
      // Colour above beyond the label.
      child(Action::CYCLE_SIDE_BY_SIDE_COLOUR, StrId::STR_TRANSLATION_COLOUR);
      return;
    case CrossPointSettings::LINGUA_INTERLINEAR:
      // Colours the ANNOTATION ROWS only; the reveal controls below are drawing/input-only. Size is
      // the one row here that is a LAYOUT input (see getInterlinearAnnotationFontId), so it sits
      // directly under Colour, matching the Colour-then-Size order of the other modes' blocks.
      child(Action::CYCLE_INTERLINEAR_COLOUR, StrId::STR_TRANSLATION_COLOUR);
      child(Action::CYCLE_INTERLINEAR_SIZE, StrId::STR_TRANSLATION_SIZE);
      child(Action::CYCLE_INTERLINEAR_TOGGLE_LONG_PRESS, StrId::STR_TOGGLE_BY_LONGPRESS);
      child(Action::CYCLE_INTERLINEAR_TOGGLE_BUTTONS, StrId::STR_TOGGLE_BUTTONS);
      return;
    case CrossPointSettings::LINGUA_WORD_LENS:
      // Pack and level first: they decide which words are glossed at all. The hint language lists
      // only languages with a pack for the book, and the book language row only exists for a book
      // that does not declare one. Colour and size are Interlinear's (the strip is an annotation row).
      child(Action::MANAGE_WORD_LENS_PACKS, StrId::STR_WORD_LENS_PACK);
      child(Action::CYCLE_WORD_LENS_LEVEL, StrId::STR_WORD_LENS_LEVEL);
      child(Action::PICK_WORD_LENS_HINT, StrId::STR_WORD_LENS_HINT_LANGUAGE);
      if (epub && epub->getLanguage().empty()) child(Action::PICK_SOURCE_LANG, StrId::STR_WORD_LENS_BOOK_LANGUAGE);
      child(Action::CYCLE_INTERLINEAR_COLOUR, StrId::STR_TRANSLATION_COLOUR);
      child(Action::CYCLE_INTERLINEAR_SIZE, StrId::STR_TRANSLATION_SIZE);
      child(Action::CYCLE_WORD_LENS_EMPTY_STRIPS, StrId::STR_WORD_LENS_EMPTY_STRIPS);
      child(Action::CYCLE_WORD_LENS_PAGE_BUTTONS, StrId::STR_WORD_LENS_PAGE_BUTTONS);
      child(Action::CYCLE_WORD_LENS_NAVIGATION, StrId::STR_WORD_LENS_AT_LAST_WORD);
      child(Action::CYCLE_WORD_LENS_TOGGLE_LONG_PRESS, StrId::STR_TOGGLE_BY_LONGPRESS);
      return;
    // No sub-settings. Normal is NOT "no translated text": it maps to LinguaLayout::Both exactly as
    // Interleaved does, so its pages are byte-identical and do carry the translation inline. What
    // makes it Normal is the gray level -- InterleavedMode::translatedWordInk() hands the renderer 0 for every
    // mode except Interleaved, so translated words are drawn in plain black, indistinguishable from
    // the source. Presenting the two languages as one undifferentiated flow is the whole point of the
    // mode, so neither a shade nor a size row belongs on it. Translation Only shows the translation
    // as the page's own primary text in the body font and colour by design (dimming it would dim the
    // whole chapter). The retired holes are migrated away at load and can never be the current mode.
    case CrossPointSettings::LINGUA_NORMAL:
    case CrossPointSettings::LINGUA_ORIGINAL_ONLY:
    case CrossPointSettings::LINGUA_TRANSLATION_ONLY:
    case CrossPointSettings::LINGUA_LEGACY_DIMMED:
    case CrossPointSettings::LINGUA_LEGACY_DIMMED_LIGHT:
      return;
  }
}

// ─── input ────────────────────────────────────────────────────────────────────

bool LinguaSubmenuActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    // Match EpubReaderMenuActivity: OptionPopup acts on the press edge, so
    // keep the trailing release away from the menu underneath.
    popupClosing = !optionPopup.isActive();
    return true;
  }
  if (popupClosing) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return true;
    }
    popupClosing = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return true;
    }
  }

  // Auto-dismiss the toast overlay once its duration elapses. Never consumes the pass: the toast is
  // an overlay on the list, not a modal.
  if (showingToast && (millis() - toastShownAtMs) >= toastDurationMs) {
    showingToast = false;
    requestUpdate();
  }
  return false;
}

bool LinguaSubmenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{};
    setResult(std::move(result));
    finish();
    return true;
  }
  return false;
}

void LinguaSubmenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  if (index < 0 || index >= static_cast<int>(menuItems.size())) return;
  // The activated row opens a popup or leaves this screen; a lingering flash would gray an
  // unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  onActionSelected(menuItems[index].action);
}

// ─── action dispatch ──────────────────────────────────────────────────────────

void LinguaSubmenuActivity::onActionSelected(Action a) {
  switch (a) {
    case Action::CYCLE_DISPLAY_MODE: {
      StrId modeLabels[LINGUA_SELECTABLE_MODE_COUNT];
      for (size_t i = 0; i < LINGUA_SELECTABLE_MODE_COUNT; i++) {
        modeLabels[i] = linguaModeLabel(LINGUA_SELECTABLE_MODES[i]);
      }
      optionPopup.show(StrId::STR_DISPLAY_MODE, modeLabels, static_cast<int>(LINGUA_SELECTABLE_MODE_COUNT),
                       static_cast<int>(linguaSelectableIndex(SETTINGS.translationDisplayMode)), [this](int idx) {
                         const uint8_t newMode = static_cast<uint8_t>(LINGUA_SELECTABLE_MODES[idx]);
                         // A bilingual mode with no translation would render an empty page. Word Lens
                         // glosses from a pack and needs no translation.
                         if (newMode != CrossPointSettings::LINGUA_NORMAL &&
                             newMode != CrossPointSettings::LINGUA_WORD_LENS && !chapterHasTranslation) {
                           showToast(tr(STR_NO_TRANSLATION_SWITCH_NORMAL), DEFAULT_TOAST_MS);
                           return;
                         }
                         if (SETTINGS.translationDisplayMode != newMode) {
                           SETTINGS.translationDisplayMode = newMode;
                           SETTINGS.saveToFile();
                           // Each mode owns a different block of child settings.
                           buildMenuItems();
                           syncRowItems();
                         }
                         requestUpdate();
                       });
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
      result.data = MenuResult{static_cast<int>(LinguaResult::TRANSLATE_CHAPTER)};
      setResult(std::move(result));
      finish();
      return;
    }

    case Action::TRANSLATE_BOOK: {
      if (!epub) return;
      ActivityResult result;
      result.data = MenuResult{static_cast<int>(LinguaResult::TRANSLATE_BOOK)};
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
      SETTINGS.translationDisplayMode = CrossPointSettings::LINGUA_NORMAL;
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
      static constexpr StrId engineLabels[] = {
          StrId::STR_ENGINE_GOOGLE_FREE, StrId::STR_ENGINE_DEEPL,       StrId::STR_ENGINE_DEEPL_PRO,
          StrId::STR_ENGINE_OPENAI,      StrId::STR_ENGINE_DEEPSEEK,    StrId::STR_ENGINE_GEMINI,
          StrId::STR_ENGINE_GOOGLE_V2,   StrId::STR_ENGINE_GOOGLE_HTML, StrId::STR_ENGINE_AZURE,
      };
      static_assert(sizeof(engineLabels) / sizeof(engineLabels[0]) == CrossPointSettings::TRANSLATION_ENGINE_COUNT);
      const int currentEngine = SETTINGS.translationEngine < CrossPointSettings::TRANSLATION_ENGINE_COUNT
                                    ? SETTINGS.translationEngine
                                    : CrossPointSettings::ENGINE_GOOGLE_V2;
      optionPopup.show(StrId::STR_TRANSLATION_ENGINE, engineLabels, CrossPointSettings::TRANSLATION_ENGINE_COUNT,
                       currentEngine, [this](int idx) {
                         if (SETTINGS.translationEngine != idx) {
                           SETTINGS.translationEngine = static_cast<uint8_t>(idx);
                           SETTINGS.saveToFile();
                           // Keyed engines expose the API-key row; keyless engines hide it.
                           buildMenuItems();
                           syncRowItems();
                         }
                         requestUpdate();
                       });
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

    // The two LINGUA_SHADE rows. Same contract as the Interleaved colour above -- drawing only --
    // but reached through the per-role ink (PageFontSet), which LinguaReaderIntegration rebuilds on
    // every draw, so the reader repaints in the new colour with no chapter rebuild. Deliberately
    // absent from the reader's re-layout gate; see EpubReaderActivity's LINGUA handler.
    case Action::CYCLE_INTERLINEAR_COLOUR:
      cycleLinguaShade(SETTINGS.interlinearAnnotationShade);
      return;

    case Action::CYCLE_INTERLINEAR_TOGGLE_LONG_PRESS:
      SETTINGS.interlinearToggleByLongPress = SETTINGS.interlinearToggleByLongPress ? 0 : 1;
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_INTERLINEAR_TOGGLE_BUTTONS:
      SETTINGS.interlinearToggleButtons =
          static_cast<uint8_t>((SETTINGS.interlinearToggleButtons + 1) % CrossPointSettings::OVERLAY_BUTTONS_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_SIDE_BY_SIDE_COLOUR:
      cycleLinguaShade(SETTINGS.sideBySideTranslationShade);
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

    // Its own cycle, not cycleTranslationSize(): the value space is INTERLINEAR_ANNOTATION_SIZE and
    // every option is always available, so there is no smaller-face guard to apply. Like the
    // Interleaved size this is a LAYOUT input — it reaches ReaderRenderSpec::annotationFontId — and
    // the reader's result handler re-lays the section out on the layoutEquals() comparison.
    case Action::CYCLE_INTERLINEAR_SIZE:
      SETTINGS.interlinearAnnotationSize = static_cast<uint8_t>((SETTINGS.interlinearAnnotationSize + 1) %
                                                                CrossPointSettings::INTERLINEAR_ANNOTATION_SIZE_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_WORD_LENS_LEVEL:
      // A LAYOUT input: the reader's result handler re-lays the chapter out on the layoutEquals()
      // comparison (the level is part of the Word Lens cache key).
      SETTINGS.wordLensLevel = static_cast<uint8_t>((SETTINGS.wordLensLevel + 1) % lingua::wordlens::LEVEL_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::PICK_WORD_LENS_HINT: {
      // Only languages with a pack for this book's language, so every choice can gloss it. A LAYOUT
      // input through the Word Lens cache key; the translator's own target is left alone.
      auto& wordLens = lingua::wordlens::WordLensService::instance();
      lingua::wordlens::WordLensService::LanguageCode targets[lingua::wordlens::WordLensService::MAX_TARGETS];
      const size_t count = lingua::wordlens::WordLensService::availableTargets(
          wordLens.wantedSource(), targets, lingua::wordlens::WordLensService::MAX_TARGETS);
      hintCodes.clear();
      hintCodes.reserve(count);
      for (size_t i = 0; i < count; i++) {
        HintCode h;
        std::memcpy(h.code, targets[i], sizeof(h.code));
        hintCodes.push_back(h);
      }
      std::vector<std::string> labels;
      labels.reserve(count + 1);
      labels.push_back(std::string(tr(STR_WORD_LENS_HINT_AUTO)));
      int current = 0;
      for (size_t i = 0; i < count; i++) {
        labels.push_back(lingua::wordlens::WordLensService::languageName(targets[i]));
        if (std::strcmp(targets[i], SETTINGS.wordLensHint) == 0) current = static_cast<int>(i + 1);
      }
      optionPopup.show(StrId::STR_WORD_LENS_HINT_LANGUAGE, labels, current, [this](const int idx) {
        const char* code = idx > 0 && idx <= static_cast<int>(hintCodes.size()) ? hintCodes[idx - 1].code : "";
        if (std::strcmp(SETTINGS.wordLensHint, code) != 0) {
          std::snprintf(SETTINGS.wordLensHint, sizeof(SETTINGS.wordLensHint), "%s", code);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
      requestUpdate();
      return;
    }

    case Action::MANAGE_WORD_LENS_PACKS: {
      if (!epub) return;
      // Like translation, the download runs after the reader has released the book (TLS heap).
      ActivityResult result;
      result.data = MenuResult{static_cast<int>(LinguaResult::MANAGE_WORD_LENS_PACKS)};
      setResult(std::move(result));
      finish();
      return;
    }

    case Action::CYCLE_WORD_LENS_PAGE_BUTTONS:
      SETTINGS.wordLensPageButtons =
          static_cast<uint8_t>((SETTINGS.wordLensPageButtons + 1) % CrossPointSettings::OVERLAY_BUTTONS_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_WORD_LENS_EMPTY_STRIPS:
      // A LAYOUT input like the level: part of the Word Lens cache key, so the chapter re-lays out.
      SETTINGS.wordLensEmptyStrips =
          static_cast<uint8_t>((SETTINGS.wordLensEmptyStrips + 1) % CrossPointSettings::WORD_LENS_EMPTY_STRIPS_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_WORD_LENS_TOGGLE_LONG_PRESS:
      SETTINGS.wordLensToggleByLongPress = SETTINGS.wordLensToggleByLongPress ? 0 : 1;
      SETTINGS.saveToFile();
      requestUpdate();
      return;

    case Action::CYCLE_WORD_LENS_NAVIGATION:
      SETTINGS.wordLensNavigation =
          static_cast<uint8_t>((SETTINGS.wordLensNavigation + 1) % CrossPointSettings::TOOLTIP_NAVIGATION_COUNT);
      SETTINGS.saveToFile();
      requestUpdate();
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

void LinguaSubmenuActivity::cycleTranslationSize(uint8_t& storedSize) {
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

void LinguaSubmenuActivity::cycleLinguaShade(uint8_t& storedShade) {
  // Black -> Grey -> Light Grey, matching the key order in english.yaml. DRAWING ONLY: the shade
  // reaches the page through PageFontSet's per-role ink, which LinguaReaderIntegration
  // rebuilds on every draw, so there is nothing to invalidate here -- no section reset, no
  // armReposition, no cache key. requestUpdate() repaints this row's value; the reader repaints in
  // the new colour on its next frame.
  storedShade = static_cast<uint8_t>((storedShade + 1) % CrossPointSettings::LINGUA_SHADE_COUNT);
  SETTINGS.saveToFile();
  requestUpdate();
}

// ─── value labels ─────────────────────────────────────────────────────────────

const char* LinguaSubmenuActivity::displayModeLabel() const {
  const uint8_t mode = SETTINGS.translationDisplayMode;
  if (mode >= CrossPointSettings::LINGUA_MODE_COUNT) return I18N.get(StrId::STR_PT_NORMAL);
  return I18N.get(linguaModeLabel(static_cast<CrossPointSettings::LINGUA_MODE>(mode)));
}

const char* LinguaSubmenuActivity::tooltipButtonsLabel() const { return overlayButtonsLabel(SETTINGS.tooltipButtons); }

const char* LinguaSubmenuActivity::overlayButtonsLabel(const uint8_t storedButtons) const {
  return I18N.get(storedButtons == CrossPointSettings::OVERLAY_BUTTONS_SIDE ? StrId::STR_SIDE_BUTTONS
                                                                            : StrId::STR_FRONT_BUTTONS);
}

const char* LinguaSubmenuActivity::tooltipBehaviorLabel() const {
  return I18N.get(SETTINGS.tooltipBehavior == CrossPointSettings::TOOLTIP_NAV_TURN_PAGE ? StrId::STR_PAGE_TURN
                                                                                        : StrId::STR_LOOP);
}

const char* LinguaSubmenuActivity::pageTranslationButtonsLabel() const {
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
// STR_PT_LIGHT stay: linguaModeLabel() still maps the two retired LINGUA_LEGACY_DIMMED* modes to them.
const char* LinguaSubmenuActivity::withAntiAliasingNotice(const char* name) {
  // A grey level is painted by the LSB/MSB plane passes, which the reader runs only when Text
  // Anti-Aliasing is on (EpubReaderActivity's needsTextGrayscale). With AA off the level is an exact
  // no-op -- not even a degradation to black -- so the row would otherwise read as a setting that
  // silently does nothing. The stored choice is deliberately left alone: turn AA on and it applies.
  //
  // ONE helper for every colour row in this menu, so Interleaved and the two per-mode LINGUA_SHADE
  // rows cannot drift apart on whether they warn. Black is the only level that needs no planes, and
  // its caller returns before reaching here.
  if (SETTINGS.textAntiAliasing) return name;
  std::snprintf(shadeLabelText, sizeof(shadeLabelText), "%s — %s", name, I18N.get(StrId::STR_NEEDS_ANTIALIASING));
  return shadeLabelText;
}

const char* LinguaSubmenuActivity::translationColourLabel() {
  // Interleaved offers only grey levels -- Dimmed and Dimmed Light -- so with AA off this row does
  // nothing whichever value is stored, and BOTH carry the notice.
  return withAntiAliasingNotice(I18N.get(SETTINGS.translationShade == CrossPointSettings::SHADE_DIMMED_LIGHT
                                             ? StrId::STR_SHADE_DIMMED_LIGHT
                                             : StrId::STR_SHADE_DIMMED));
}

// Same three standalone colour nouns, one of them new. STR_SHADE_BLACK follows the rule the two
// existing keys were written to (see the comment above translationColourLabel): a value-column
// entry names the colour outright, so it reads correctly beside every language's word for
// "colour" — no adjective agreement to get wrong.
const char* LinguaSubmenuActivity::linguaShadeLabel(const uint8_t storedShade) {
  const char* name = nullptr;
  switch (static_cast<CrossPointSettings::LINGUA_SHADE>(storedShade)) {
    case CrossPointSettings::LINGUA_GREY:
      name = I18N.get(StrId::STR_SHADE_DIMMED);
      break;
    case CrossPointSettings::LINGUA_GREY_LIGHT:
      name = I18N.get(StrId::STR_SHADE_DIMMED_LIGHT);
      break;
    case CrossPointSettings::LINGUA_BLACK:
    case CrossPointSettings::LINGUA_SHADE_COUNT:
      break;
  }
  // Black is also the out-of-range answer, and it is the one level that does not need the grayscale
  // passes -- so it never carries the notice.
  if (name == nullptr) return I18N.get(StrId::STR_SHADE_BLACK);
  return withAntiAliasingNotice(name);
}

const char* LinguaSubmenuActivity::translationSizeLabel(const uint8_t storedSize) const {
  // Report Same whenever no smaller face exists, whatever is stored: translationFontIdForSize() also
  // degrades to the body font there, so Same is what the reader actually does. The stored value is
  // left alone on purpose — switch back to a family that ships a smaller face and the user's
  // Smaller choice is still in effect, without this screen having spent an SPIFFS write to erase it.
  // Takes the value rather than reading a field, so the three per-mode rows cannot diverge in how
  // they present it.
  const bool smaller = storedSize == CrossPointSettings::SIZE_SMALLER && SETTINGS.smallerReaderFontId() != 0;
  return I18N.get(smaller ? StrId::STR_SIZE_SMALLER : StrId::STR_SIZE_SAME);
}

const char* LinguaSubmenuActivity::interlinearSizeLabel() const {
  // Asks the RESOLVER rather than re-deriving the rule: font id 0 is its "same as the body font"
  // answer, which covers both an explicit ANNOTATION_BODY and a target script the small face cannot
  // cover — and a degraded row must read as what the page is actually drawn in. The stored point size
  // is left alone, exactly as translationSizeLabel() leaves a dormant Smaller alone: pick a supported
  // target language and the choice is back, with no SPIFFS write spent erasing it. Sound here because
  // this row only exists while the mode IS Interlinear (see appendModeChildren), which is the
  // resolver's other reason to return 0.
  if (SETTINGS.getInterlinearAnnotationFontId() == 0) return I18N.get(StrId::STR_SIZE_SAME);
  // "pt" is deliberately not translated, for the reason TextSettingsActivity's size list gives: it is
  // the typographic unit symbol, written the same way in every language CrossPoint ships. String
  // literals, so there is nothing to format and nothing to store — ListItem::value borrows a pointer
  // and these live in flash for the life of the program.
  switch (static_cast<CrossPointSettings::INTERLINEAR_ANNOTATION_SIZE>(SETTINGS.interlinearAnnotationSize)) {
    case CrossPointSettings::ANNOTATION_10PT:
      return "10 pt";
    case CrossPointSettings::ANNOTATION_12PT:
      return "12 pt";
    case CrossPointSettings::ANNOTATION_8PT:
    case CrossPointSettings::INTERLINEAR_ANNOTATION_SIZE_COUNT:
      break;
  }
  return "8 pt";  // ANNOTATION_8PT, and the out-of-range answer
}

const char* LinguaSubmenuActivity::wordLensPackLabel() const {
  switch (lingua::wordlens::WordLensService::instance().state()) {
    case lingua::wordlens::WordLensService::State::Ready:
      return I18N.get(StrId::STR_INSTALLED);
    case lingua::wordlens::WordLensService::State::NoLanguage:
      return I18N.get(StrId::STR_WORD_LENS_NO_BOOK_LANGUAGE);
    case lingua::wordlens::WordLensService::State::OpenFailed:
      return I18N.get(StrId::STR_WORD_LENS_UNREADABLE);
    case lingua::wordlens::WordLensService::State::NotInstalled:
    case lingua::wordlens::WordLensService::State::Inactive:
      break;
  }
  return I18N.get(StrId::STR_WORD_LENS_NOT_INSTALLED);
}

const char* LinguaSubmenuActivity::engineLabel() const {
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

const char* LinguaSubmenuActivity::targetLangLabel() const {
  // 0xFF == unset (never picked).
  if (SETTINGS.translationLanguage == AUTO_DETECT_SENTINEL) return "(unset)";
  if (SETTINGS.translationLanguage >= LanguagePickerActivity::NUM_LANGUAGES) return "(unset)";
  return LanguagePickerActivity::LANGUAGES[SETTINGS.translationLanguage].name;
}

const char* LinguaSubmenuActivity::sourceLangLabel() const {
  // 0xFF == auto-detect for source language (this is the documented sentinel).
  if (SETTINGS.sourceTranslationLanguage == AUTO_DETECT_SENTINEL) return I18N.get(StrId::STR_AUTO_DETECT);
  if (SETTINGS.sourceTranslationLanguage >= LanguagePickerActivity::NUM_LANGUAGES)
    return I18N.get(StrId::STR_AUTO_DETECT);
  return LanguagePickerActivity::LANGUAGES[SETTINGS.sourceTranslationLanguage].name;
}

void LinguaSubmenuActivity::maskedApiKey(char* out, size_t outSize) const {
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

void LinguaSubmenuActivity::showToast(const char* msg, unsigned long durationMs) {
  toastMessage = msg;
  toastDurationMs = durationMs;
  toastShownAtMs = millis();
  showingToast = true;
  requestUpdate();
}

// ─── render ───────────────────────────────────────────────────────────────────

void LinguaSubmenuActivity::syncRowItems() {
  rowItems.assign(menuItems.size(), fui::ListItem{});
  bool headingPlaced = false;
  for (size_t i = 0; i < menuItems.size(); i++) {
    rowItems[i].label = I18N.get(menuItems[i].labelId);
    rowItems[i].actionValue = static_cast<int16_t>(i);
    // The sub-settings of the selected display mode are grouped under a heading naming that mode,
    // so it reads as "these belong to that mode". Replaces the drawing-offset indent the pre-FUI
    // list used: a bidi-reordered title cannot carry an indent of its own (an RTL label moves its
    // leading run to the visual right), and FreeInkUI rows have no per-row indent.
    if (menuItems[i].isChild && !headingPlaced) {
      rowItems[i].sectionHeading = displayModeLabel();
      headingPlaced = true;
    } else if (menuItems[i].action == Action::CYCLE_ENGINE) {
      // A section in this list is delimited by the NEXT heading, not by an end marker of its own
      // (fui::list draws a heading before a row and nothing after one), so the mode block needs a
      // heading AFTER it or its end is invisible and its rows read as part of the rest of the menu.
      // Engine is the anchor because it is the only row that is always present and always the first
      // one past the block — pinning the heading to a position instead would move it whenever the
      // engine's API-key row appears or the mode's child count changes.
      rowItems[i].sectionHeading = I18N.get(StrId::STR_TRANSLATION_SECTION);
    }
  }
  // Keep the cursor inside the (potentially shorter) list after a delete or a mode change.
  nav.selected = std::max(std::min(nav.selected.load(), static_cast<int>(menuItems.size()) - 1), 0);
}

const char* LinguaSubmenuActivity::rowValue(const int index) {
  switch (menuItems[index].action) {
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
    case Action::CYCLE_INTERLINEAR_COLOUR:
      return linguaShadeLabel(SETTINGS.interlinearAnnotationShade);
    case Action::CYCLE_INTERLINEAR_TOGGLE_LONG_PRESS:
      return I18N.get(SETTINGS.interlinearToggleByLongPress ? StrId::STR_ON : StrId::STR_OFF);
    case Action::CYCLE_INTERLINEAR_TOGGLE_BUTTONS:
      return overlayButtonsLabel(SETTINGS.interlinearToggleButtons);
    case Action::CYCLE_SIDE_BY_SIDE_COLOUR:
      return linguaShadeLabel(SETTINGS.sideBySideTranslationShade);
    case Action::CYCLE_INTERLEAVED_SIZE:
      return translationSizeLabel(SETTINGS.interleavedTranslationSize);
    case Action::CYCLE_TOOLTIP_SIZE:
      return translationSizeLabel(SETTINGS.tooltipTranslationSize);
    case Action::CYCLE_PAGE_TRANSLATION_SIZE:
      return translationSizeLabel(SETTINGS.pageTranslationSize);
    case Action::CYCLE_INTERLINEAR_SIZE:
      return interlinearSizeLabel();
    case Action::CYCLE_WORD_LENS_LEVEL:
      return lingua::wordlens::LEVEL_LABELS[lingua::wordlens::clampLevel(SETTINGS.wordLensLevel)];
    case Action::MANAGE_WORD_LENS_PACKS:
      return wordLensPackLabel();
    case Action::PICK_WORD_LENS_HINT:
      return lingua::wordlens::WordLensService::languageName(
          lingua::wordlens::WordLensService::instance().wantedTarget());
    case Action::CYCLE_WORD_LENS_PAGE_BUTTONS:
      return overlayButtonsLabel(SETTINGS.wordLensPageButtons);
    case Action::CYCLE_WORD_LENS_EMPTY_STRIPS:
      return I18N.get(SETTINGS.wordLensEmptyStrips == CrossPointSettings::WORD_LENS_HIDE_EMPTY_STRIPS
                          ? StrId::STR_WORD_LENS_HIDE
                          : StrId::STR_WORD_LENS_KEEP);
    case Action::CYCLE_WORD_LENS_TOGGLE_LONG_PRESS:
      return I18N.get(SETTINGS.wordLensToggleByLongPress ? StrId::STR_ON : StrId::STR_OFF);
    case Action::CYCLE_WORD_LENS_NAVIGATION:
      return I18N.get(SETTINGS.wordLensNavigation == CrossPointSettings::TOOLTIP_NAV_TURN_PAGE ? StrId::STR_PAGE_TURN
                                                                                               : StrId::STR_LOOP);
    case Action::PICK_TARGET_LANG:
      return targetLangLabel();
    case Action::PICK_SOURCE_LANG:
      return sourceLangLabel();
    case Action::ENTER_API_KEY:
      // Bullet-masked or "(none)". ListItem::value borrows, so it goes to the member buffer.
      maskedApiKey(apiKeyMasked, sizeof(apiKeyMasked));
      return apiKeyMasked;
    // Plain command rows: no value column. Listed explicitly, with no `default:`, for the same
    // reason onActionSelected() has none — an Action added without deciding what its value
    // column shows must fail the build here (-Werror=switch), not silently render blank.
    case Action::TRANSLATE_CHAPTER:
    case Action::TRANSLATE_BOOK:
    case Action::DELETE_TRANSLATIONS:
      return nullptr;
  }
  return nullptr;  // unreachable: every enumerator returns above
}

void LinguaSubmenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome() paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Labels and headings were set by syncRowItems(); only the value column reflects live state.
  for (size_t i = 0; i < rowItems.size(); i++) {
    rowItems[i].value = rowValue(static_cast<int>(i));
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void LinguaSubmenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_LINGUA));
}

void LinguaSubmenuActivity::render(RenderLock&& lock) {
  // Popup FIRST, and it OWNS the frame — the same early return every other OptionPopup host uses
  // (SettingsActivity, TextSettingsActivity, StatusBarSettingsActivity, EpubReaderMenuActivity...).
  // The popup paints over the retained framebuffer without clearing it, so moving the highlight is
  // ONE flush. Running the base render first instead flushed twice per press — the cleared list
  // WITHOUT the popup, then the same list WITH it — which on e-ink reads as the dialog blinking out
  // and back on every press.
  if (optionPopup.processRender(renderer, mappedInput)) return;

  // List, header and button hints, in the base's order (it also re-runs the build when wrapped
  // labels shrank the page).
  UiListActivity::render(std::move(lock));

  if (showingToast && toastMessage) {
    // Toasts here include STR_NO_TRANSLATION_SWITCH_NORMAL, which is long in many languages and
    // overflows drawPopup's single-line box; wrap it to the viewable area instead. Flushes itself.
    drawWrappedPopup(renderer, toastMessage);
  }
}
