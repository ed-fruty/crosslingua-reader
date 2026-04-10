# Tooltip Translation Mode — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new CT_TOOLTIP translation mode that displays only original text, with on-demand sentence-level translation tooltips activated by button press.

**Architecture:** New `src/tooltip/` module contains all tooltip logic (SentenceSplitter + TooltipOverlay). Minimal integration points in existing files (~25 lines total). CT_TOOLTIP has its own parser block, cache key, and rendering path. Tooltip font is one system size smaller than page font.

**Tech Stack:** C++20, PlatformIO/Arduino, ESP32-C3, GfxRenderer drawing primitives

**Branch:** `feat/tooltip-translation` (off `fruty-custom`)

**Spec:** `docs/superpowers/specs/2026-04-10-tooltip-translation-design.md`

---

## File Map

**New files:**
| File | Responsibility |
|------|---------------|
| `src/tooltip/SentenceSplitter.h` | `SentenceSpan` struct, `splitSentences()`, `mapSentenceTranslations()` declarations |
| `src/tooltip/SentenceSplitter.cpp` | Sentence splitting state machine, proportional translation mapping |
| `src/tooltip/TooltipOverlay.h` | `TooltipOverlay` class: state, input handling, rendering declarations |
| `src/tooltip/TooltipOverlay.cpp` | State machine, button dispatch, tooltip positioning and drawing |

