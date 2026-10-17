#include "modules/lingua/modes/word_lens/WordLensReader.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/RenderLock.h"
#include "activities/reader/ReaderUtils.h"
#include "components/WrappedPopup.h"
#include "modules/lingua/modes/word_lens/WordLensNavigation.h"
#include "modules/lingua/wordlens/GlossClean.h"
#include "modules/lingua/wordlens/WordLensGlosser.h"
#include "modules/lingua/wordlens/WordLensService.h"
#include "util/Dictionary.h"

using lingua::wordlens::WordLensNavigator;
using lingua::wordlens::WordLensService;

namespace {

bool wordLensActive() { return SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_WORD_LENS; }

bool entryForWord(void* ctx, const char* word, const size_t length, uint16_t& rank, uint32_t& entryOffset) {
  auto* glosser = static_cast<lingua::wordlens::WordLensGlosser*>(ctx);
  lingua::wordlens::WordLensGlosser::Decision decision;
  if (!glosser->lookupEntry(word, length, decision)) return false;
  rank = decision.rank;
  entryOffset = decision.entryOffset;
  return true;
}

// Index builds can take seconds on a large dictionary: let the idle task feed the watchdog.
void indexBuildYield(void*) { vTaskDelay(1); }

int annotationFontId() {
  const int id = SETTINGS.getInterlinearAnnotationFontId();
  return id != 0 ? id : SETTINGS.getReaderFontId();
}

}  // namespace

WordLensReader::WordLensReader() = default;
WordLensReader::~WordLensReader() = default;

bool WordLensReader::ensureNavigator() {
  if (!nav_) {
    nav_ = makeUniqueNoThrow<WordLensNavigator>();
    if (!nav_) LOG_ERR("WLN", "OOM: navigator");
  }
  return nav_ != nullptr;
}

void WordLensReader::onPageChanged() {
  if (nav_) nav_->clear();
  collected_ = false;
  pendingStep_ = 0;
  popupText_.clear();
}

void WordLensReader::onReaderExit() {
  onPageChanged();
  stripsVisible_ = true;
  nav_.reset();
  definition_ = Definition{};
}

