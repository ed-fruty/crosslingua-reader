# Tooltip Translation Mode — Design Spec

## Overview

New translation mode (`CT_TOOLTIP`) for the CrossPoint Reader. When active on a translated chapter, the page displays only original text. The user steps through sentences with front buttons (Left/Right), and a tooltip overlay shows the translation for each sentence. Side buttons retain normal page-turn behavior.

**Branch:** `feat/tooltip-translation` (off `fruty-custom`)

## Goals

- Sentence-level translation display via on-demand tooltips
- Minimal changes to existing files (fork tracks upstream)
- Independent mode with own cache, parser logic, and rendering
- Under 1KB additional RAM when active, zero when inactive

## Non-Goals

- On-the-fly translation (uses existing `.translated.html` only)
- Triangle pointer on tooltip (deferred, can add later with `drawLine()`)
- Partial e-ink refresh

---

## 1. New Translation Mode: CT_TOOLTIP

Add `CT_TOOLTIP = 6` to `COLOR_TEXT_STYLE` enum in `CrossPointSettings.h` (before `_COUNT`).

In `ChapterHtmlSlimParser.cpp`, add an independent block:

```cpp
// Tooltip: preserve both original and translated words.
// Translated words keep their grayLevel marker (wordColor unchanged).
// They will be hidden at render time via colorTextGrayLevel=3,
// but remain in TextBlock data for tooltip extraction.
if (translationMode == 6) {
  // No transformation needed — keep wordColor as-is.
  // Original words (wordColor=0) render normally.
  // Translated words (wordColor>0) are stored but hidden during render.
}
```

This keeps both original and translated words in the section `.bin` cache. The cache key is automatically separate (translationMode=6 stored in section header).

**Page rendering in tooltip mode:** In `main.cpp` (or `EpubReaderActivity`), when CT_TOOLTIP is active, call `renderer.setColorTextGrayLevel(3)`. The renderer already supports grayLevel=3 as "hidden" — words with grayLevel > 0 are simply not drawn. This means only original text appears on the page. The translated words remain accessible in TextBlock data for the tooltip overlay to extract.

This approach is fully independent from CT_DARK/CT_LIGHT. It uses a different mechanism: CT_DARK sets `colorTextGrayLevel=1` to render translations in gray; CT_TOOLTIP sets `colorTextGrayLevel=3` to hide them entirely.

When the chapter has no translation, CT_TOOLTIP behaves like CT_NORMAL — no overlay, normal button behavior.

## 2. Sentence Splitting

**New files:** `src/tooltip/SentenceSplitter.h`, `src/tooltip/SentenceSplitter.cpp`

Single-pass state machine over paragraph text. Ported from readest's `mode7-tooltip.ts`, simplified for C++/embedded.

### Algorithm

- Scan character by character for terminators: `.` `!` `?`
- Terminator must be followed by space, newline, or end-of-string
- Skip ellipsis (`...`)
- Skip abbreviations — small static array: `Mr.`, `Dr.`, `Mrs.`, `Ms.`, `Inc.`, `St.`, `vs.`, `etc.`, `Jr.`, `Sr.`, `Prof.`, `Gen.`, `Gov.`, `Sgt.`, `Corp.`, `Ltd.`, `Fig.`, `Vol.`, `Dept.`, `Univ.`, `approx.`, `i.e.`, `e.g.`
- Handle closing quotes/brackets (`"`, `'`, `)`, `]`) after punctuation before the space check

### Output

```cpp
struct SentenceSpan {
  uint16_t startWord;  // word index in paragraph (original words only)
  uint16_t endWord;    // exclusive
};
```

Word indices map directly to TextBlock word lists. Max 50 sentences per paragraph (fixed-size stack array).

### Translation Mapping

For each original sentence, find the corresponding translated words:

- Walk original words (grayLevel=0) to build original sentences
- Walk translated words (grayLevel>0) to build translated sentences
- If sentence counts match: 1:1 mapping
- If counts differ: proportional character-length mapping (from readest's `mapSentences()`)

No heap allocations. Translated text for the active tooltip is built into a ~256 byte stack buffer at render time only.

## 3. Tooltip State Machine

**New files:** `src/tooltip/TooltipOverlay.h`, `src/tooltip/TooltipOverlay.cpp`

### State

```
IDLE  ←→  SHOWING_SENTENCE_N
```

- `int8_t currentSentenceIndex = -1` (-1 = IDLE)
- `bool wrapAround = false` (passed last sentence once)
- Cached sentence spans rebuilt on page change

### Button Handling

`bool TooltipOverlay::handleInput(MappedInputManager&)` — returns true if input consumed.

**Tooltip buttons (front Left/Right by default, configurable):**

| State | Next button | Behavior |
|-------|------------|----------|
| IDLE | next | Show tooltip for sentence 0 |
| Showing N, N < last | next | Show tooltip for sentence N+1 |
| Showing last sentence | next | Dismiss tooltip, set wrapAround=true |
| IDLE, wrapAround=true | next | Show sentence 0, reset wrapAround |
| Showing any | back | Dismiss tooltip (close), set IDLE |
| IDLE | back | Not consumed, falls through to normal handling |

Back button behavior is designed for easy future change to "show previous sentence" mode.

**Page-turn buttons (side PageBack/PageForward by default):**
- Not consumed by tooltip overlay
- Fall through to normal page navigation
- On page change: `TooltipOverlay::onPageChanged()` resets to IDLE, clears cached sentences

### Button Setting

New setting `tooltipButtons` in `CrossPointSettings.h`:
- `0` = front buttons (Left/Right) for tooltip, side for page turns (default)
- `1` = side buttons for tooltip, front for page turns

## 4. Tooltip Rendering

`TooltipOverlay::render(GfxRenderer&, Page&, int xOffset, int yOffset, int fontId)`

Called from `EpubReaderActivity::renderContents()` after page content is drawn. Only executes when `currentSentenceIndex >= 0`.

### Steps

1. **Find sentence screen position** — walk Page elements to find PageLines containing the active sentence's word range. Extract `sentenceY`, `sentenceStartX`, `sentenceEndX`, `sentenceLineHeight`.

2. **Build tooltip text** — concatenate translated words for this sentence from Page's TextBlock data (grayLevel > 0). Stack buffer, ~256 bytes.

3. **Calculate dimensions** — measure text width via `renderer.getTextWidth()`. Word-wrap if wider than viewport. `tooltipWidth = min(textWidth + padding*2, viewportWidth - margin*2)`. Max height: ~40% of viewport.

4. **Position above or below:**
   - `spaceAbove = sentenceY - pageTopMargin`
   - If `tooltipHeight + gap <= spaceAbove` → above: `tooltipY = sentenceY - gap - tooltipHeight`
   - Else → below: `tooltipY = sentenceY + sentenceLineHeight + gap`
   - Clamp to viewport bounds

5. **Draw:**
   - `fillRect` — white fill to erase underlying text
   - `drawRoundedRect` — thin black border, rounded corners
   - `drawText` — tooltip translation text, one font size smaller than page
   - `drawLine` — underline under active sentence on the page

### Font Size

Use the next smaller system font size. Available sizes: 12, 14, 16, 18.

| Page font | Tooltip font |
|-----------|-------------|
| 18 | 16 |
| 16 | 14 |
| 14 | 12 |
| 12 | 12 |

Easy to change to same-size via a single constant.

### Visual Style

- No background fill (page color shows through)
- 1px black border, small corner radius
- No triangle pointer (deferred — can add later with `drawLine()`)
- Active sentence: underline spanning all words of the sentence

## 5. Edge Cases

- **Untranslated chapters:** CT_TOOLTIP = CT_NORMAL behavior. No overlay, all buttons normal.
- **Sentences spanning multiple lines:** Underline follows across all lines. Tooltip positioned at first line.
- **Sentences spanning two pages:** Only show sentences starting on current page. Use available translated words on this page.
- **Long translations:** Word-wrap in tooltip. If exceeds 40% viewport height, truncate with "...".
- **E-ink refresh:** Full page redraw via `requestUpdate()` — same as all other screen updates.

## 6. Changes to Existing Files

| File | Changes | Lines |
|------|---------|-------|
| `src/CrossPointSettings.h` | `CT_TOOLTIP = 6` enum value, `tooltipButtons` setting | ~3 |
| `src/SettingsList.h` | Label for CT_TOOLTIP, tooltipButtons setting entry | ~5 |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | Independent `translationMode == 6` block (no-op, preserves both texts) | ~4 |
| `src/main.cpp` | Set `colorTextGrayLevel=3` when CT_TOOLTIP active | ~2 |
| `src/activities/reader/EpubReaderActivity.h` | Include + `unique_ptr<TooltipOverlay>` member | ~2 |
| `src/activities/reader/EpubReaderActivity.cpp` | Input dispatch, render call, mode enter/exit, page change reset | ~20 |
| I18n YAML files (8 languages) | `STR_TOOLTIP`, `STR_TOOLTIP_BUTTONS`, `STR_FRONT_BUTTONS`, `STR_SIDE_BUTTONS` | ~4 per file |

**Total:** ~25 lines across existing C++ files.

## 7. New Files

| File | Purpose | Est. size |
|------|---------|-----------|
| `src/tooltip/TooltipOverlay.h` | Tooltip state, rendering, input handling | ~60 lines |
| `src/tooltip/TooltipOverlay.cpp` | Implementation | ~250 lines |
| `src/tooltip/SentenceSplitter.h` | Sentence splitting and translation mapping | ~30 lines |
| `src/tooltip/SentenceSplitter.cpp` | Implementation | ~150 lines |

## 8. RAM Budget

| Component | Bytes | When |
|-----------|-------|------|
| TooltipOverlay object | ~50 | CT_TOOLTIP active |
| Sentence spans (max 50) | ~400 | Page loaded in tooltip mode |
| Tooltip text buffer | ~256 | During render only (stack) |
| **Total** | **~700** | **CT_TOOLTIP active** |
| When inactive | 0 | All other modes |