**Modified files:**
| File | What changes |
|------|-------------|
| `src/CrossPointSettings.h:186` | Add `CT_TOOLTIP = 6` to enum, `tooltipButtons` setting |
| `src/SettingsList.h:76-79` | Add CT_TOOLTIP label to translation mode list, add tooltipButtons setting |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:117-121` | Add `translationMode == 6` block |
| `src/main.cpp:364` | Extend colorTextGrayLevel mapping for CT_TOOLTIP |
| `src/activities/reader/EpubReaderActivity.h:5-12,31` | Include TooltipOverlay, add member + `onPageChanged()` helper |
| `src/activities/reader/EpubReaderActivity.cpp:265,374-400,932` | Input dispatch, page change reset, tooltip render call |
| `lib/I18n/translations/*.yaml` (8 files) | Add `STR_TOOLTIP`, `STR_TOOLTIP_BUTTONS`, `STR_FRONT_BUTTONS`, `STR_SIDE_BUTTONS` |

---

### Task 1: Create branch and add CT_TOOLTIP enum + settings

**Files:**
- Modify: `src/CrossPointSettings.h:186-187`
- Modify: `src/SettingsList.h:76-79`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:117-121`
- Modify: `src/main.cpp:364`

- [ ] **Step 1: Create feature branch**

```bash
git checkout fruty-custom
git checkout -b feat/tooltip-translation
```

- [ ] **Step 2: Add CT_TOOLTIP enum value and tooltipButtons setting to CrossPointSettings.h**

In `src/CrossPointSettings.h`, change line 186 from:

```cpp
  enum COLOR_TEXT_STYLE { CT_NORMAL = 0, CT_DARK = 1, CT_LIGHT = 2, CT_NO_RENDER = 3, CT_INVERT = 4, CT_SIDE_BY_SIDE = 5, COLOR_TEXT_STYLE_COUNT };
```

to:

```cpp
  enum COLOR_TEXT_STYLE { CT_NORMAL = 0, CT_DARK = 1, CT_LIGHT = 2, CT_NO_RENDER = 3, CT_INVERT = 4, CT_SIDE_BY_SIDE = 5, CT_TOOLTIP = 6, COLOR_TEXT_STYLE_COUNT };
```

Add after line 206 (`uint8_t sourceLanguage = 0xFF;`):

```cpp
  // Tooltip button assignment: 0=front buttons (Left/Right), 1=side buttons (PageBack/PageForward)
  uint8_t tooltipButtons = 0;
```

- [ ] **Step 3: Add parser block for CT_TOOLTIP in ChapterHtmlSlimParser.cpp**

In `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`, after line 121 (after the `// Side by side:` block closing brace, before `// flush the buffer`), add:

```cpp
  // Tooltip: keep both original and translated text as-is.
  // Translated words retain their grayLevel marker and will be hidden
  // at render time via colorTextGrayLevel=3, but stay in TextBlock for extraction.
  // (No transformation needed — wordColor passes through unchanged.)
```

This is a comment-only block since CT_TOOLTIP needs no transformation — the default path already preserves both texts with their original wordColor. The comment documents why there's no `if (translationMode == 6)` filter.

- [ ] **Step 4: Update colorTextGrayLevel mapping in main.cpp**

In `src/main.cpp`, change line 364 from:

```cpp
  renderer.setColorTextGrayLevel(SETTINGS.colorTextStyle <= 2 ? SETTINGS.colorTextStyle : (uint8_t)0);
```

to:

```cpp
  renderer.setColorTextGrayLevel(SETTINGS.colorTextStyle <= 2 ? SETTINGS.colorTextStyle
                                 : SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP ? (uint8_t)3
                                 : (uint8_t)0);
```

This sets grayLevel=3 (hidden) for CT_TOOLTIP, so translated words are not rendered but remain in the data.

- [ ] **Step 5: Add STR_TOOLTIP label to translation mode enum in SettingsList.h**

In `src/SettingsList.h`, change lines 76-79 from:

```cpp
      SettingInfo::Enum(StrId::STR_TRANSLATION_MODE, &CrossPointSettings::colorTextStyle,
                        {StrId::STR_NORMAL, StrId::STR_TRANSLATION_GREY, StrId::STR_TRANSLATION_LIGHT_GREY,
                         StrId::STR_NO_RENDER, StrId::STR_INVERT_TRANSLATION, StrId::STR_SIDE_BY_SIDE},
                        "colorTextStyle", StrId::STR_CAT_READER),
```

to:

```cpp
      SettingInfo::Enum(StrId::STR_TRANSLATION_MODE, &CrossPointSettings::colorTextStyle,
                        {StrId::STR_NORMAL, StrId::STR_TRANSLATION_GREY, StrId::STR_TRANSLATION_LIGHT_GREY,
                         StrId::STR_NO_RENDER, StrId::STR_INVERT_TRANSLATION, StrId::STR_SIDE_BY_SIDE,
                         StrId::STR_TOOLTIP},
                        "colorTextStyle", StrId::STR_CAT_READER),
```

Add the tooltipButtons setting after the colorTextStyle entry (before `// --- Controls ---`):

```cpp
      SettingInfo::Enum(StrId::STR_TOOLTIP_BUTTONS, &CrossPointSettings::tooltipButtons,
                        {StrId::STR_FRONT_BUTTONS, StrId::STR_SIDE_BUTTONS},
                        "tooltipButtons", StrId::STR_CAT_READER),
```

- [ ] **Step 6: Add I18n strings to all 8 YAML files**

Add to the end of each YAML file in `lib/I18n/translations/`:

**english.yaml:**
```yaml
STR_TOOLTIP: "Tooltip"
STR_TOOLTIP_BUTTONS: "Tooltip Buttons"
STR_FRONT_BUTTONS: "Front Buttons"
STR_SIDE_BUTTONS: "Side Buttons"
```

**czech.yaml:**
```yaml
STR_TOOLTIP: "Tooltip"
STR_TOOLTIP_BUTTONS: "Tlačítka pro tooltip"
STR_FRONT_BUTTONS: "Přední tlačítka"
STR_SIDE_BUTTONS: "Boční tlačítka"
```

**french.yaml:**
```yaml
STR_TOOLTIP: "Info-bulle"
STR_TOOLTIP_BUTTONS: "Boutons info-bulle"
STR_FRONT_BUTTONS: "Boutons avant"
STR_SIDE_BUTTONS: "Boutons latéraux"
```

**german.yaml:**
```yaml
STR_TOOLTIP: "Tooltip"
STR_TOOLTIP_BUTTONS: "Tooltip-Tasten"
STR_FRONT_BUTTONS: "Vordere Tasten"
STR_SIDE_BUTTONS: "Seitliche Tasten"
```

**portuguese.yaml:**
```yaml
STR_TOOLTIP: "Dica"
STR_TOOLTIP_BUTTONS: "Botões de dica"
STR_FRONT_BUTTONS: "Botões frontais"
STR_SIDE_BUTTONS: "Botões laterais"
```

**russia.yaml:**
```yaml
STR_TOOLTIP: "Подсказка"
STR_TOOLTIP_BUTTONS: "Кнопки подсказки"
STR_FRONT_BUTTONS: "Передние кнопки"
STR_SIDE_BUTTONS: "Боковые кнопки"
```

**spanish.yaml:**
```yaml
STR_TOOLTIP: "Tooltip"
STR_TOOLTIP_BUTTONS: "Botones de tooltip"
STR_FRONT_BUTTONS: "Botones frontales"
STR_SIDE_BUTTONS: "Botones laterales"
```

**swedish.yaml:**
```yaml
STR_TOOLTIP: "Tooltip"
STR_TOOLTIP_BUTTONS: "Tooltip-knappar"
STR_FRONT_BUTTONS: "Framknappar"
STR_SIDE_BUTTONS: "Sidoknappar"
```

- [ ] **Step 7: Regenerate I18n code**

Run:
```bash
python3 scripts/gen_i18n.py
```

Expected: Regenerates `lib/I18n/I18nKeys.h`, `I18nStrings.h`, and `I18nStrings.cpp` with new `STR_TOOLTIP`, `STR_TOOLTIP_BUTTONS`, `STR_FRONT_BUTTONS`, `STR_SIDE_BUTTONS` entries.

- [ ] **Step 8: Build to verify**

Run:
```bash
pio run 2>&1 | tail -20
```

Expected: Build succeeds. The new enum value and settings exist but are not yet used in any logic.

- [ ] **Step 9: Commit**

```bash
git add src/CrossPointSettings.h src/SettingsList.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp src/main.cpp lib/I18n/
git commit -m "feat: add CT_TOOLTIP enum, settings, parser passthrough, and i18n strings"
```

---

### Task 2: Add TextBlock accessor and implement SentenceSplitter

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h:35`
- Create: `src/tooltip/SentenceSplitter.h`
- Create: `src/tooltip/SentenceSplitter.cpp`

- [ ] **Step 1: Add getWordXpos() accessor to TextBlock.h**

In `lib/Epub/Epub/blocks/TextBlock.h`, after line 35 (`const std::list<EpdFontFamily::Style>& getWordStyles() const { return wordStyles; }`), add:

```cpp
  const std::list<uint16_t>& getWordXpos() const { return wordXpos; }
```

TooltipOverlay needs this to find word positions for sentence bounds and underline drawing.

- [ ] **Step 2: Create src/tooltip directory**

```bash
mkdir -p src/tooltip
```

- [ ] **Step 3: Create SentenceSplitter.h**

Create `src/tooltip/SentenceSplitter.h`:

```cpp
#pragma once

#include <cstdint>

// Maximum sentences per paragraph — fixed-size to avoid heap allocation.
static constexpr int MAX_SENTENCES = 50;

// Word index range within a paragraph's original words (grayLevel == 0).
struct SentenceSpan {
  uint16_t startWord;  // inclusive
  uint16_t endWord;    // exclusive
};

// Result of sentence splitting: array of spans + count.
struct SentenceSplitResult {
  SentenceSpan spans[MAX_SENTENCES];
  int count = 0;
};

// A mapped sentence: original word range + pointer to translated text.
// The translatedText buffer is owned by the caller (stack-allocated).
struct MappedSentence {
  SentenceSpan original;
  const char* translatedText;  // points into caller-owned buffer
};

struct MappedSentenceResult {
  MappedSentence sentences[MAX_SENTENCES];
  int count = 0;
};

// Split a sequence of words into sentences based on punctuation boundaries.
// words: array of C-strings (the original words from TextBlock, grayLevel==0 only)
// wordCount: number of words
// Returns SentenceSplitResult with word-index-based spans.
SentenceSplitResult splitSentences(const char* const* words, int wordCount);

// Map translated words to original sentences using proportional character-length mapping.
// originalWords/originalCount: original (grayLevel==0) words
// translatedWords/translatedCount: translated (grayLevel>0) words
// splits: result from splitSentences()
// outBuffer: caller-owned buffer where concatenated translation strings are written
// outBufferSize: size of outBuffer
// Returns MappedSentenceResult with original spans + translation text pointers.
MappedSentenceResult mapSentenceTranslations(const char* const* originalWords, int originalCount,
                                             const char* const* translatedWords, int translatedCount,
                                             const SentenceSplitResult& splits, char* outBuffer, int outBufferSize);
```

- [ ] **Step 4: Create SentenceSplitter.cpp**

Create `src/tooltip/SentenceSplitter.cpp`:

```cpp
#include "SentenceSplitter.h"

#include <cstring>

// Common abbreviations that end with '.' but are not sentence boundaries.
static const char* const ABBREVIATIONS[] = {
    "Mr.",  "Mrs.", "Ms.",  "Dr.",    "Prof.", "Sr.",   "Jr.",   "St.",
    "Inc.", "Ltd.", "Corp.","Gen.",   "Gov.",  "Sgt.",  "vs.",   "etc.",
    "Fig.", "Vol.", "Dept.","Univ.",  "approx.", nullptr
};

// i.e. and e.g. handled specially (two-letter prefix + period)
static const char* const DOT_ABBREVIATIONS[] = {"i.e.", "e.g.", nullptr};

static bool isClosingQuote(char c) {
  return c == '"' || c == '\'' || c == ')' || c == ']';
}

// Check if the word ending at the current position is an abbreviation.
// lastWord: the last word added (the one containing the period).
static bool isAbbreviation(const char* lastWord) {
  if (!lastWord || *lastWord == '\0') return false;
  for (int i = 0; ABBREVIATIONS[i]; i++) {
    // Check if lastWord ends with the abbreviation
    const size_t abbrLen = strlen(ABBREVIATIONS[i]);
    const size_t wordLen = strlen(lastWord);
    if (wordLen >= abbrLen && strcmp(lastWord + wordLen - abbrLen, ABBREVIATIONS[i]) == 0) {
      return true;
    }
  }
  for (int i = 0; DOT_ABBREVIATIONS[i]; i++) {
    const size_t abbrLen = strlen(DOT_ABBREVIATIONS[i]);
    const size_t wordLen = strlen(lastWord);
    if (wordLen >= abbrLen && strcmp(lastWord + wordLen - abbrLen, DOT_ABBREVIATIONS[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Check if the period at position `pos` in `word` is part of an ellipsis.
static bool isEllipsis(const char* word, int pos) {
  const int len = static_cast<int>(strlen(word));
  // Check for "..." pattern: at least 3 consecutive dots around pos
  int dotCount = 0;
  int start = pos;
  while (start > 0 && word[start - 1] == '.') start--;
  while (start + dotCount < len && word[start + dotCount] == '.') dotCount++;
  return dotCount >= 3;
}

// Check if a word ends with a sentence terminator (. ! ?)
// Returns the terminator character or '\0' if none.
static char getTerminator(const char* word) {
  if (!word || *word == '\0') return '\0';
  int len = static_cast<int>(strlen(word));
  // Skip trailing closing quotes/brackets
  while (len > 0 && isClosingQuote(word[len - 1])) len--;
  if (len <= 0) return '\0';
  char c = word[len - 1];
  if (c == '.' || c == '!' || c == '?') return c;
  return '\0';
}

SentenceSplitResult splitSentences(const char* const* words, int wordCount) {
  SentenceSplitResult result;
  if (wordCount == 0) return result;

  int sentenceStart = 0;

  for (int i = 0; i < wordCount; i++) {
    const char* word = words[i];
    char terminator = getTerminator(word);

    if (terminator == '\0') continue;

    // Skip ellipsis
    if (terminator == '.') {
      int len = static_cast<int>(strlen(word));
      // Find the position of the terminator period
      int pos = len - 1;
      while (pos > 0 && isClosingQuote(word[pos])) pos--;
      if (isEllipsis(word, pos)) continue;
      if (isAbbreviation(word)) continue;
    }

    // This word ends a sentence
    if (result.count < MAX_SENTENCES) {
      result.spans[result.count].startWord = sentenceStart;
      result.spans[result.count].endWord = i + 1;
      result.count++;
    }
    sentenceStart = i + 1;
  }

  // Remaining words form the last sentence (or there were no terminators)
  if (sentenceStart < wordCount && result.count < MAX_SENTENCES) {
    result.spans[result.count].startWord = sentenceStart;
    result.spans[result.count].endWord = wordCount;
    result.count++;
  }

  // If no sentences were split, treat the whole paragraph as one sentence
  if (result.count == 0 && wordCount > 0) {
    result.spans[0].startWord = 0;
    result.spans[0].endWord = wordCount;
    result.count = 1;
  }

  return result;
}

// Helper: total character length of words[start..end)
static int totalCharsInRange(const char* const* words, int start, int end) {
  int total = 0;
  for (int i = start; i < end; i++) {
    total += static_cast<int>(strlen(words[i]));
    if (i < end - 1) total++;  // space between words
  }
  return total;
}

MappedSentenceResult mapSentenceTranslations(const char* const* originalWords, int originalCount,
                                             const char* const* translatedWords, int translatedCount,
                                             const SentenceSplitResult& splits, char* outBuffer, int outBufferSize) {
  MappedSentenceResult result;
  if (splits.count == 0 || translatedCount == 0) return result;

  // Split translated words into sentences too
  SentenceSplitResult transSplits = splitSentences(translatedWords, translatedCount);

  // Total character counts for proportional mapping
  const int origTotalChars = totalCharsInRange(originalWords, 0, originalCount);
  const int transTotalChars = totalCharsInRange(translatedWords, 0, translatedCount);

  int bufPos = 0;
  int transWordIdx = 0;
  int transCharAccum = 0;
  int origCharAccum = 0;

  for (int i = 0; i < splits.count && i < MAX_SENTENCES; i++) {
    result.sentences[i].original = splits.spans[i];

    // Calculate what fraction of original text this sentence represents
    origCharAccum += totalCharsInRange(originalWords, splits.spans[i].startWord, splits.spans[i].endWord);
    const float targetFraction = origTotalChars > 0 ? static_cast<float>(origCharAccum) / origTotalChars : 1.0f;

    // Map proportional range of translated words
    const int transStart = transWordIdx;
    if (transSplits.count == splits.count) {
      // Perfect sentence count match — use 1:1 mapping
      transWordIdx = (i < transSplits.count) ? transSplits.spans[i].endWord : translatedCount;
    } else {
      // Proportional mapping
      while (transWordIdx < translatedCount) {
        transCharAccum += static_cast<int>(strlen(translatedWords[transWordIdx])) + 1;
        transWordIdx++;
        const float transFraction = transTotalChars > 0 ? static_cast<float>(transCharAccum) / transTotalChars : 1.0f;
        if (transFraction >= targetFraction - 0.01f || transWordIdx >= translatedCount) break;
      }
      // Last sentence gets all remaining
      if (i == splits.count - 1) {
        transWordIdx = translatedCount;
      }
    }

    // Write concatenated translation into outBuffer
    const int textStart = bufPos;
    for (int j = transStart; j < transWordIdx && bufPos < outBufferSize - 1; j++) {
      if (j > transStart && bufPos < outBufferSize - 1) {
        outBuffer[bufPos++] = ' ';
      }
      const int wlen = static_cast<int>(strlen(translatedWords[j]));
      const int copyLen = (bufPos + wlen < outBufferSize - 1) ? wlen : (outBufferSize - 1 - bufPos);
      memcpy(outBuffer + bufPos, translatedWords[j], copyLen);
      bufPos += copyLen;
    }
    outBuffer[bufPos++] = '\0';

    result.sentences[i].translatedText = outBuffer + textStart;
    result.count++;
  }

  return result;
}
```

- [ ] **Step 5: Build to verify new files compile**

Run:
```bash
pio run 2>&1 | tail -20
```

Expected: Build succeeds. SentenceSplitter compiles but is not yet called from anywhere.

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/blocks/TextBlock.h src/tooltip/SentenceSplitter.h src/tooltip/SentenceSplitter.cpp
git commit -m "feat: add TextBlock accessor and SentenceSplitter for sentence splitting and translation mapping"
```

---

### Task 3: Implement TooltipOverlay

**Files:**
- Create: `src/tooltip/TooltipOverlay.h`
- Create: `src/tooltip/TooltipOverlay.cpp`

- [ ] **Step 1: Create TooltipOverlay.h**

Create `src/tooltip/TooltipOverlay.h`:

```cpp
#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>
#include <Epub/Page.h>

#include "SentenceSplitter.h"

class TooltipOverlay {
 public:
  // Returns true if input was consumed (caller should skip normal page-turn handling).
  bool handleInput(MappedInputManager& input);

  // Render the tooltip overlay on top of the already-drawn page.
  // page: the currently displayed page (for accessing TextBlock data)
  // fontId: the reader font ID used for the page
  // tooltipFontId: the font ID for tooltip text (one size smaller)
  // xOffset, yOffset: page rendering offsets (margins)
  // viewportWidth, viewportHeight: available rendering area
  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight);

  // Reset tooltip state when page changes.
  void onPageChanged();

  // Check if tooltip is currently visible.
  bool isActive() const { return currentSentenceIndex >= 0; }

 private:
  int8_t currentSentenceIndex = -1;
  bool wrapAround = false;
  bool pagePrepared = false;

  // Cached sentence data for the current page.
  // Original word pointers and translated word pointers extracted from TextBlocks.
  static constexpr int MAX_WORDS = 500;
  const char* origWordPtrs[MAX_WORDS];
  int origWordCount = 0;
  const char* transWordPtrs[MAX_WORDS];
  int transWordCount = 0;

  // Sentence split result for the current page.
  SentenceSplitResult splits;

  // Prepare sentence data from the page's TextBlocks (called once per page).
  void preparePage(const Page& page);

  // Find the screen Y coordinate and X bounds for a sentence span.
  struct SentenceBounds {
    int firstLineY;
    int lineHeight;
    int startX;  // leftmost X of sentence on first line
    int endX;    // rightmost X of sentence on first line
  };
  SentenceBounds findSentenceBounds(const Page& page, const SentenceSpan& span, int fontId, int xOffset,
                                    int yOffset) const;

  // Draw underline under all lines of the active sentence.
  void drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span, int fontId,
                             int xOffset, int yOffset) const;
};
```

- [ ] **Step 2: Create TooltipOverlay.cpp**

Create `src/tooltip/TooltipOverlay.cpp`:

```cpp
#include "TooltipOverlay.h"

#include <CrossPointSettings.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

bool TooltipOverlay::handleInput(MappedInputManager& input) {
  // Determine which buttons control tooltip based on setting
  const bool useFrontButtons = (SETTINGS.tooltipButtons == 0);
  const auto nextBtn =
      useFrontButtons ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
  const auto backBtn =
      useFrontButtons ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;

  if (input.wasReleased(nextBtn)) {
    if (currentSentenceIndex < 0) {
      // IDLE → show first sentence (or restart after wrap-around)
      currentSentenceIndex = 0;
      wrapAround = false;
      return true;
    }
    if (currentSentenceIndex < splits.count - 1) {
      // Advance to next sentence
      currentSentenceIndex++;
      return true;
    }
    // At last sentence
    if (!wrapAround) {
      // First press past last: dismiss and set wrap-around flag
      currentSentenceIndex = -1;
      wrapAround = true;
      return true;
    }
    // Second press past last (wrap-around): restart from first
    currentSentenceIndex = 0;
    wrapAround = false;
    return true;
  }

  if (input.wasReleased(backBtn)) {
    if (currentSentenceIndex >= 0) {
      // Showing tooltip → dismiss
      currentSentenceIndex = -1;
      return true;
    }
    // IDLE → don't consume, let normal handling proceed
    return false;
  }

  return false;
}

void TooltipOverlay::onPageChanged() {
  currentSentenceIndex = -1;
  wrapAround = false;
  pagePrepared = false;
  origWordCount = 0;
  transWordCount = 0;
  splits.count = 0;
}

void TooltipOverlay::preparePage(const Page& page) {
  if (pagePrepared) return;
  pagePrepared = true;
  origWordCount = 0;
  transWordCount = 0;

  // Walk all PageLine elements and collect original/translated word pointers
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    for (; wIt != words.end() && sIt != styles.end(); ++wIt, ++sIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel == 0) {
        if (origWordCount < MAX_WORDS) {
          origWordPtrs[origWordCount++] = wIt->c_str();
        }
      } else {
        if (transWordCount < MAX_WORDS) {
          transWordPtrs[transWordCount++] = wIt->c_str();
        }
      }
    }
  }

  splits = splitSentences(origWordPtrs, origWordCount);
}

TooltipOverlay::SentenceBounds TooltipOverlay::findSentenceBounds(const Page& page, const SentenceSpan& span,
                                                                   int fontId, int xOffset, int yOffset) const {
  SentenceBounds bounds = {0, 0, 0, 0};
  int origWordIdx = 0;
  bool foundFirst = false;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();
    const auto& xpositions = block->getWordXpos();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    auto xIt = xpositions.begin();
    for (; wIt != words.end() && sIt != styles.end() && xIt != xpositions.end(); ++wIt, ++sIt, ++xIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel > 0) continue;  // skip translated words

      if (origWordIdx >= span.startWord && origWordIdx < span.endWord) {
        const int wordX = *xIt + line->xPos + xOffset;
        const int wordY = line->yPos + yOffset;

        if (!foundFirst) {
          bounds.firstLineY = wordY;
          bounds.startX = wordX;
          bounds.endX = wordX;
          foundFirst = true;
        }

        // Track X bounds on the first line only
        if (wordY == bounds.firstLineY) {
          if (wordX < bounds.startX) bounds.startX = wordX;
          const int wordEndX = wordX;  // approximate; exact width would need getTextWidth
          if (wordEndX > bounds.endX) bounds.endX = wordEndX;
        }
      }
      origWordIdx++;
    }
  }

  return bounds;
}

void TooltipOverlay::drawSentenceUnderline(GfxRenderer& renderer, const Page& page, const SentenceSpan& span,
                                           int fontId, int xOffset, int yOffset) const {
  int origWordIdx = 0;
  int currentLineY = -1;
  int lineStartX = 0;
  int lineEndX = 0;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int underlineY_offset = renderer.getFontAscenderSize(fontId) + 2;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(el.get());
    const auto& block = line->getTextBlock();
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();
    const auto& xpositions = block->getWordXpos();

    auto wIt = words.begin();
    auto sIt = styles.begin();
    auto xIt = xpositions.begin();
    for (; wIt != words.end() && sIt != styles.end() && xIt != xpositions.end(); ++wIt, ++sIt, ++xIt) {
      const uint8_t grayLevel = (static_cast<uint8_t>(*sIt) >> 5) & 0x3;
      if (grayLevel > 0) continue;

      if (origWordIdx >= span.startWord && origWordIdx < span.endWord) {
        const int wordX = *xIt + line->xPos + xOffset;
        const int wordY = line->yPos + yOffset;
        const int wordWidth = renderer.getTextWidth(fontId, wIt->c_str(),
                                                    static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(*sIt) & 0x1F));

        if (wordY != currentLineY) {
          // Draw underline for previous line segment if any
          if (currentLineY >= 0) {
            renderer.drawLine(lineStartX, currentLineY + underlineY_offset, lineEndX,
                              currentLineY + underlineY_offset, true);
          }
          currentLineY = wordY;
          lineStartX = wordX;
          lineEndX = wordX + wordWidth;
        } else {
          lineEndX = wordX + wordWidth;
        }
      }
      origWordIdx++;
    }
  }

  // Draw underline for last line segment
  if (currentLineY >= 0) {
    renderer.drawLine(lineStartX, currentLineY + underlineY_offset, lineEndX, currentLineY + underlineY_offset, true);
  }
}

void TooltipOverlay::render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset,
                            int yOffset, int viewportWidth, int viewportHeight) {
  if (currentSentenceIndex < 0) return;

  preparePage(page);

  if (currentSentenceIndex >= splits.count) {
    currentSentenceIndex = -1;
    return;
  }

  const SentenceSpan& span = splits.spans[currentSentenceIndex];

  // Map translations
  char translationBuffer[512];
  MappedSentenceResult mapped = mapSentenceTranslations(origWordPtrs, origWordCount, transWordPtrs, transWordCount,
                                                        splits, translationBuffer, sizeof(translationBuffer));

  const char* tooltipText = "";
  if (currentSentenceIndex < mapped.count) {
    tooltipText = mapped.sentences[currentSentenceIndex].translatedText;
  }

  if (tooltipText[0] == '\0') return;  // no translation available

  // Find sentence position on screen
  const int lineHeight = renderer.getLineHeight(fontId);
  SentenceBounds bounds = findSentenceBounds(page, span, fontId, xOffset, yOffset);
  if (bounds.firstLineY == 0 && bounds.startX == 0) return;  // sentence not found

  // Calculate tooltip dimensions
  constexpr int PADDING = 6;
  constexpr int CORNER_RADIUS = 3;
  constexpr int GAP = 4;
  const int maxTooltipWidth = viewportWidth - 2 * PADDING;

  // Word-wrap: measure text and split into lines if needed
  const int textWidth = renderer.getTextWidth(tooltipFontId, tooltipText);
  const int tooltipLineHeight = renderer.getLineHeight(tooltipFontId);

  int tooltipWidth;
  int tooltipHeight;
  int numLines = 1;

  if (textWidth <= maxTooltipWidth - 2 * PADDING) {
    // Single line
    tooltipWidth = textWidth + 2 * PADDING;
    tooltipHeight = tooltipLineHeight + 2 * PADDING;
  } else {
    // Multi-line: estimate number of lines
    tooltipWidth = maxTooltipWidth;
    const int availableTextWidth = tooltipWidth - 2 * PADDING;
    numLines = (textWidth + availableTextWidth - 1) / availableTextWidth;
    const int maxLines = (viewportHeight * 4 / 10) / tooltipLineHeight;  // 40% of viewport
    if (numLines > maxLines) numLines = maxLines;
    tooltipHeight = numLines * tooltipLineHeight + 2 * PADDING;
  }

  // Position: above or below the sentence
  const int spaceAbove = bounds.firstLineY - yOffset;
  int tooltipX = xOffset + PADDING;
  int tooltipY;

  if (tooltipHeight + GAP <= spaceAbove) {
    // Place above
    tooltipY = bounds.firstLineY - GAP - tooltipHeight;
  } else {
    // Place below
    tooltipY = bounds.firstLineY + lineHeight + GAP;
  }

  // Clamp to viewport
  if (tooltipY < yOffset + PADDING) tooltipY = yOffset + PADDING;
  if (tooltipY + tooltipHeight > yOffset + viewportHeight - PADDING) {
    tooltipY = yOffset + viewportHeight - PADDING - tooltipHeight;
  }

  // Draw tooltip background (white fill to erase text underneath)
  renderer.fillRect(tooltipX - 1, tooltipY - 1, tooltipWidth + 2, tooltipHeight + 2, false);

  // Draw border
  renderer.drawRoundedRect(tooltipX, tooltipY, tooltipWidth, tooltipHeight, 1, CORNER_RADIUS, true);

  // Draw tooltip text (word-wrapped)
  const int textX = tooltipX + PADDING;
  int textY = tooltipY + PADDING;
  const int availWidth = tooltipWidth - 2 * PADDING;
  const int spaceW = renderer.getSpaceWidth(tooltipFontId);

  // Simple word-wrap renderer
  const char* p = tooltipText;
  int lineCount = 0;
  while (*p && lineCount < numLines) {
    // Find how many words fit on this line
    int lineWidth = 0;
    const char* lineStart = p;
    const char* lastWordEnd = p;

    while (*p) {
      // Find next word
      const char* wordStart = p;
      while (*p && *p != ' ') p++;
      const int wLen = static_cast<int>(p - wordStart);

      // Measure this word
      char wordBuf[128];
      const int copyLen = (wLen < 127) ? wLen : 127;
      memcpy(wordBuf, wordStart, copyLen);
      wordBuf[copyLen] = '\0';
      const int wordWidth = renderer.getTextWidth(tooltipFontId, wordBuf);

      if (lineWidth > 0 && lineWidth + spaceW + wordWidth > availWidth) {
        // Word doesn't fit — draw what we have and start new line
        p = wordStart;
        break;
      }

      lineWidth += (lineWidth > 0 ? spaceW : 0) + wordWidth;
      lastWordEnd = p;

      // Skip space
      while (*p == ' ') p++;
    }

    // Draw this line
    const int drawLen = static_cast<int>(lastWordEnd - lineStart);
    if (drawLen > 0) {
      char lineBuf[256];
      const int cLen = (drawLen < 255) ? drawLen : 255;
      memcpy(lineBuf, lineStart, cLen);
      lineBuf[cLen] = '\0';
      renderer.drawText(tooltipFontId, textX, textY, lineBuf);
      textY += tooltipLineHeight;
      lineCount++;
    }

    if (p == lineStart) break;  // safety: no progress
  }

  // Draw underline under the active sentence
  drawSentenceUnderline(renderer, page, span, fontId, xOffset, yOffset);
}
```

- [ ] **Step 3: Build to verify**

Run:
```bash
pio run 2>&1 | tail -20
```

Expected: Build succeeds. TooltipOverlay compiles but is not yet instantiated.

- [ ] **Step 4: Commit**

```bash
git add src/tooltip/TooltipOverlay.h src/tooltip/TooltipOverlay.cpp
git commit -m "feat: add TooltipOverlay with state machine, positioning, and rendering"
```

---

### Task 4: Integrate TooltipOverlay into EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h:2-9,12,31`
- Modify: `src/activities/reader/EpubReaderActivity.cpp:265,311-323,374-400,925-932`

- [ ] **Step 1: Add TooltipOverlay member to EpubReaderActivity.h**

In `src/activities/reader/EpubReaderActivity.h`, add include after line 8:

```cpp
#include "tooltip/TooltipOverlay.h"
```

Add member after line 12 (`std::unique_ptr<Section> section = nullptr;`):

```cpp
  std::unique_ptr<TooltipOverlay> tooltipOverlay;
```

- [ ] **Step 2: Add tooltip input handling in loop()**

In `src/activities/reader/EpubReaderActivity.cpp`, after line 265 (closing brace of `skipNextButtonCheck` block), before line 267 (`// Confirm button: short press opens menu`), add:

```cpp
  // Tooltip mode: let overlay consume front/side button presses for sentence stepping
  if (tooltipOverlay && tooltipOverlay->handleInput(mappedInput)) {
    requestUpdate();
    return;
  }
```

- [ ] **Step 3: Split page navigation to respect tooltip button assignment**

In `src/activities/reader/EpubReaderActivity.cpp`, the existing page navigation (lines 311-323) combines both front and side buttons for prev/next. When CT_TOOLTIP is active, the tooltip buttons should NOT trigger page turns.

Replace lines 311-323 (the `prevTriggered`/`nextTriggered` calculation) with:

```cpp
  // When long-press behavior is None, turn pages on press instead of release.
  const bool usePressForPageTurn = SETTINGS.longPressChapterSkip == CrossPointSettings::LP_NONE;
  const bool isTooltipMode = SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP && tooltipOverlay;
  const bool tooltipUsesFront = isTooltipMode && SETTINGS.tooltipButtons == 0;
  const bool tooltipUsesSide = isTooltipMode && SETTINGS.tooltipButtons == 1;

  // Side buttons for page turn (skip if tooltip hijacks them)
  const bool sidePrev = !tooltipUsesSide && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::PageBack)
                                                                  : mappedInput.wasReleased(MappedInputManager::Button::PageBack));
  const bool sideNext = !tooltipUsesSide && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::PageForward)
                                                                  : mappedInput.wasReleased(MappedInputManager::Button::PageForward));
  // Front buttons for page turn (skip if tooltip hijacks them)
  const bool frontPrev = !tooltipUsesFront && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::Left)
                                                                    : mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool frontNext = !tooltipUsesFront && (usePressForPageTurn ? mappedInput.wasPressed(MappedInputManager::Button::Right)
                                                                    : mappedInput.wasReleased(MappedInputManager::Button::Right));

  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool prevTriggered = sidePrev || frontPrev;
  const bool nextTriggered = sideNext || frontNext || powerPageTurn;
```

- [ ] **Step 4: Reset tooltip on page change**

In `src/activities/reader/EpubReaderActivity.cpp`, in the page navigation section (around lines 374-400), after each `requestUpdate()` call inside the prevTriggered/nextTriggered blocks, add tooltip reset. The simplest approach: add a single reset at the top of each `requestUpdate()` trigger.

After line 373 (before `if (prevTriggered) {`), add:

```cpp
  // Reset tooltip when changing pages
  if (tooltipOverlay && (prevTriggered || nextTriggered)) {
    tooltipOverlay->onPageChanged();
  }
```

- [ ] **Step 5: Create/destroy TooltipOverlay in applyTranslationMode()**

In `src/activities/reader/EpubReaderActivity.cpp`, in `applyTranslationMode()` (line 642), after `SETTINGS.saveToFile();` (line 656) and before `section.reset();` (line 659), add:

```cpp
    // Create or destroy tooltip overlay based on mode
    if (translationMode == CrossPointSettings::CT_TOOLTIP) {
      tooltipOverlay = std::make_unique<TooltipOverlay>();
    } else {
      tooltipOverlay.reset();
    }
```

Also, in `onEnter()`, we need to create the overlay if the saved setting is already CT_TOOLTIP. Find the `onEnter()` method and add at the end:

```cpp
  if (SETTINGS.colorTextStyle == CrossPointSettings::CT_TOOLTIP) {
    tooltipOverlay = std::make_unique<TooltipOverlay>();
  }
```

- [ ] **Step 6: Add tooltip rendering in renderContents()**

In `src/activities/reader/EpubReaderActivity.cpp`, in `renderContents()` (line 925), after `page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);` (line 932) and before `renderStatusBar(...)` (line 933), add:

```cpp
  // Render tooltip overlay if active
  if (tooltipOverlay && tooltipOverlay->isActive()) {
    const int tooltipFontId = SETTINGS.fontSize > CrossPointSettings::SMALL
                                  ? CrossPointSettings(/* temp */).getReaderFontId()  // we need a helper
                                  : SETTINGS.getReaderFontId();
