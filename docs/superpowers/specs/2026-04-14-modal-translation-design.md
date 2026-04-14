# Modal Translation Mode — Design Spec

## Overview

New translation mode (`CT_MODAL`) for the CrossPoint Reader. When active on a translated chapter, the page displays only original text (same as tooltip mode). On button press, a full-screen white overlay replaces the page content and shows the translated text for all paragraphs visible on the current page. The user scrolls through the translation with short presses and turns pages with long presses.

**Branch:** `feat/modal-translation` (off current `feat/book-translation`)

## Goals

- Full-page translation overlay showing all visible paragraphs' translations at once
- Paragraph-level extraction from HTML (no sentence splitting needed)
- Reuse existing settings: `tooltipButtons` and `tooltipBehavior`
- Independent overlay module, similar structure to `TooltipOverlay`
- Minimal RAM footprint on ESP32-C3

## Non-Goals

- Preserving heading/list/blockquote visual hierarchy (plain text flow only)
- On-the-fly translation (uses existing `.translated.html` or EPUB bilingual HTML)
- Partial e-ink refresh

---

## 1. New Translation Mode: CT_MODAL

Add `CT_MODAL = 7` to `COLOR_TEXT_STYLE` enum in `CrossPointSettings.h` (before `_COUNT`).

Cache strategy: **identical to CT_TOOLTIP**. In `ChapterHtmlSlimParser.cpp`, `CT_MODAL` (translationMode=7) uses the same no-op block as CT_TOOLTIP — both original and translated words are kept in the `.bin` cache, with translated words hidden at render time via `colorTextGrayLevel=3`.

When the chapter has no translation, CT_MODAL behaves like CT_NORMAL — no overlay, normal button behavior.

## 2. Paragraph Extraction from HTML

The modal overlay extracts translations at the paragraph level, not sentence level. This is substantially simpler than tooltip's sentence index approach.

### HTML Sources (same two as tooltip)

