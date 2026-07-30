# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 34 — current

**The byte layout is unchanged from 33.** No new field, no new tag, no change to
`TextBlock`'s arena or to the header. The fork previously used 34 and then 35 because
the version *is* the cache key and the **content** of every cached Interlinear page
changed under each:

- **34** — the source stopped breaking a line at every sentence, and each source line
  gained exactly one annotation strip in place of a variable stack over the
  sentence's first line.
- **35** — a sentence's translation is now *distributed* across the strips above its
  own source lines instead of packed into the fewest rows and truncated at the last
  one, and the runs sharing a strip are held to disjoint horizontal bands. Same
  strips, different text on them and different x on nearly all of them.

The current build deliberately realigns the numeric version with upstream at 34.
Upstream's v34 also changes cached layout without changing the byte structure:
word-gap suppression now applies only to tokens glued together in the source, so
spaces between Hangul words survive, and `<br>` handling now distinguishes an inline
line break from an empty-block scene break. Devices that ran this fork's earlier
version 34 must clear their section cache when installing this build; the number has
been reused for a different page layout.

Without a bump a device holding pages built by an earlier model would serve them
forever. Side-by-Side forced a bump for exactly the same reason when it landed (see
the note under [Side-by-Side](#side-by-side-two-column-layout)).

> **The number now matches upstream's, but the format does not.** This fork's header carries four fields
> upstream's has no concept of — `translationFontId`, `annotationFontId`, the
> `PtLayout` byte, and the `translatedSource` / `embeddedTranslation` pair — so an
> upstream-written `.bin` and one of ours are mutually unreadable whatever number
> either stamps. The version byte is a cache key **within this fork only**, never a
> portable format identifier and never a compatibility claim. No cross-fork cache
> sharing is possible or intended.
>
> **Do not "resume" at 42.** The next genuine format change takes this to 36.

#### Why the number went backwards, then forwards again

This line ran 33 → 41 across internal iterations that were never released. Numbers
34 through 41 were local-only history: no build carrying them left the fork, so no
user had a cache stamped with them that anyone needed to interoperate with, and a
private counter drifting further from upstream's bought nothing. They were collapsed
back into 33, the value upstream `develop` has. **Everything those iterations added
to the format is still here** — the sections below are the merged result, not a
rollback. Only the counter was rewound.

34 was then claimed again for a real change — the Interlinear line-parity rewrite —
reusing the number because nothing outside this fork ever saw the first 34, and
because resuming at 42 would preserve a gap that means nothing. 35 follows it for the
distribution fix, and is a normal forward step: a build stamped 34 *was* flashed, so
that number is spent and cannot be reused.

Every one of these invalidates caches on any device that ran an affected build; those
files are rejected as an unknown version and rebuilt once per book in the background.
That is expected. The partial sentinel moves with the version by the same derivation
(`0xFE - (version - 28)`): `0xF1` under the 34–41 iterations, `0xF9` after the
rewind, `0xF8` at the current 34, and `0xF7` at the former 35. Stale partials are
rejected on the same grounds except when a numeric version has deliberately been reused.

#### Header layout

Each file in `sections/*.bin` stores one laid-out spine section. The header is also
the cache-busting key: if any layout-affecting field differs from the current reader
settings, the section is discarded and rebuilt. In write order:

| Field | Type | Cache key |
|---|---|---|
| `version` | `u8` | yes |
| `fontId` | `s32` | yes |
| `translationFontId` | `s32` | yes (normalized) |
| `annotationFontId` | `s32` | yes (normalized) |
| `lineCompression` | `float` | yes |
| `extraParagraphSpacing` | `bool` | yes |
| `paragraphAlignment` | `u8` | yes |
| `viewportWidth` | `u16` | yes |
| `viewportHeight` | `u16` | yes |
| `hyphenationEnabled` | `bool` | yes |
| `embeddedStyle` | `bool` | yes |
| `ptLayout` | `u8` | yes |
| `translatedSource` | `bool` | yes |
| `embeddedTranslation` | `bool` | **no** — memo only |
| `imageRendering` | `u8` | yes |
| `focusReadingEnabled` | `bool` | yes |
| `pageCount` | `u16` | — |
| four LUT offsets | `u32` × 4 | — |

`embeddedTranslation` is the only field that is not part of the key.

#### Pre-Translation: the header stores a layout, not a display mode

The Pre-Translation byte holds the `PtLayout` a display mode *implies*, not the raw
mode: `0 = Both`, `1 = OriginalOnly`, `2 = TranslationOnly`, `3 = SideBySide`,
`4 = Interlinear` (see `lib/Epub/Epub/PtLayout.h`). Several display modes produce
byte-identical pages and share one cache entry: Normal and Interleaved both map to
`Both`, and Original Only / Page Translation / Tooltip all map to `OriginalOnly` (the
overlay modes composite their translation at view time, so their main flow is
original-only). Switching between two modes that share a layout is a cache hit only
when `translationFontId` also matches: Normal and Interleaved share `Both`, but
Interleaved can lay its translated words out in a smaller font, so the two are a hit
only while Interleaved's own Translation Size is "Same".

Because the byte is part of the cache key, **adding a value to the enum is itself a
format change**.

The per-chapter auto-fallback keys on the layout: a chapter with no translation in
its source is laid out and stamped as `Both`, which for an untranslated chapter is
simply the plain original (`Section::effectiveLayout`). "No translation" spans both
sources — neither a `.translated.html` sidecar nor translations embedded in the
chapter's own XHTML. So an Interlinear cache entry only exists for a chapter that
actually has a translation.

#### Interlinear line parity

Interlinear has a layout of its own and cannot share a cache entry with Normal or
Interleaved. Its rule is **line parity**: every source line of an annotated paragraph
is paired with exactly one small `LineFontRole::Annotation` strip directly above it,
so a page reads *strip, source, strip, source* all the way down — never two source
lines in a row, never two strips in a row. A strip is an ordinary `PageLine` —
nothing new is serialized per line or per page — stamped with the original
paragraph's index just as its source line is, with height
`getLineHeight(annotationFontId)`, which is what lets two type sizes tile edge to
edge on one page. No sentence metadata reaches disk: everything is resolved before
the page is written.

- **The source flows completely normally.** It is laid out exactly as
  `PtLayout::OriginalOnly` would lay it out: same DP, same hyphenation, same
  justification, same first-line indent, no constraint of any kind. Sentences do
  **not** each begin a new line. Sentence starts are handed to layout as *tracked
  words* (`ParsedText::setTrackedWords`), which constrain nothing — layout simply
  reports back, per tracked index, which emitted line it landed on, at what x, and
  whether it opened that line (`ParsedText::TrackedWordPos`). The reported line
  ordinal counts only lines that were actually emitted, so a line dropped to a
  `TextBlock` arena OOM cannot shift every later annotation by one.
- **One strip per source line, not one per sentence — and the translation is
  *distributed*, not packed.** A sentence spanning three source lines has its
  translation spread across the three strips above those three lines: the translation
  flows too, in small type, one line of it per line of source. `buildAnnotationRuns`
  gives each row a **band** — the horizontal room its own source line leaves free —
  and wraps that row at a fraction of its band chosen so the text reaches the
  sentence's last source line. Wrapping every row at the full measure instead
  (the version 34 model) filled two strips of that three-strip sentence and left the
  third an empty band, which on the page reads as two source lines with a gap between
  them; with an annotation face at ~57% of the body pitch that was the steady state of
  ordinary prose, not a corner case.
- **Sentence sync, expressed as a band rather than an indent.** A sentence's
  translation begins at the x its *source* sentence begins, not wherever the previous
  translation happened to stop. The x is read off the real laid-out word, so it
  already carries the block's left inset, its alignment, its first-line indent and any
  justification stretch of the words before it on that line — nothing is re-derived.
  Each row is laid out at its band's *width* and then placed as a whole box at the
  band's start, which is direction-agnostic: an LTR row's first glyph and an RTL row's
  last one both land on the sentence's x. (Version 34 used a `text-indent` on row 0,
  which an RTL target cannot use — see **RTL** below.)
- **A source line can carry two runs at one `yPos`, in disjoint bands.** When a line
  holds the tail of one sentence and the head of the next, its strip carries the tail
  fragment and the head fragment as two `PageLine`s at the same `yPos` with different
  `xPos`. The tail's band ends where the next *source* sentence starts and the head's
  begins there, mirroring the split the source line itself has at that x, so the two
  can never print on top of each other. That is the shape `renderSideBySide` already
  writes for its two columns, and every downstream consumer walks `page->elements`
  without assuming distinct or monotonic y. It is still one visual annotation line and
  y still advances once.
- **A blank strip is reserved, not skipped.** A strip with nothing on it emits **no**
  `PageLine` at all (no arena, no serialized bytes, no render call) and only advances
  y. Dropping it instead would put two source lines back to back and leave a source
  line without its annotation line, which is exactly what the layout exists to
  prevent. With distribution this is now the *residual* case — a translation too short
  to split across its slots, e.g. one word over three source lines — rather than the
  normal one. A paragraph with **no** annotation at all (no pairing function, an RTL
  source, an empty translation, an unpaired original) reserves no strip and flows as
  plain source.
- **Overflow stretches; it is never a second row, and it is not cut at the band.** A
  translation longer than its band takes the room it needs, up to the free width of
  its strip — it eats the space the next sentence's annotation would have had, and
  that sentence's run is pushed right off this one's ink edge rather than being
  overprinted. A second row is forbidden (it would break line parity). Text is
  therefore lost only when a translation exceeds the sentence's **entire** on-page
  region with every row already stretched to the panel edge, and that case is
  `LOG_ERR`'d rather than passing silently. The panel edge is the one hard limit:
  `GfxRenderer::drawPixel` `LOG_ERR`s every out-of-panel pixel while
  `TextBlock::render` does no x culling, so a row is pulled left rather than allowed
  to cross it. Version 34 cut the surplus at the *band* instead, which fired on every
  sentence that began and ended inside one source line — a short line-final sentence
  with a wordier translation lost most of it, and a first word too wide for the
  residual room was fallback-hyphenated first, so what survived ended in a fabricated
  `-` promising a continuation that had been discarded.
- **The pairing reads WORDS, not tokens.** `ParsedText` does not always store one token
  per word: with Focus Reading on, every word is a bold prefix plus a regular tail
  ("Ok" is `"O"` + `"k"`), and `extractLine` concatenates the tail back before a line
  leaves the layout engine. Pairing runs *before* layout and so must merge them itself
  (`buildMergedWordStream`). Without that, `"Ok."` keys as `"O k"` (3 bytes) instead of
  `"Ok"` (2), clears the junk-sentence test, and becomes a sentence boundary the
  Tooltip overlay — which reads laid-out page words — does not see. The two consumers
  `src/translator/SentencePairing.h` promises a single answer to would then disagree,
  and only when the setting is on. Annotation indices come back in merged-word space
  and are mapped to token indices before anything else uses them.
- **A sentence whose translation is empty produces no run**, so its source lines are
  absorbed into the preceding sentence's slot range and simply show blank strips.
- **Page breaking is atomic over a fixed group.** One strip plus one source line is
  tested against the viewport once, before the strip is placed, so a strip can never
  be separated from its source line. The group height is now constant, so the old
  degenerate per-row fallback is gone; the anti-loop guard is that an empty page never
  breaks.
- **RTL.** An RTL source paragraph is not annotated at all — `extractLine` permutes a
  line into visual order whenever the block resolves RTL *or* contains any RTL word, so
  a source paragraph that is RTL, or that merely carries an inline Hebrew/Arabic run,
  lays out as plain unannotated source. An RTL *target* now keeps sentence sync too:
  `extractLine` flips such a row onto its own natural margin, and because the row is
  wrapped at its **band's** width and the whole box is placed at the band's start, that
  flip puts the row's reading-order first glyph on the band's right edge — the span its
  source sentence occupies, mirrored. Version 34 could not do this: it expressed sync
  as a `text-indent`, which `resolveFirstLineIndent` discards for a Left-aligned RTL
  block (`isNaturalAlign` is false), so every RTL row was laid out against the *full*
  measure and placed at x = 0. Two runs sharing a strip were then both flush right at
  the same y, printing on top of each other — and at body size, since `he` / `ar` /
  `fa` are on `UNSUPPORTED_TARGETS` and take the body font.

Page cost is exactly `(bodyLineHeight + annotationLineHeight) / bodyLineHeight`,
about **+57% pages versus Normal** at the 14pt/8pt portrait default. Unlike the
earlier sentence-per-line model it does **not** degrade with short-sentence prose,
because no sentence ends its line early any more.

##### What this replaced

Two earlier iterations shipped and were wrong about the *model*, not the code:

1. A hanging-indent scheme: the first row of a sentence was inset to the x of the
   sentence's first word, continuation rows fell back to the margin, and the indent
   was dropped past 3/4 of the measure. Several row-sets stacked over one line while
   neighbouring lines carried none.
2. A sentence-per-line scheme: sentence starts were fed into line breaking as **hard
   constraints** (`ParsedText::setForcedLineBreaks`) so every sentence began its own
   line, and its whole translation was emitted as a variable-height stack of rows
   above that line. That broke ordinary paragraph flow, left every sentence ending on
   a short ragged line (justification was suppressed there to stop it stretching), and
   still gave continuation lines no annotation at all.

Both are gone. `setForcedLineBreaks`, `isForcedBreakAt`, `nextForcedBreakAfter`,
the DP's forced-run cost case and the per-run justification suppression were all
deleted from `ParsedText`; only the *reporting* half survives, widened into
`setTrackedWords` / `TrackedWordPos`.

#### `translationFontId` and `annotationFontId`

Both are `s32`, written immediately after `fontId`, and both are genuine layout
inputs conditionally normalized to `0` so a font that cannot reach the page under the
current layout does not pointlessly invalidate the cache. The lookup normalizes
identically (`Section::keyedTranslationFontId` / `keyedAnnotationFontId`, applied at
the header write, the lookup compare and the id handed to the parser), so key and
layout can never disagree.

- **`translationFontId`** is the font translated text is laid out in (`0` = same as
  the body font), resolved from the *Interleaved* mode's own size setting; the Tooltip
  and Page Translation sizes are separate settings, composited at view time, and never
  reach this field. A distinct font changes word measurement and hence line breaking.
  It is stamped `0` for every layout other than `Both`: `OriginalOnly` drops every
  translated word, and `TranslationOnly` / `SideBySide` keep them but lay them out in
  the body font by design, so in all three no line is ever measured in it. Under `Both`
  the real id is always kept, including for a chapter with no translation —
  `Section::effectiveLayout` maps that case to `Both` too, and the chapter's own HTML
  can still contain a block whose `lang=` differs from the book's language, which IS
  laid out in this font when it is non-zero.
- **`annotationFontId`** is the font the small annotation rows are laid out in (`0` =
  same as the body font), from `CrossPointSettings::getInterlinearAnnotationFontId()`.
  It decides both how an annotation row wraps and how tall it is. It is stamped `0`
  whenever the effective layout is anything other than `Interlinear`, since no other
  layout emits an annotation row. Without that normalization, an annotation-size
  setting would invalidate every cached chapter of every mode. `0` is also the graceful
  answer when the annotation face cannot cover the selected target script: the rows
  still appear above their sentences, at body size.

#### Per-line font role

`PageLine` serializes a `LineFontRole` byte (`0 = Body`, `1 = Translation`,
`2 = Annotation`) immediately after `paragraphIdx`, so one page can mix the body font
with smaller translated or annotation text. The role is chosen at layout time, which is
also when the line was measured, so a cached page always redraws in the font it was
measured with. The renderer resolves the role through a `PageFontSet` supplied by the
app (`lib/Epub` stores roles, never font ids). A line is tagged `Translation` only
under the `Both` layout, and only when its enclosing block is translated and
`translationFontId` is non-zero (every mode but Interleaved stamps `0`, so their pages
are all-`Body`). `Annotation` is emitted under the `Interlinear` layout only.

#### `translatedSource` and `embeddedTranslation`

- **`translatedSource`** is a 1-byte `bool` immediately after the `PtLayout` byte. It
  records whether the HTML these pages were laid out from **contained translations** —
  from *either* source: a reader-produced `.translated.html` sidecar, or translations
  embedded in the chapter's own XHTML. A book translated by a Calibre plugin has no
  sidecar at all: its translated paragraphs are embedded in the chapter's own XHTML
  (`<p lang="uk">` beside the `lang="en"` original), with no marker attribute — the
  language tag is the whole signal. The layout byte cannot express this: `Both` is
  stamped both by an untranslated chapter and by one that simply requested Normal. So
  without this flag a chapter laid out before its translation was downloaded would stay
  a cache **hit** afterwards and silently serve untranslated pages in a bilingual mode,
  and symmetrically a translated cache would survive the translation being deleted.
- **The language comparison** is `translationdetect::isTranslatedLangTag`
  (`lib/Epub/Epub/TranslationDetection.h`): primary subtag, ASCII case-insensitive,
  with `-` and `_` both ending the subtag, so `uk-UA` in an `en` book is translated
  while `en-GB` is not. It is never class, style, colour or `dir` — those are one
  plugin's presentation choices. A book with no `<dc:language>` answers "not
  translated", which degrades to `PtLayout::Both` and renders the full text. The
  predicate is shared with `ChapterHtmlSlimParser`, so the gate that enables a bilingual
  layout and the engine that renders it can never disagree.
- **`embeddedTranslation`** is a 1-byte `bool` immediately after `translatedSource`,
  and is **not** part of the cache key. It records only the half of `translatedSource`
  that is *immutable for a given book file* — "the chapter's own XHTML embeds translated
  blocks" — so a load can recompute `translatedSource` as
  `hasTranslatedSidecar() || embeddedTranslation`, one SD stat, instead of SAX-scanning
  the chapter HTML on every chapter load (a cost that would fall on every reader,
  including those who never enable a translation mode). A build that read the *sidecar*
  never looked at the chapter HTML and stamps `false` without knowing; that state is
  recognisable as `translatedSource == true && embeddedTranslation == false`, and a load
  that sees it with the sidecar now gone treats the answer as unknown — it forces the
  (required) rebuild and lets the rebuild re-scan, rather than memoizing a value that
  could understate the truth.

The invariant: a chapter cached while it had no translation must never be served once
it has one, and vice versa. Both transitions invalidate — a downloaded or deleted
sidecar flips the byte, and an embedded translation is baked into the chapter HTML, so
a book that gains one is a different file with a different cache directory.

#### Ruby, block splits and the word style byte

- **Native `<ruby>` / `<rt>`.** `TextBlock` serializes a per-word ruby annotation string
  array immediately after the word arena (see the `TextBlock` pattern below). `<rp>`
  fallback parentheses are skipped at parse time. The array is written unconditionally —
  one length-prefixed `String` per word, empty for words without ruby — so every book
  pays 4 bytes per word even with no ruby present.
- **Closed-tag block splits.** A closing block tag starts a fresh text block, so a
  closed block's style does not leak into following bare text.
- **The ruby word-style bit is 128 (bit 7),** not upstream's `64`: `64` is this fork's
  `TRANSLATED` bit (Pre-Translation). The reservation bit 7 previously carried for the
  Tooltip display mode is retired — that flag was never written nor persisted. The word
  style byte is now **full**: a further flag requires widening the persisted style (the
  `TextBlock` arena stores `styles[]` as `uint8_t`) plus a version bump.

#### Lazy image extraction

`ImageBlock` serializes the book-internal source href (`srcPath`) after the cache path,
so images are header-probed at build time and extracted from the EPUB on first render.

#### Side by Side

Side-by-Side lays original and translation paragraphs into two half-width columns
instead of full-width sequential blocks. The two columns are emitted as lockstep
`PageLine` rows — the left line at `xPos = 0` and the right line at `xPos = rightColX`,
both sharing one `yPos` — reusing the existing per-line `xPos` field, so no new fields
are added. An inline "not translated" marker is laid out after originals that have no
paired translation. See [Side by Side](./pre-translation.md#side-by-side).

#### Two rules this history paid for

1. **Never change the byte layout without changing the number, even if a mismatch check
   "would catch it".** Two different header layouts were once written under the number
   38 during development — first without `translatedSource`, then with it. That byte
   sits in the *middle* of the header, so the earlier layout passes a version-38 gate
   and then every field after the `PtLayout` byte is read shifted by one, including the
   `pageCount` and LUT offsets, which are consumed *before* the parameter-mismatch check
   could reject the entry. The check usually catches it, but it is not guaranteed to —
   shifted bytes can compare equal. **Version 38 was never a stable layout and nothing
   may claim to read it.** Any mid-header insertion makes a bump mandatory rather than
   merely correct.
2. **A pure layout change with no byte change still needs a bump,** because the version
   *is* the cache key and nothing else in the key moved. A device holding the old
   entries would otherwise serve the old pages forever. Note this invalidates every
   cached chapter of every book for *every* layout, not just the one that changed — the
   key is a single number, so the blast radius is always total. That is the accepted
   cost: one background re-layout per book on next open.

### Earlier history — the fork's original 28–33 numbering

> **The "version 33" in this section is not the 33 this fork last shipped.** It is
> the fork's *original* 33 (per-block hyphenation), long superseded. The number was
> reused when 34–41 were collapsed, and the current format now uses 34; these
> notes are kept for archaeology only and describe formats no current firmware
> reads.

#### Original version 33

Version 33 is binary-identical to version 32 in layout; it was bumped because
per-block hyphenation changed the cached line breaks. Translated blocks (a
`lang=` attribute differing from the book language) now hyphenate with their own
script's rules instead of the single book-wide hyphenator, so a bilingual book
(e.g. an en->uk translation) finally breaks its translated words. Hyphenated
splits are baked into the serialized pages, so sections cached under v32 stored
English-only splits on Cyrillic text and must be regenerated.

Version 32 adds the Pre-Translation feature: a `translationMode` byte in the
header (part of the cache key), a per-line `paragraphIdx`, and a per-page
paragraph range (`firstParagraphIdx` / `lastParagraphIdx`). These let the
reader map rendered lines back to their originating source paragraphs and
dim/pair translated text according to the selected display mode.

Version 31 is binary-identical to version 30; it was bumped because CJK word
continuation across `MAX_WORD_SIZE` splits changed cached word grouping.
Version 30 is binary-identical to version 29. The version was bumped because
Arabic contextual shaping changed text measurement (`getTextAdvanceX` now
measures the shaped visual text), so word positions cached by v29 no longer
match what `drawText` renders.

Version 28 introduced serialized word style bits for underline, strikethrough,
superscript, and subscript. The format also includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Focus Reading split metadata
- per-page footnote entries
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage (v29): per-word arrays plus one shared
  NUL-terminated text blob, replacing v28's length-prefixed word strings. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

### ImHex pattern

Describes the **current** format (version 34 as defined above, not the original 33
in the archaeology section; the byte layout is identical between the two).

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 33
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PtLayout : u8 {
    Both = 0,
    OriginalOnly = 1,
    TranslationOnly = 2,
    SideBySide = 3,
    Interlinear = 4
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32,
    TRANSLATED = 64,
    RUBY_CONTINUE = 128
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
        String rubyText[wordCount] [[comment("Per-word ruby annotation, empty when the word has none")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String srcPath [[comment("Book-internal source href; extracted on first render")]];
    s16 width;
    s16 height;
};

enum LineFontRole : u8 {
    Body = 0,
    Translation = 1,
    Annotation = 2
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
    s16 paragraphIdx [[comment("Pre-Translation: source paragraph index; -1 = unset")]];
    LineFontRole fontRole [[comment("Which role's font this line draws in")]];
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    s16 firstParagraphIdx [[comment("Pre-Translation: first source paragraph on this page; -1 = none")]];
    s16 lastParagraphIdx [[comment("Pre-Translation: last source paragraph on this page; -1 = none")]];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    s32 translationFontId [[comment("Font translated text is laid out in; 0 = same as fontId")]];
    s32 annotationFontId [[comment("Font the Interlinear annotation rows are laid out in; 0 = same as fontId; non-zero only under PtLayout::Interlinear")]];
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    PtLayout ptLayout [[comment("Pre-Translation page layout (NOT the display mode); part of the cache key")]];
    bool translatedSource [[comment("Laid out from content containing translations - a .translated.html sidecar OR translations embedded in the chapter's own XHTML; part of the cache key")]];
    bool embeddedTranslation [[comment("The chapter's own XHTML embeds translated blocks; a memo, NOT a cache key - lets a load recompute translatedSource with one sidecar stat instead of re-scanning the HTML. False also means 'this build read the sidecar and never looked'")]];
    u8 imageRendering;
    bool focusReadingEnabled;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## Translated chapter HTML (`sections/<n>.translated.html`)

When a chapter is translated on the device (or a book ships already bilingual), the
translated chapter is written as a plain XHTML sidecar next to the cached section,
named `sections/<spineIndex>.translated.html` inside the book's
`.crosspoint/epub_<hash>/` directory. Translated paragraphs carry a `lang` /
`xml:lang` attribute whose value differs from the book's primary language (declared in
`content.opf`); the layout parser treats those blocks as translations and pairs, dims,
or filters them according to the active **Translation Mode** — more precisely, according to
the `PtLayout` that mode maps to (the layout byte in the `section.bin` header). See the
[Pre-Translation guide](./pre-translation.md#how-it-stores-translations).

The reader prefers this sidecar over re-extracting the original chapter from the EPUB,
so translated content **survives layout-cache invalidation**: changing the font, size,
or margins deletes the `.bin` layout cache and rebuilds it from the already-translated
HTML, without re-contacting the network. Deleting a book's cache directory (or the
specific `.translated.html`) removes the translation.

### Atomic commit (`.translated.html.part`)

The sidecar is committed atomically. The translator writes to
`sections/<spineIndex>.translated.html.part` and only renames it into place once a
clean, complete write has finished. Consequences:

- A finished translation is exactly "the final file exists" —
  `Section::hasTranslatedSidecar()` is a plain existence check on the final path. A power
  loss mid-translation leaves only a `.part`, never a truncated final file, so the
  reader never lays out from a partial.
- `Section::createSectionFile()` builds from the translated source only when
  `hasTranslatedSidecar()` is true, never from a `.part`.
- That sidecar check answers "which file does the build parse", **not** "does this chapter
  have a translation". The latter is `Section::hasTranslation()`, which is also true for a
  chapter whose translations are embedded in its own XHTML (see the current version above); it is
  what gates the display-mode fallback and what is stamped as `translatedSource`.
- A leftover `.part` from an interrupted run is transient; `Section::clearCache()`
  reclaims it on the next `.bin` invalidation, while the completed final file is
  preserved across that invalidation.

### Side-by-Side two-column layout

`section.bin` layouts built in **Side by Side** mode (`PtLayout::SideBySide`) place each
original paragraph and its paired translation into two half-width columns: the original
in the left column (`xPos = 0`), the translation in the right column (`xPos = rightColX`,
where `rightColX = colWidth + gapWidth`, `gapWidth = viewportWidth * 0.04`, and
`colWidth = (viewportWidth - gapWidth) / 2`). The parser buffers the original block and,
when its paired translation arrives, lays both out at `colWidth` and emits them as lockstep
`PageLine` rows: the left and right lines of each row share one `yPos`, and `yPos` advances
one line-height per row (see [Side by Side](./pre-translation.md#side-by-side)). This rides
entirely on the existing per-line `xPos` field, so the serialized `Page`/`PageLine` structure
is unchanged; the layout difference on its own is what forced a `SECTION_FILE_VERSION` bump
when it landed (the header cache key was identical across that change, since the
Pre-Translation header byte kept the same value).

An original paragraph that has no paired translation renders full-width, with a short,
dimmed `tr(STR_NO_TRANSLATION)` marker appended inline after its source text so the gap is
visible. The marker words reuse the existing per-word `TRANSLATED` style bit for dimming and
add no new fields. Columns are never mirrored for RTL — the original always occupies the
left column — though per-word RTL within each half-width line is handled normally by the
line layout.