```

Actually, we need a cleaner way to get the smaller font ID. Add a helper function. In `src/tooltip/TooltipOverlay.h`, add a free function:

```cpp
// Get the font ID one size smaller than the current reader font.
// Falls back to same font if already at smallest size.
int getTooltipFontId();
```

In `src/tooltip/TooltipOverlay.cpp`, add the implementation:

```cpp
#include <fontIds.h>

int getTooltipFontId() {
  if (SETTINGS.fontSize <= CrossPointSettings::SMALL) {
    return SETTINGS.getReaderFontId();  // already smallest, use same
  }
  // Temporarily compute font ID with one size smaller
  const uint8_t smallerSize = SETTINGS.fontSize - 1;
  const uint8_t family = SETTINGS.fontFamily;
  switch (family) {
    case CrossPointSettings::BOOKERLY:
    default:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return BOOKERLY_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return BOOKERLY_14_FONT_ID;
        case CrossPointSettings::LARGE: return BOOKERLY_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return BOOKERLY_18_FONT_ID;
      }
    case CrossPointSettings::EDSLAB:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return EDSLAB_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return EDSLAB_14_FONT_ID;
        case CrossPointSettings::LARGE: return EDSLAB_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return EDSLAB_18_FONT_ID;
      }
    case CrossPointSettings::ALEGREYA:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return ALEGREYA_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return ALEGREYA_14_FONT_ID;
        case CrossPointSettings::LARGE: return ALEGREYA_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return ALEGREYA_18_FONT_ID;
      }
    case CrossPointSettings::GPRO:
      switch (smallerSize) {
        case CrossPointSettings::SMALL: return GPRO_12_FONT_ID;
        case CrossPointSettings::MEDIUM: default: return GPRO_14_FONT_ID;
        case CrossPointSettings::LARGE: return GPRO_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE: return GPRO_18_FONT_ID;
      }
  }
}
```

Now the renderContents integration becomes clean. After line 932, before line 933:

```cpp
  // Render tooltip overlay if active
  if (tooltipOverlay && tooltipOverlay->isActive()) {
    const int tooltipFontId = getTooltipFontId();
    const int vpWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
    const int vpHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
    tooltipOverlay->render(renderer, *page, SETTINGS.getReaderFontId(), tooltipFontId, orientedMarginLeft,
                           orientedMarginTop, vpWidth, vpHeight);
  }