WordLensReader::Action WordLensReader::handleInput(MappedInputManager& input, const bool hasSection) {
  if (!wordLensActive() || !hasSection) {
    if (nav_ && !wordLensActive()) onReaderExit();
    return Action::None;
  }
  // The gloss popup is modal: any button dismisses it, nothing else happens on that press.
  if (!popupText_.empty()) {
    if (input.wasReleased(MappedInputManager::Button::Back) || input.wasReleased(MappedInputManager::Button::Confirm) ||
        input.wasReleased(MappedInputManager::Button::Left) || input.wasReleased(MappedInputManager::Button::Right) ||
        input.wasReleased(MappedInputManager::Button::PageBack) ||
        input.wasReleased(MappedInputManager::Button::PageForward)) {
      RenderLock lock;
      popupText_.clear();
      return Action::Render;
    }
    int x = 0;
    int y = 0;
    if (input.wasScreenTapped(x, y)) {
      RenderLock lock;
      popupText_.clear();
      return Action::Render;
    }
    return Action::Consumed;
  }

  // No pack open: no strips and nothing to step through, so the buttons keep their reader meaning.
  if (!WordLensService::instance().glosser()) return Action::None;

  // The pair that does not turn pages steps through the hard words.
  const bool pagesOnSide = SETTINGS.wordLensPageButtons == CrossPointSettings::OVERLAY_BUTTONS_SIDE;
  const bool swap = pagesOnSide && input.isNavDirectionSwapped();
  const auto prevButton = pagesOnSide ? (swap ? MappedInputManager::Button::Right : MappedInputManager::Button::Left)
                                      : MappedInputManager::Button::PageBack;
  const auto nextButton = pagesOnSide ? (swap ? MappedInputManager::Button::Left : MappedInputManager::Button::Right)
                                      : MappedInputManager::Button::PageForward;
  const bool loop = SETTINGS.wordLensNavigation == CrossPointSettings::TOOLTIP_NAV_LOOP;

  const bool next = input.wasReleased(nextButton);
  const bool prev = input.wasReleased(prevButton);
  if ((next || prev) && SETTINGS.wordLensToggleByLongPress && input.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
    // A long press of the stepping pair hides or shows the strips (Interlinear's gesture).
    RenderLock lock;
    stripsVisible_ = !stripsVisible_;
    return Action::Render;
  }
  if (next || prev) {
    // The render task collects and draws the targets; the loop task steps through them.
    RenderLock lock;
    if (!ensureNavigator()) return Action::Consumed;
    if (!collected_) {
      // The targets belong to the page on screen and are collected when it is drawn next.
      pendingStep_ = next ? 1 : -1;
      return Action::Render;
    }
    const auto step = next ? nav_->next(loop) : nav_->prev(loop);
    switch (step) {
      case WordLensNavigator::Step::Moved:
      case WordLensNavigator::Step::Wrapped:
        return Action::Render;
      case WordLensNavigator::Step::PastEnd:
        return Action::PageForward;
      case WordLensNavigator::Step::PastStart:
        return Action::PageBack;
      case WordLensNavigator::Step::NoTargets:
        // Nothing to step through on this page: with Page Turn the pair simply turns the page.
        return loop ? Action::Consumed : (next ? Action::PageForward : Action::PageBack);
    }
    return Action::Consumed;
  }
  if (input.isPressed(nextButton) || input.isPressed(prevButton)) return Action::Consumed;

  if (nav_ && nav_->hasFocus()) {
    if (input.wasReleased(MappedInputManager::Button::Confirm)) return confirm(input);
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      RenderLock lock;
      nav_->clearFocus();
      return Action::Render;
    }
    if (input.isPressed(MappedInputManager::Button::Confirm) || input.isPressed(MappedInputManager::Button::Back)) {
      return Action::Consumed;
    }
  }

  // Touch: a tap on a hard word or on its gloss focuses it and looks it up.
  int x = 0;
  int y = 0;
  if (input.hasTouch() && SETTINGS.touchReaderControls && input.wasScreenTapped(x, y)) {
    bool hit = false;
    {
      RenderLock lock;
      hit = ensureNavigator() && collected_ && nav_->focusAt(x - xOffset_, y - yOffset_, stripHeight_);
    }
    if (hit) return confirm(input);
  }
  return Action::None;
}

