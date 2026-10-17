# Lingua

Lingua is available on every CrossLingua target: Xteink X3, X4, X4 Pro, and
M5Stack Paper Mono. The same translation modes and workflows are shared across
the button-only and touch/frontlight devices.

Lingua is CrossLingua's bilingual reading system. It can produce a bilingual
copy of any EPUB on the device or use language-tagged translations already
embedded by a Calibre workflow. The current code provides nine display modes:
Normal, Interleaved, Side by Side, Original Only, Translation Only, Tooltip,
Page Translation, Interlinear, and Word Lens. Word Lens needs no translation at
all: it glosses hard words from a downloaded word list.

<img src="./images/crosslingua/interlinear.jpg" height="500" alt="CrossLingua Interlinear mode" />

*Interlinear mode on an Xteink reader.*

## What it does

- Translates a single chapter or the whole book on demand
- Stores the bilingual copy on the SD card next to the original
- Re-uses the translation when you re-open the book — no network needed after the first pass
- Lets you switch between several display modes without re-translating
- Recognizes translated paragraphs embedded in bilingual EPUBs

## What it doesn't do

- It is **not** an on-the-fly translator that renders the page you are looking
  at through a live translation engine (separate PR).
- It does not modify your EPUB files.

## Setup

### 1. Pick your engine (optional)

Default is Google V2 (Free) which needs no API key. Azure is also keyless.
For DeepL, OpenAI, DeepSeek, Gemini, or other engines requiring
authentication, enter your API key under **API Key** (the row only appears
for engines that need one).

### 2. Translate

Open a book, press **Confirm**, and select **Lingua**. Start **Translate
Chapter** or **Translate Book**, then choose the target and source languages
in the translation flow. The source language can be detected automatically.

- **Translate Chapter** — fastest. ~30 seconds for a typical chapter
  over WiFi.
- **Translate Book** — full book. ~4 minutes for a typical novel.

You can press **Back** to cancel at any time. Cancelling mid-chapter
leaves the chapter un-translated (the partial file is discarded).

## Display Modes

Once any chapter is translated, set **Display Mode** in the Lingua
submenu. The mode applies to all translated chapters in the current book.

### Normal