```

- [ ] **Step 7: Build and verify**

Run:
```bash
pio run 2>&1 | tail -20
```

Expected: Build succeeds. All integration is wired up.

- [ ] **Step 8: Commit**

```bash
git add src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp src/tooltip/TooltipOverlay.h src/tooltip/TooltipOverlay.cpp
git commit -m "feat: integrate TooltipOverlay into EpubReaderActivity"
```

---

### Task 5: End-to-end testing on device

**Files:** None (testing only)

- [ ] **Step 1: Flash to device**

```bash
pio run --target upload
```

- [ ] **Step 2: Test tooltip mode activation**

1. Open an EPUB that has a translated chapter (`.translated.html` exists)
2. Go to Settings → Reader → Translation Mode → select "Tooltip"
3. Verify: page re-renders showing only original text (translated text hidden)
4. Verify: side buttons (PageBack/PageForward) still turn pages normally

- [ ] **Step 3: Test sentence stepping**

1. Press Right (front button) → tooltip appears for first sentence with underline
2. Press Right again → tooltip advances to next sentence
3. Continue pressing Right until last sentence
4. Press Right → tooltip dismisses
5. Press Right again → tooltip restarts from first sentence
6. Press Left while tooltip is showing → tooltip dismisses

- [ ] **Step 4: Test edge cases**

1. Navigate to a page with very long sentences → verify tooltip word-wraps
2. Navigate to the first line of a page → verify tooltip appears below (no space above)
3. Switch to an untranslated chapter → verify buttons work as normal page turns
4. Change "Tooltip Buttons" setting to "Side Buttons" → verify side buttons now step sentences and front buttons turn pages
5. Switch translation mode away from Tooltip → verify buttons return to normal

- [ ] **Step 5: Test mode switching**

1. Switch between CT_TOOLTIP and other modes (CT_DARK, CT_NO_RENDER, CT_SIDE_BY_SIDE)
2. Verify each mode switch causes proper cache rebuild
3. Verify no crashes or rendering artifacts

- [ ] **Step 6: Commit any fixes**

If any issues are found during testing, fix and commit:

```bash
git add -A
git commit -m "fix: address issues found during tooltip translation testing"
```