WordLensReader::Action WordLensReader::confirm(MappedInputManager& input) {
  (void)input;
  // Lemma (<= 255 bytes) and full gloss, read once per Confirm: heap rather than ~500 bytes of stack.
  constexpr size_t LEMMA_BYTES = 256;
  constexpr size_t GLOSS_BYTES = lingua::wordlens::GLOSS_MAX_BYTES + 1;
  auto scratch = makeUniqueNoThrow<char[]>(LEMMA_BYTES + GLOSS_BYTES);
  if (!scratch) {
    LOG_ERR("WLN", "OOM: lookup scratch");
    return Action::Consumed;
  }
  char* lemma = scratch.get();
  char* gloss = scratch.get() + LEMMA_BYTES;
  char surface[sizeof(lingua::wordlens::NavTarget::surface)];
  {
    // The pack file and the targets are shared with the render task.
    RenderLock lock;
    const lingua::wordlens::NavTarget* target = nav_ ? nav_->focused() : nullptr;
    auto* glosser = WordLensService::instance().glosser();
    if (!target || !glosser || !target->hasEntry) return Action::Consumed;
    std::memcpy(surface, target->surface, sizeof(surface));
    if (!glosser->readEntry(target->entryOffset, lemma, LEMMA_BYTES, gloss, GLOSS_BYTES)) {
      LOG_ERR("WLN", "Cannot read the focused word's entry");
      return Action::Consumed;
    }
  }
  const bool lemmaDiffers = std::strcmp(lemma, surface) != 0 && lemma[0] != '\0';

  bool surfaceFound = false;
  bool lemmaFound = false;
  bool dictionaryAvailable = false;
  bool htmlDefinition = false;
  std::string definition;
  std::string headword;
  if (SETTINGS.dictionaryName[0] != '\0') {
    auto dict = makeUniqueNoThrow<Dictionary>();
    if (dict && dict->open(SETTINGS.dictionaryName)) {
      bool indexed = true;
      if (dict->needsIndex()) {
        // The first lookup in a new dictionary indexes it, which takes a while: say so.
        if (renderer_) {
          RenderLock lock;
          drawWrappedPopup(*renderer_, tr(STR_DICT_INDEXING));
        }
        indexed = dict->buildIndex(&indexBuildYield, nullptr);
      }
      dictionaryAvailable = indexed;
      htmlDefinition = dict->definitionsAreHtml();
      if (indexed && surface[0] != '\0') surfaceFound = dict->lookup(surface, definition, headword);
      if (indexed && !surfaceFound && lemmaDiffers) lemmaFound = dict->lookup(lemma, definition, headword);
    }
  }

  switch (lingua::wordlens::decideLookup(dictionaryAvailable, surfaceFound, lemmaDiffers, lemmaFound)) {
    case lingua::wordlens::LookupAction::DictionarySurface:
    case lingua::wordlens::LookupAction::DictionaryLemma:
      definition_.headword = std::move(headword);
      definition_.text = std::move(definition);
      definition_.html = htmlDefinition;
      return Action::OpenDefinition;
    case lingua::wordlens::LookupAction::GlossPopup:
      break;
  }
  // drawTransientUi reads the popup text on the render task.
  RenderLock lock;
  popupText_.clear();
  popupText_.reserve(std::strlen(surface) + std::strlen(lemma) + std::strlen(gloss) + 8);
  popupText_ += surface;
  if (lemmaDiffers) {
    popupText_ += " (";
    popupText_ += lemma;
    popupText_ += ")";
  }
  popupText_ += ": ";
  popupText_ += gloss;
  return Action::Render;
}

void WordLensReader::renderOverlay(GfxRenderer& renderer, const Page& page, const int xOffset, const int yOffset) {
  if (!wordLensActive()) return;
  auto* glosser = WordLensService::instance().glosser();
  if (!glosser) return;
  renderer_ = &renderer;
  xOffset_ = xOffset;
  yOffset_ = yOffset;
  stripHeight_ = renderer.getLineHeight(annotationFontId());
  const int bodyFont = SETTINGS.getReaderFontId();

  const uint32_t signature = WordLensService::instance().cacheKey() ^ (page.visibleTextOffset * 2654435761u) ^
                             (static_cast<uint32_t>(page.elements.size()) << 20) ^ static_cast<uint32_t>(bodyFont);
  if (collected_ && signature != collectedSignature_) {
    // Re-laid out under the same page number: the old targets (and their pack offsets) are void.
    if (nav_) nav_->clear();
    collected_ = false;
  }
  // Every page is collected as it is drawn, so a press (or a tap) is resolved at once, and a page
  // without hard words hands the press straight to a page turn.
  if (!collected_) {
    if (!ensureNavigator()) return;
    nav_->collect(renderer, page, bodyFont, &entryForWord, glosser);
    collected_ = true;
    collectedSignature_ = signature;
    const bool loop = SETTINGS.wordLensNavigation == CrossPointSettings::TOOLTIP_NAV_LOOP;
    if (pendingStep_ > 0) nav_->next(loop);
    if (pendingStep_ < 0) nav_->prev(loop);
    pendingStep_ = 0;
  }

  const lingua::wordlens::NavTarget* t = nav_ ? nav_->focused() : nullptr;
  if (!t) return;
  const int underlineY = yOffset + t->y + renderer.getFontAscenderSize(bodyFont) + 3;
  renderer.fillRect(xOffset + t->x, underlineY, t->width, 2, true);
}

void WordLensReader::drawTransientUi(GfxRenderer& renderer) const {
  if (!popupText_.empty()) drawWrappedPopup(renderer, popupText_.c_str());
}
