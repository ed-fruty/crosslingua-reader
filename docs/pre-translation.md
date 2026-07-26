# Pre-Translation

Pre-Translation lets CrossPoint produce a bilingual copy of any EPUB on
device, then read it in one of several display modes — original-only,
translation-only, side-by-side, a page-translation overlay, or with translated paragraphs
shaded gray.

<img src="./images/pre-translation/overview.jpg" height="500" alt="Side-by-Side mode showing English original above each Russian translation" />

*Side-by-Side mode showing English original above each Russian translation.*

## What it does

- Translates a single chapter or the whole book on demand
- Stores the bilingual copy on the SD card next to the original
- Re-uses the translation when you re-open the book — no network needed after the first pass
- Lets you switch between several display modes without re-translating

## What it doesn't do

- It is **not** an on-the-fly translator that renders the page you are looking
  at through a live translation engine (separate PR).
- It does not modify your EPUB files.

## Setup

### 1. Pick your target language

Open a book, press **Confirm** to open the Reader Menu, scroll to
**Pre-Translation**, press **Confirm**.

In the submenu, pick **Target Language** and select the language you want
translations in.

<img src="./images/pre-translation/target-language.gif" height="500" alt="Target language picker" />

*Setting Russian as the target language.*

### 2. Pick your engine (optional)

Default is Google V2 (Free) which needs no API key. For DeepL, OpenAI,
DeepSeek, Gemini, or other engines requiring authentication, enter your
API key under **API Key**.

<img src="./images/pre-translation/engine.jpg" height="500" alt="Engine picker" />

### 3. Translate

From the Pre-Translation submenu:

- **Translate Chapter** — fastest. ~30 seconds for a typical chapter
  over WiFi.
- **Translate Book** — full book. ~4 minutes for a typical novel.

<img src="./images/pre-translation/translating-chapter.gif" height="500" alt="Translating chapter progress UI" />

*Progress UI during chapter translation.*

You can press **Back** to cancel at any time. Cancelling mid-chapter
leaves the chapter un-translated (the partial file is discarded).

## Display Modes

Once any chapter is translated, set **Display Mode** in the Pre-Translation
submenu. The mode applies to all translated chapters in the current book.

### Normal

Renders everything inline as the bilingual EPUB stores it — original and
translation interleaved without coloring. Use this when reading bilingual
EPUBs prepared elsewhere (e.g. Calibre's Polyglot output).

<img src="./images/pre-translation/mode-normal.jpg" height="500" alt="Normal mode" />

### Interleaved

Like Normal — original and translation alternate paragraph by paragraph —
but translated paragraphs are drawn in gray so you can tell them apart at a
glance.

The gray level is a separate stored setting (**Dimmed** or **Dimmed
Light**), not a mode of its own. It affects drawing only: switching shade
never re-lays out the chapter.

<img src="./images/pre-translation/mode-dark.jpg" height="500" alt="Interleaved mode, Dimmed shade" />

*Dimmed shade.*

<img src="./images/pre-translation/mode-light.jpg" height="500" alt="Interleaved mode, Dimmed Light shade" />

*Dimmed Light shade.*

> Earlier builds exposed the two shades as two separate display modes
> ("Dark" and "Light"). They are now one mode plus a shade setting; an
> existing setting saved under either old mode is migrated automatically on
> upgrade.

### Original Only

Drops translated paragraphs from the page. The book looks identical to the
untranslated original — useful when you want to read in the source language
and only consult translations occasionally.

<img src="./images/pre-translation/mode-original.jpg" height="500" alt="Original only mode" />

### Translation Only

Drops original paragraphs. The book reads as if it were monolingual in
your target language.

<img src="./images/pre-translation/mode-translation.jpg" height="500" alt="Translation only mode" />

### Side by Side

Pairs each original paragraph with its translation, with no spacing
between paired paragraphs and normal spacing between pairs.

<img src="./images/pre-translation/mode-side-by-side.jpg" height="500" alt="Side by side mode" />

### Page Translation

The reader shows only the original text on each page. **Long-press either
side button** to bring up an overlay with the translations of the
paragraphs visible on the current page.

<img src="./images/pre-translation/mode-modal.gif" height="500" alt="Page Translation mode overlay" />

*Long-press to open, side buttons scroll, Back to close.*

In Page Translation mode, when a paragraph spans page boundaries (starts on one page
and continues on the next), the overlay shows only the translation of the
sentences actually visible on the current page — not the whole paragraph.

## Mode switching with no translation

If you switch to any non-Normal mode but the current chapter hasn't been
translated, the device shows a notice and reverts to Normal mode. Translate
the chapter (or the book) before selecting a translation-dependent mode.

## Re-translating

If a chapter is already translated, the menu shows **Re-translate Chapter**
instead of "Translate Chapter". Re-translation deletes the current
bilingual copy and starts over. Useful if you change target language or
engine.

For whole books, **Re-translate Book** has two sub-options:

- **Skip Translated** — only re-translates chapters that don't have a
  bilingual copy yet (resume after a cancel)
- **Re-translate All** — wipes and redoes every chapter

## Deleting translations

To free SD card space or start fresh, pick **Delete Translations** in the
Pre-Translation submenu. This removes all bilingual copies for the current
book but leaves the original EPUB untouched.

## How it stores translations

Bilingual copies are written to:

```
.crosspoint/epub_<hash>/sections/<N>.translated.html
```

They survive font/size changes (which invalidate layout cache) and reboot.
They are tied to the EPUB's file hash — moving or renaming the EPUB
invalidates them.

## Translation engines

| Engine | Authentication | Notes |
|---|---|---|
| Google (Free) - New | None | Default. Uses `translate.googleapis.com/translate_a/single`. Best free choice. |
| Google (Free) - HTML | None | Google free with HTML preservation. |
| Google (Free) - Old | None | Legacy free endpoint. |
| DeepL | API key | Free tier — 500k chars/month. |
| DeepL Pro | API key | Paid. |
| OpenAI | API key | Translation via chat-completion API. |
| DeepSeek | API key | Translation via DeepSeek chat API. |
| Gemini | API key | Translation via Google Gemini API. |

## Notes

- **Network required only during translation.** Once translated, the book
  reads offline.
- **CJK sentence detection** is limited in v1 — paragraphs straddling pages
  in Page Translation mode may show the whole translated paragraph rather than the
  sentences corresponding to visible text. Latin, Cyrillic, Greek, and
  Vietnamese punctuation work correctly.
- **Translation engines have rate limits.** Google Free can throttle on
  long sessions. DeepL Free has a 500k char/month cap.
- **Translation API security.** Requests are encrypted (TLS) but the
  server certificate is not verified, matching the firmware's standard
  HTTPS stack. API keys for paid engines (DeepL, OpenAI, DeepSeek,
  Gemini) travel over this connection; treat them as you would any
  credential on an untrusted network. The free Google engines send only
  the text being translated.
- The first run takes longer than re-runs — chapter layouts re-index after
  translation completes.
