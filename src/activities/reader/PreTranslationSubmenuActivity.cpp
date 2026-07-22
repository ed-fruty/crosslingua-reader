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
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/reader/BookTranslatorActivity.h"
#include "activities/reader/ChapterTranslatorActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/TranslatorReturnTarget.h"
#include "activities/translator/LanguagePickerActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

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

PreTranslationSubmenuActivity::PreTranslationSubmenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string epubPath)
    : Activity("PreTranslationSubmenu", renderer, mappedInput),
      currentSpineIndex(0),
      epubPath(std::move(epubPath)),
      pathMode(true) {}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void PreTranslationSubmenuActivity::onEnter() {
  Activity::onEnter();

  // Path mode has no reader-supplied Epub: load a lean, metadata-only one and derive
  // the resume spine index from saved progress before scanning translation state.
  if (pathMode) {
    LOG_DBG("MEM", "PTSUB onEnter (path): free=%u max=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap());
    if (!ensureEpubLoaded()) {
      epubLoadFailed = true;
      requestUpdate();
      return;
    }
    currentSpineIndex = loadSavedSpineIndex();
  }

  rebuildAfterReturn();
}

void PreTranslationSubmenuActivity::onExit() { Activity::onExit(); }

// ─── path-mode epub (re)loading ─────────────────────────────────────────────────

bool PreTranslationSubmenuActivity::ensureEpubLoaded() {
  if (epub) return true;
  LOG_DBG("PTSUB", "Loading lean epub (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS, and don't rebuild the cache if it is missing.
  if (!epub->load(false, true)) {
    // Diagnostic for "invalid book" reports where the cache was reportedly built
    // minutes earlier: load(false, true) fails only when BookMetadataCache::load()
    // can't open <cachePath>/book.bin (wrong hash / never built for this exact path
    // string) or the cached version doesn't match BOOK_CACHE_VERSION. Print every
    // fact needed to tell those apart from the next device log: the exact path we
    // hashed, whether the source file itself is even found at that path, the
    // computed (std::hash-based) cache directory, and whether book.bin exists
    // there. A path-string mismatch between whatever opened the book originally
    // and this path-mode reopen would show epubExists=1 but book.bin exists=0 at
    // a cachePath that looks "off".
    const bool epubExists = Storage.exists(epubPath.c_str());
    const std::string cachePath = epub->getCachePath();
    const bool bookBinExists = Storage.exists((cachePath + "/book.bin").c_str());
    LOG_ERR("PTSUB", "Failed to load epub: path=%s epubExists=%d cachePath=%s book.bin exists=%d", epubPath.c_str(),
            (int)epubExists, cachePath.c_str(), (int)bookBinExists);
    epub.reset();
    return false;
  }
  LOG_DBG("PTSUB", "Lean epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  return true;
}

int PreTranslationSubmenuActivity::loadSavedSpineIndex() {
  // Mirror EpubReaderActivity::onEnter()'s progress.bin decode: bytes [0..1] are the
  // little-endian spine index. 4- and 6-byte payloads are both valid (the 6-byte form
  // adds a cached page count we don't need here).
  int spineIndex = 0;
  HalFile f;
  if (Storage.openFileForRead("PTSUB", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    const int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      spineIndex = data[0] + (data[1] << 8);
    }
  }
  // Clamp to a real chapter: the saved index can be the end-of-book sentinel
  // (== spine count) or stale after the book changed.
  const int count = epub->getSpineItemsCount();
  if (count <= 0) return 0;
  if (spineIndex >= count) spineIndex = count - 1;
  if (spineIndex < 0) spineIndex = 0;
  LOG_DBG("PTSUB", "Path-mode resume spine index: %d / %d", spineIndex, count);
  return spineIndex;
}

void PreTranslationSubmenuActivity::launchTranslatorFromPath(Action a) {
  if (!epub) return;
  // replaceActivity() clears the whole stack (this submenu and its lean Epub included);
  // the translator reopens its own lean Epub from epubPath. No teardown is needed here
  // because no reader/Section is resident in the browser context.
  if (a == Action::TRANSLATE_BOOK) {
    activityManager.replaceActivity(std::make_unique<BookTranslatorActivity>(renderer, mappedInput, epubPath,
                                                                             TranslatorReturnTarget::FILE_BROWSER));
    return;
  }
  // TRANSLATE_CHAPTER: compute the same PODs EpubReaderActivity::launchTranslation does.
  const std::string translatedHtmlPath =
      epub->getCachePath() + "/sections/" + std::to_string(currentSpineIndex) + ".translated.html";
  const bool alreadyTranslated = Storage.exists(translatedHtmlPath.c_str());
  activityManager.replaceActivity(std::make_unique<ChapterTranslatorActivity>(
      renderer, mappedInput, epubPath, currentSpineIndex, translatedHtmlPath, alreadyTranslated,
      TranslatorReturnTarget::FILE_BROWSER));
}

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
  menuItems.reserve(8);

  menuItems.push_back({Action::CYCLE_DISPLAY_MODE, StrId::STR_DISPLAY_MODE});

  menuItems.push_back(
      {Action::TRANSLATE_CHAPTER, chapterIsTranslated ? StrId::STR_RETRANSLATE_CHAPTER : StrId::STR_TRANSLATE_CHAPTER});

  menuItems.push_back(
      {Action::TRANSLATE_BOOK, bookHasAnyTranslation ? StrId::STR_RETRANSLATE_BOOK : StrId::STR_TRANSLATE_BOOK});

  if (bookHasAnyTranslation) {
    menuItems.push_back({Action::DELETE_TRANSLATIONS, StrId::STR_DELETE_TRANSLATIONS});
  }

  menuItems.push_back({Action::PICK_TARGET_LANG, StrId::STR_TARGET_LANGUAGE});
  menuItems.push_back({Action::PICK_SOURCE_LANG, StrId::STR_SOURCE_LANGUAGE});
  menuItems.push_back({Action::CYCLE_ENGINE, StrId::STR_TRANSLATION_ENGINE});
  menuItems.push_back({Action::ENTER_API_KEY, StrId::STR_TRANSLATE_API_KEY});
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
      const uint8_t newMode =
          static_cast<uint8_t>((SETTINGS.translationDisplayMode + 1) % CrossPointSettings::PT_MODE_COUNT);
      // Switching to any translation-display mode while no translated.html exists for
      // the current chapter would render a blank page; reject the change with a
      // toast and keep the mode at Normal.
      if (newMode != CrossPointSettings::PT_NORMAL && !chapterIsTranslated) {
        showToast(tr(STR_NO_TRANSLATION_SWITCH_NORMAL), DEFAULT_TOAST_MS);
        return;
      }
      SETTINGS.translationDisplayMode = newMode;
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    }

    case Action::TRANSLATE_CHAPTER: {
      if (!epub) return;
      // Path mode: no reader exists, so launch the translator directly (it returns to
      // the file browser afterwards). The submenu's own lean Epub dies when
      // replaceActivity() clears the stack.
      if (pathMode) {
        launchTranslatorFromPath(a);
        return;
      }
      // Reader mode: hand the request back to the reader rather than launching the
      // translator here. The reader must release its Epub + Section (freeing ~65KB)
      // before the TLS handshake, and replaceActivity() clears the whole stack, so the
      // teardown has to run while the reader is the current activity.
      ActivityResult result;
      result.data = MenuResult{static_cast<int>(PreTranslationResult::TRANSLATE_CHAPTER)};
      setResult(std::move(result));
      finish();
      return;
    }

    case Action::TRANSLATE_BOOK: {
      if (!epub) return;
      if (pathMode) {
        launchTranslatorFromPath(a);
        return;
      }
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

    case Action::CYCLE_ENGINE:
      SETTINGS.translationEngine =
          static_cast<uint8_t>((SETTINGS.translationEngine + 1) % CrossPointSettings::TRANSLATION_ENGINE_COUNT);
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

// ─── value labels ─────────────────────────────────────────────────────────────

const char* PreTranslationSubmenuActivity::displayModeLabel() const {
  static const StrId labels[] = {
      StrId::STR_PT_NORMAL,           StrId::STR_PT_DARK,         StrId::STR_PT_LIGHT, StrId::STR_PT_ORIGINAL_ONLY,
      StrId::STR_PT_TRANSLATION_ONLY, StrId::STR_PT_SIDE_BY_SIDE, StrId::STR_PT_MODAL,
  };
  const uint8_t mode = SETTINGS.translationDisplayMode;
  if (mode >= sizeof(labels) / sizeof(labels[0])) return I18N.get(StrId::STR_PT_NORMAL);
  return I18N.get(labels[mode]);
}

const char* PreTranslationSubmenuActivity::engineLabel() const {
  static const StrId labels[] = {
      StrId::STR_ENGINE_GOOGLE_FREE, StrId::STR_ENGINE_DEEPL,       StrId::STR_ENGINE_DEEPL_PRO,
      StrId::STR_ENGINE_OPENAI,      StrId::STR_ENGINE_DEEPSEEK,    StrId::STR_ENGINE_GEMINI,
      StrId::STR_ENGINE_GOOGLE_V2,   StrId::STR_ENGINE_GOOGLE_HTML,
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

  // Path-mode load failure: show a message and let Back leave (handled in loop()).
  if (epubLoadFailed) {
    renderer.drawCenteredText(UI_12_FONT_ID, screen.y + screen.height / 2, tr(STR_INDEX_FAILED), true,
                              EpdFontFamily::BOLD);
    const auto errLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, errLabels.btn1, errLabels.btn2, errLabels.btn3, errLabels.btn4);
    renderer.displayBuffer();
    return;
  }

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
          case Action::TRANSLATE_CHAPTER:
          case Action::TRANSLATE_BOOK:
          case Action::DELETE_TRANSLATIONS:
          default:
            return "";
        }
      },
      /*highlightValue=*/true);

  // Button hints follow EpubReaderMenuActivity's pattern (Back / Select / Up / Down).
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showingToast && toastMessage) {
    GUI.drawPopup(renderer, toastMessage);
  }

  renderer.displayBuffer();
}
