#pragma once
#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;
class MappedInputManager;
class Page;

namespace lingua::wordlens {
class WordLensNavigator;
}

// The reader side of the Word Lens mode: stepping through the page's hard words with the button
// pair that does not turn pages, the focus underline, and Confirm -> dictionary definition or the
// full-gloss popup. Owned by LinguaReaderIntegration; idle (no allocation) outside the mode.
class WordLensReader {
 public:
  enum class Action : uint8_t { None, Consumed, Render, PageBack, PageForward, OpenDefinition };

  WordLensReader();
  ~WordLensReader();

  Action handleInput(MappedInputManager& input, bool hasSection);
  // Called right after the page is drawn: collects the page's hard words when navigation needs
  // them, applies a step that was waiting for them, and underlines the focused word.
  void renderOverlay(GfxRenderer& renderer, const Page& page, int xOffset, int yOffset);
  // The gloss popup, drawn over the finished frame.
  void drawTransientUi(GfxRenderer& renderer) const;
  bool popupActive() const { return !popupText_.empty(); }
  // The strips' long-press hide toggle (drawing only: hidden strips keep their space).
  bool stripsVisible() const { return stripsVisible_; }
  void onPageChanged();
  void onReaderExit();

  // Filled when handleInput returned OpenDefinition.
  struct Definition {
    std::string headword;
    std::string text;
    bool html = false;
  };
  Definition takeDefinition() { return std::move(definition_); }

 private:
  Action confirm(MappedInputManager& input);
  bool ensureNavigator();

  std::unique_ptr<lingua::wordlens::WordLensNavigator> nav_;
  // The page's targets are collected (they belong to the page last drawn).
  bool collected_ = false;
  // Which page they were collected from: the pack/level key and the page's own identity. A
  // re-layout (level, language, font) keeps the page index but not this, and drops the targets.
  uint32_t collectedSignature_ = 0;
  // A step requested before the targets existed: +1 next, -1 previous.
  int8_t pendingStep_ = 0;
  // The renderer of the last overlay pass, for the dictionary's "Indexing" popup.
  GfxRenderer* renderer_ = nullptr;
  int xOffset_ = 0;
  int yOffset_ = 0;
  int stripHeight_ = 0;
  bool stripsVisible_ = true;
  std::string popupText_;
  Definition definition_;
};