1. **`.translated.html`** — our translator output. Format: `<p>original</p><p lang="xx" data-translation="true">translation</p>` alternating pairs.
2. **Fallback: extracted EPUB HTML** — for Calibre-style pre-translated books. Extracted to `.modal.html` cache file (same pattern as tooltip's `.tooltip.html`).

### Extraction Algorithm

Streaming expat parser (same 1KB buffer approach as tooltip):
1. Walk block-level tags (`<p>`, `<h1>`–`<h6>`, `<li>`, `<blockquote>`, `<div>`)
2. Detect translation paragraphs by `lang`, `xml:lang`, or `data-translation` attribute
3. For each translation paragraph, store the text into a `std::vector<std::string>` (one entry per translated paragraph)
4. This gives us an ordered list of ALL translated paragraphs for the entire chapter

This runs once per section load (not per page), cached in the `ModalOverlay` object.

### Page-to-Paragraph Matching

The Page contains `PageLine` elements, each referencing a `TextBlock`. Multiple `PageLine`s sharing the same `TextBlock*` pointer belong to the same paragraph. By collecting the original words from each unique `TextBlock` group, we can build a list of original paragraph texts visible on the current page.

Matching strategy:
1. Collect page paragraphs: walk `Page::elements`, group consecutive `PageLine`s by `TextBlock*` identity, concatenate their words (grayLevel=0 only) into paragraph strings
2. For each page paragraph, build a key from the first N words (normalized, same as tooltip's `sentenceKey` approach)
3. Also parse original paragraphs from the HTML (the non-translation blocks) and build matching keys
4. Match page paragraph keys against HTML paragraph keys to find the corresponding index
5. Use that index to look up the translation from the parallel translation list

**Simpler alternative (sequential position):** Since we know which chapter section we're in, and pages are sequential, we can track a `paragraphOffset` — the index of the first paragraph on the current page within the chapter. On page change, advance or rewind the offset. This avoids per-paragraph key matching entirely.

We'll use the **key-matching approach** for robustness (handles partial paragraphs at page boundaries, same proven pattern as tooltip), but with paragraph-level keys instead of sentence-level keys.

## 3. ModalOverlay State Machine

### State

```
IDLE  ←→  SHOWING (with scrollOffset)
```

- `bool active = false` (false = IDLE, true = showing overlay)
- `int16_t scrollOffset = 0` (pixel offset for scrolling within the modal)
- `int16_t totalContentHeight = 0` (total rendered height of all translated paragraphs)
- Cached paragraph translations rebuilt on section/page change

### Activation / Deactivation

- **Activate:** Short press of next button while IDLE → show overlay at scrollOffset=0
- **Deactivate:** ESC/Back button while overlay is showing → dismiss overlay, return to page
- **Deactivate:** Short press of back button while at scrollOffset=0 → dismiss overlay

### Scrolling

When active, short presses scroll through the translated content:

- **Next button (short press):** Scroll down by one "screen" (viewportHeight minus some overlap). If already at the bottom → behavior depends on `tooltipBehavior`:
  - Loop (0): wrap to scrollOffset=0
  - Page Turn (1): set `pendingPageForward`, auto-activate on next page
- **Back button (short press):** Scroll up by one screen. If already at scrollOffset=0 → dismiss overlay (back to original page). This matches tooltip's pattern where back button dismisses.

### Long Press

- **Next button (long press, ≥700ms):** Page turn forward (same as tooltip). Dismiss overlay, set `pendingPageForward`.
- **Back button (long press, ≥700ms):** Page turn backward. Dismiss overlay, set `pendingPageBack`.

### Page Turn Integration

Same flag-based mechanism as tooltip:
- `pendingPageForward` / `pendingPageBack` — checked by `EpubReaderActivity` after `handleInput()`
- `activateOnNextPage` — auto-show overlay on the new page after tooltip-behavior page turn
- `onPageChanged()` — resets state, clears cached translations

## 4. Modal Rendering

`ModalOverlay::render(GfxRenderer&, const Page&, int fontId, int modalFontId, int xOffset, int yOffset, int viewportWidth, int viewportHeight)`

Called from `EpubReaderActivity::renderContents()` after page content is drawn (same hook point as tooltip). Only executes when `active == true`.

### Steps

1. **Prepare page** (lazy, once per page): extract translated paragraphs for visible page paragraphs from cached chapter translations.

2. **Draw white overlay:** `fillRect` covering the entire viewport area (xOffset, yOffset, viewportWidth, viewportHeight). This completely hides the original page content.

3. **Render translated text:** Starting from `-scrollOffset` (to support scrolling), draw each translated paragraph:
   - Word-wrap each paragraph's text to fit `viewportWidth - 2*padding`
   - Draw with `modalFontId` (one size smaller than page font, same rule as tooltip)
   - Add paragraph spacing between paragraphs (e.g., `lineHeight * 0.5`)
   - Track `totalContentHeight` for scroll bounds

4. **Scroll indicator** (deferred): No scroll indicator in v1. Can add a thin position bar on the right edge later if needed.

### Font Size

Same rule as tooltip:

| Page font | Modal font |
|-----------|-----------|
| 18 | 16 |
| 16 | 14 |
| 14 | 12 |
| 12 | 12 |

### Visual Style

- Full white background covering the entire page area
- No border (the overlay IS the page)
- Text rendered with standard paragraph spacing
- Padding: same as page margins (or a fixed 8-12px internal padding)

## 5. Text Rendering Within the Modal

The modal needs to word-wrap and render multi-paragraph translated text. This is different from tooltip (which renders a single short sentence) — we're rendering potentially a full page of text.

### Approach: Manual word-wrap using GfxRenderer

1. Split each paragraph into words (simple space-split)
2. Measure each word width with `renderer.getTextWidth(modalFontId, word)`
3. Accumulate words on a line until exceeding `viewportWidth - 2*padding`
4. Draw each line with `renderer.drawText()`
5. Advance Y by `lineHeight` (from `renderer.getFontLineHeight(modalFontId)`)
6. Between paragraphs, advance Y by extra spacing

This is the same technique tooltip uses for its word-wrapped text, just applied to multiple paragraphs.

### Scroll Mechanics

- `scrollOffset` is in pixels, represents how far down we've scrolled
- One "screen" scroll = `viewportHeight - lineHeight` (small overlap for reading continuity)
- Content that's above the viewport (y < yOffset) or below (y > yOffset + viewportHeight) is simply not drawn (skip draw calls, not clip)
- `totalContentHeight` is computed during the first render pass and cached

## 6. Edge Cases

- **Untranslated chapters:** CT_MODAL = CT_NORMAL behavior. No overlay, all buttons normal.
- **Partial paragraphs at page boundaries:** First/last paragraph on a page may be a fragment. Key matching finds the full translation from the HTML. The user sees the complete translated paragraph even if only part of the original is on the current page.
- **Very long translations:** Scrollable — the user can scroll through any amount of translated text.
- **Empty translation for a paragraph:** Skip it in the modal display (don't show a blank gap).
- **Page with only images:** No translated paragraphs to show. Buttons fall through to normal page navigation.
- **E-ink refresh:** Full page redraw via `requestUpdate()` on every scroll step.

## 7. Changes to Existing Files

| File | Changes | Lines |
|------|---------|-------|
| `src/CrossPointSettings.h` | `CT_MODAL = 7` enum value | ~1 |
| `src/SettingsList.h` | Label for CT_MODAL in translation mode list | ~1 |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | `translationMode == 7` block (same no-op as tooltip) | ~1 |
| `src/main.cpp` | Set `colorTextGrayLevel=3` when CT_MODAL active | ~1 |
| `src/activities/reader/EpubReaderActivity.h` | Include + `unique_ptr<ModalOverlay>` member | ~2 |
| `src/activities/reader/EpubReaderActivity.cpp` | Input dispatch, render call, HTML setup, page change reset | ~25 |
| I18n YAML files (8 languages) | `STR_MODAL` string | ~1 per file |

**Total:** ~35 lines across existing C++ files.

## 8. New Files

| File | Purpose | Est. size |
|------|---------|-----------|
| `src/tooltip/ModalOverlay.h` | Modal state, rendering, input handling | ~50 lines |
| `src/tooltip/ModalOverlay.cpp` | Implementation: HTML parsing, paragraph matching, word-wrap rendering, scroll | ~400 lines |

Placed in `src/tooltip/` alongside `TooltipOverlay` since they share the same domain (translation overlay UI).

## 9. RAM Budget

| Component | Bytes | When |
|-----------|-------|------|
| ModalOverlay object | ~40 | CT_MODAL active |
| Chapter translations (all paragraphs) | ~2-5KB | Section loaded (depends on chapter length) |
| Page paragraph translations (current page) | ~500B | Page prepared |
| Word-wrap line buffers | ~256B | During render only (stack) |
| **Total peak** | **~3-6KB** | **CT_MODAL active, section loaded** |
| When inactive | 0 | All other modes |

Note: This is higher than tooltip (~700B) because we store all chapter translations at once for fast page switching. If this is too much, we can fall back to per-page extraction (re-parse HTML on each page change, similar to tooltip). The chapter-level cache avoids re-parsing the HTML file on every page turn.

## 10. Settings Reuse

No new settings fields needed. CT_MODAL reuses:
- `tooltipButtons` (0=front buttons, 1=side buttons) — controls which buttons scroll the modal
- `tooltipBehavior` (0=Loop, 1=Page Turn) — controls end-of-content behavior

The setting names say "tooltip" but apply to both modes. We keep them as-is per the design decision to minimize churn.