Renders everything inline as the bilingual EPUB stores it — original and
translation interleaved without coloring. Use this when reading bilingual
EPUBs prepared elsewhere (e.g. Calibre's Polyglot output).

### Interleaved

Like Normal — original and translation alternate paragraph by paragraph —
but translated paragraphs are drawn in gray so you can tell them apart at a
glance.

The gray level is a separate stored setting (**Dimmed** or **Dimmed
Light**), not a mode of its own. It affects drawing only: switching shade
never re-lays out the chapter.

> Earlier builds exposed the two shades as two separate display modes
> ("Dark" and "Light"). They are now one mode plus a shade setting; an
> existing setting saved under either old mode is migrated automatically on
> upgrade.

### Original Only

Drops translated paragraphs from the page. The book looks identical to the
untranslated original — useful when you want to read in the source language
and only consult translations occasionally.

### Translation Only

Drops original paragraphs. The book reads as if it were monolingual in
your target language.

### Side by Side

Lays each source paragraph and its translation into synchronized left and
right columns. The translation column can be Black, Grey, or Light Grey.

<img src="./images/crosslingua/side-by-side-landscape.jpg" height="500" alt="CrossLingua Side by Side mode" />

### Page Translation

The reader shows only the original text on each page. **Long-press either
side button** to bring up an overlay with the translations of the
paragraphs visible on the current page.

*Long-press to open, side buttons scroll, Back to close.*

In Page Translation mode, when a paragraph spans page boundaries (starts on one page
and continues on the next), the overlay shows only the translation of the
sentences actually visible on the current page — not the whole paragraph.

The overlay controls can use either the front or side button pair. Translation
text can use the same size as the book or one step smaller.

### Tooltip

Shows the original page and reveals one translated sentence at a time in a
small overlay. Sentence stepping can use either the front or side button pair.
At a page boundary it can either loop within the current page or turn the page
and continue. Translation text can use the same size as the book or one step
smaller.

<img src="./images/crosslingua/tooltip-portrait.jpg" height="500" alt="CrossLingua Tooltip mode in portrait orientation" />

### Interlinear

Places compact translated annotations above the source lines they belong to.
The annotation colour can be Black, Grey, or Light Grey, and the annotation
size can be 8 pt (the default), 10 pt, or 12 pt. Changing the size re-lays the
chapter out, since taller annotation rows leave fewer lines for the source text
on each page. All three stay narrower than the body text, which is what keeps a
sentence's translation above its own source lines most of the time.

A larger annotation size needs more room per sentence, and a translation is
often longer than the sentence it translates. When one does not fit the strips
above its own source lines, the tail is carried into the following strips
rather than cut, so no translated text is lost. The trade is that a carried
tail sits above a later source line than the sentence it belongs to, so the
bigger the annotation size, the further the translation can drift from its
sentence. At 8 pt this almost never happens; at 12 pt it is occasional.

If a paragraph's translation outruns every strip above it, the remainder is
drawn as small-type lines directly below the paragraph, with no source line
between them.

### Word Lens

Shows a short gloss in your language above the hard words of the book, in a
small strip over each line, like Kindle's Word Wise. No translation is needed:
the glosses come from a **gloss pack** for the language pair (the book's
language → your hint language), downloaded once.

*Setup.* Pick **Display Mode → Word Lens** in the Lingua menu. Under it:

- **Gloss Pack** — shows whether the pack for this book is installed; select it
  to open the pack list (Wi-Fi), where packs can be downloaded or deleted. Each
  row names the pair; the line under it shows the size, or "Installed". The
  device restarts into the book when you leave the list.
- **Level** — A1, A2, B1, B2, C1 or C2. A word is glossed when it is rarer than
  the vocabulary of that level, so A1 glosses the most words and C2 only the
  rarest. The levels are **word-frequency bands named after CEFR levels**, not a
  real CEFR grading of each word: A1 glosses everything outside the 1000
  commonest words, A2 outside 2000, B1 4000, B2 8000, C1 14000 and C2 24000. The
  1000 commonest words are never glossed. An inflected form uses its lemma's rank
  ("whispered" counts as "whisper").
- **Hint Language** — the language of the glosses. The list offers only
  languages with a pack for the book's language (installed, or in the pack list
  last fetched). **Automatic** uses the translator's Target Language when such a
  pack exists, then the device's UI language, then any installed pack for the
  book. Choosing a hint language does not change the translator's Target
  Language.
- **Book Language** — only for a book that declares no language.
- **Translation Colour** and **Translation Size** — the strip's colour and type
  size, shared with Interlinear.
- **Empty Strips** — **Hide** (default) gives a strip only to lines with a
  glossed word, so a page holds more text; **Keep** gives every line a strip, so
  the line pitch stays even.
- **Page Turn Buttons** — which button pair turns pages (default: the side
  buttons). The other pair steps through the hard words of the page.
- **At Last Word** — whether stepping past the last (or first) hard word wraps
  around (**Loop**) or turns the page (**Page Turn**).
- **Toggle by Longpress** — a long press of the stepping pair hides or shows
  the strips (their space stays), separately from Interlinear's own toggle.

*Reading.* The source text is laid out exactly as in Original Only; each line
gets a strip above it (blank strips keep the line pitch even, unless Empty
Strips is set to Hide), so a page holds fewer lines. A gloss sits centred over its word. When two would overlap, the
rarer word keeps its gloss and the other is shortened with "…" or, if fewer than
three letters would fit, left out. Capitalised words in mid-sentence are taken
for names and not glossed (except in German, which capitalises every noun), and
words inside real `<ruby>` keep their own annotation.

*Looking a word up.* Press the stepping pair: the first press focuses the first
(or, backwards, the last) hard word of the page, underlined — including words
whose gloss was shortened or left out. **Confirm** opens the word in your
dictionary when one is installed (Settings → Dictionary) and has it, trying the
word as written and then its lemma; otherwise it shows the full gloss with the
lemma. **Back** clears the focus. On touch screens, tap a glossed word or its
gloss. Turning the page clears the focus.

