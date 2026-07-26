#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
    // Font role of the LINE this word came from. A page can mix type sizes (a smaller translated
    // line under Interleaved), and this screen measures, boxes and redraws words itself instead of
    // going through PageLine::render, so it has to resolve the same role the page was drawn with or
    // the highlight would be measured and repainted in the wrong face.
    LineFontRole role = LineFontRole::Body;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  // The concrete font a word draws in, and the height of its line box — both role-resolved, so a
  // smaller translated line gets a smaller highlight instead of a body-sized one bleeding into the
  // line below.
  int wordFontId(const WordBox& word) const { return pageFonts.forRole(word.role); }
  int wordLineHeight(const WordBox& word) const { return renderer.getLineHeight(wordFontId(word)); }
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  // Per-word measurement and highlight drawing go through the word's own role (wordFontId); this
  // body id is only for the one genuinely page-wide job left, priming the SD-card font's advance
  // table. That is a conservative approximation for a page carrying a second size: Annotation rows are
  // filtered out of the word list entirely (extractWords), and an SD family's smaller-translation slot
  // resolves back to body (CrossPointSettings::smallerReaderFontId), so the only face this misses is
  // the Interlinear annotation one — whose words are never boxed here.
  int fontId = 0;
  PageFontSet pageFonts;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // The activity is entered while Confirm is still held (long-press trigger):
  // ignore the stale release until a fresh press is seen.
  bool confirmPressSeen = false;
};