*Packs.* Packs exist for English → Arabic, Chinese, English (simple
definitions), French, German, Hungarian, Portuguese, Russian, Spanish,
Ukrainian, Vietnamese, and for German, French, Italian, Portuguese, Russian and
Spanish → English (0.5 – 1.7 MB each, `/.crosspoint/wordlens/` on the card). They
are attached to every CrossLingua release (`word-lens-<pair>.lens`) and fetched
from the latest release; if GitHub cannot be reached, the device downloads
Readest's original JSON pack from `cdn.readest.com` and converts it itself: it
switches Wi-Fi off first, shows the progress, and **Back** cancels (a minute or
two; picking another pack afterwards reconnects). English → Ukrainian is only on
our releases. Changing the pack, the hint language, the level or Empty Strips
re-lays the chapter out.

The data is Readest's Word Lens data (WikDict / Wiktionary glosses and
FrequencyWords ranks, CC-BY-SA; ECDICT, MIT); the English → Ukrainian glosses are
the English → Russian ones machine-translated into Ukrainian. See
`lingua-data/wordlens/ATTRIBUTION.md`; each pack also carries its own notice.

## Word Lens pack maintenance

The packs are committed under `lingua-data/wordlens/` as one reproducible
archive, `word-lens-packs.tar.xz` (not part of the firmware image), and are
refreshed with one script:

```bash
python3 scripts/wordlens/update_packs.py --dry-run          # what would change
python3 scripts/wordlens/update_packs.py                    # refresh changed packs
python3 scripts/wordlens/update_packs.py --pairs en-ru,en-uk
```

It fetches Readest's manifest and packs from `cdn.readest.com/wordlens`, checks
their sha256, re-converts only the packs whose source changed (the conversion is
deterministic), rebuilds `en-uk` from the refreshed `en-ru` (Russian glosses
already in `scripts/wordlens/en-uk.translations.tsv` are reused, only new ones
are machine-translated; `--offline` never calls out, `--retry-failed` retries
earlier failures), rewrites the archive (packs, manifest, attribution) and
`ATTRIBUTION.md`, prints what changed, and leaves the commit to you. The release
workflow unpacks the archive, checks every pack against the manifest and
attaches the packs, the manifest and the attribution to the next release. Tests: `python3 -m unittest test_wordlens`
in `scripts/wordlens/` (also run by `ctest`). Format details:
[`.lens` in file-formats.md](file-formats.md#word-lens-pack-lens).

## Mode switching with no translation

If you switch to any non-Normal mode but the current chapter hasn't been
translated, the device shows a notice and reverts to Normal mode. Word Lens is
the exception: it never needs a translation, and a book without an installed
pack is shown plain with a one-time notice. Translate
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
Lingua submenu. This removes all bilingual copies for the current
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
| Azure | None | Microsoft's Edge-browser translator endpoint. Keyless — see below. |

### Azure

Azure needs **no API key and no region**: pick it in the engine cycler and
translate. It calls `api-edge.cognitive.microsofttranslator.com`, the
deployment behind Microsoft Edge's built-in page translator, and
authenticates with a short-lived bearer token fetched anonymously from
`edge.microsoft.com/translate/auth` (cached for 8 minutes — normally one
fetch per chapter, renewed between batches on a chapter long enough to
outlive it). No credentials of any kind are sent.

This is **not** the paid Azure Translator resource
(`api.cognitive.microsofttranslator.com`), which would require a
subscription key plus a region. Neither Edge endpoint is a documented,
supported API, so Microsoft can change or withdraw them without notice —
the same durability caveat that applies to the free Google engines.

Azure is one of the engines that batches: a whole batch of paragraphs goes
out as a single array of text items and comes back as one result per item,
in order, so it makes far fewer requests per chapter than the per-paragraph
engines. Chinese targets are sent as `zh-Hans` / `zh-Hant`, Norwegian as
`nb` and Serbian as `sr-Cyrl`, which are the codes Azure's language list
uses.

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
  credential on an untrusted network. The free Google engines and Azure
  send only the text being translated.
- The first run takes longer than re-runs — chapter layouts re-index after
  translation completes.
