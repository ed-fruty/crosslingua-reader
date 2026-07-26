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

### Version 41

Version 41 changes **no bytes**. The header, the page records, the line records and
every LUT are laid out exactly as in version 40, and a version 40 file is still
structurally decodable by the pattern below — only the `EXPECTED_VERSION` guard
rejects it.

The bump exists because the version number *is* the cache key, and under
`PtLayout::Interlinear` the pages it keys are no longer the same pages:

- **A source sentence now always begins a new line.** Sentence starts are resolved
  *before* line breaking and fed into it as hard constraints
  (`ParsedText::setForcedLineBreaks`), instead of being discovered afterwards over the
  already-broken word array. A sentence that wraps flows normally onto the lines below
  it.
- **An annotation row starts where its source line starts, directly above it.** Not
  "at the left margin": that is only the same thing for a left-aligned or justified
  paragraph whose sentence is not the first. Three things place the row, all taken from
  the *source* block so the two cannot drift:
  - the block's left inset (unchanged — `emitInterlinearRow`);
  - the block's **alignment**. A `text-align:center` epigraph or a `text-align:right`
    verse block gets centred / right-aligned rows, so the pair shares a centre line or
    a right edge. `Justify` degrades to left, which is where a justified line starts
    anyway, because a row this small stretched to the full measure would open gaping
    word gaps;
  - the **first-line indent**, on the rows over source line 0 *only*
    (`ParsedText::firstLineIndent`, the value `extractLine` itself used). The
    paragraph's first source line is indented — three spaces when extra paragraph
    spacing is off, the CSS `text-indent` when one is set, a negative hanging indent
    verbatim — so a row hard-zeroed to the margin left the *first* pair of every
    paragraph out of line while every later pair matched. This is also what keeps
    annotation rows on the body's own rule: indent when extra paragraph spacing is
    off, no indent (and a paragraph gap instead) when it is on.

  The version 40 hanging indent — first row inset to the x of the sentence's first
  *word*, continuation rows falling back to the margin, indent dropped entirely past
  3/4 of the measure — **is gone**, and the two version 40 bullets describing it below
  are superseded. That scheme produced several row-sets stacked over one line while
  neighbouring lines carried none, and parked a shard of translation against the right
  margin whenever a sentence started late.
- **The pairing reads WORDS, not tokens.** `ParsedText` does not always store one token
  per word: with Focus Reading on, every word is a bold prefix plus a regular tail
  ("Ok" is `"O"` + `"k"`), and `extractLine` concatenates the tail back before a line
  leaves the layout engine. Version 40 paired over post-layout lines and so saw whole
  words for free; version 41 pairs *before* layout and must merge them itself
  (`buildMergedWordStream`). Without that, `"Ok."` keys as `"O k"` (3 bytes) instead of
  `"Ok"` (2), clears the junk-sentence test, and takes an annotation row and a forced
  line break that the Tooltip overlay — which reads laid-out page words — does not give
  it. The two consumers `src/translator/SentencePairing.h` promises a single answer to
  would then disagree, and only when the setting is on. Annotation indices come back in
  merged-word space and are mapped to token indices before anything else uses them.
- **The last line of every sentence is not justified.** It is now usually short, since
  the following sentence no longer fills it; stretching it to the full measure would
  open gaping word gaps. This reuses the rule the paragraph's own last line already had
  (`ParsedText::extractLine`).
- **A sentence whose translation is empty produces no row and no forced break**, and
  simply continues on the previous sentence's last line — there is no row that could be
  mispaired, and the next translated sentence still starts a line of its own.
- **A sentence opening on a continuation token** (one glued to the word before it) is
  the one case the constraint cannot honour: no line breaker may break there. That
  sentence keeps the version 40 placement — its rows sit above the line that *contains*
  its start — but at the margin, not anchored.
- **RTL is unchanged.** An RTL source paragraph is still not annotated at all — which is
  also what keeps the inherited alignment above safe, since a right-aligned *source* is
  only ever an LTR block with explicit `text-align:right`. An RTL *target* still lays
  its rows out at the full measure on their own natural margin (flush right): the
  RTL-flip in `extractLine` fires only on the left branch, so an inherited `Center` or
  `Right` survives it (and both are already correct for an RTL row), while a left row
  flips to flush right and takes no indent, as before.

Page cost rises accordingly: each sentence ends with roughly half a line of white
space, so Interlinear is now about **+70% pages versus Normal** at the 14pt portrait
default (was ~+40%), and more for dialogue-heavy prose.

### Version 40

Version 40 adds a fifth `PtLayout` value and one header field. Either change alone
would force the bump; together they also shift every field after the new one, which
is exactly the failure mode the version 39 note below was written about.

- **`PtLayout` gains `4 = Interlinear`.** The Interlinear display mode no longer maps
  to `Both`: it now has a layout of its own, so its pages genuinely differ and cannot
  share a cache entry with Normal or Interleaved. The source flows full-width and
  breaks exactly as it does under Original Only, and each sentence's translation is
  emitted as its own `LineFontRole::Annotation` row directly *above* the source line
  that sentence starts on. Because the byte is part of the cache key, adding a value
  to it requires a version bump on its own (`lib/Epub/Epub/PtLayout.h`).
- **`annotationFontId`** is added as an `s32` immediately after `translationFontId`.
  It is the font the small annotation rows are laid out in (`0` = same as the body
  font), resolved from `CrossPointSettings::getInterlinearAnnotationFontId()`. It is a
  genuine layout input — it decides both how an annotation row wraps and how tall the
  row is — so it belongs in the cache key for the same reason `translationFontId`
  does, and it is normalized the same way: stamped as `0` whenever the (effective)
  layout is anything other than `Interlinear`, since no other layout emits an
  annotation row and therefore no line under `Both` / `OriginalOnly` /
  `TranslationOnly` / `SideBySide` is ever measured in it. Without that
  normalization, a future annotation-size setting would invalidate every cached
  chapter of every mode. The lookup normalizes identically
  (`Section::keyedAnnotationFontId`, applied at the header write, the lookup compare
  and the id handed to the parser), so key and layout can never disagree. `0` is also
  the graceful answer when the annotation face cannot cover the selected target
  script: the rows still appear above their sentences, at body size.
- **`LineFontRole::Annotation` is now actually emitted.** An annotation row is an
  ordinary `PageLine` — nothing new is serialized per line or per page. It is
  left-aligned, laid out at the *full* text measure with a hanging indent (the first
  row's text-indent is the x of the sentence start, so continuation rows fall back to
  the margin — **superseded by version 41, which always uses `text-indent = 0`**),
  placed at `xPos` = the block's left inset, and stamped with the
  original paragraph's index just as its source line is. Its height is
  `getLineHeight(annotationFontId)`, which is what lets two type sizes tile edge to
  edge on one page (see the per-line font role bullet under version 38). No sentence
  metadata reaches disk: alignment is fully resolved before the page is written.
  - **The indent is dropped, not clamped, for a sentence that starts very late.**
    *(Superseded by version 41: there is no indent at all.)* A
    first row needs a usable measure — `computeLineBreaks` force-hyphenates any word
    wider than the width left to it — so a sentence beginning past 3/4 of the measure
    gets `text-indent = 0` and its row sits at the margin. That is deliberate: a row
    parked at the 3/4 limit would sit above words belonging to the *previous*
    sentence and read as their translation, whereas a row at the margin reads as
    "this whole line". Everything up to 3/4 is anchored exactly.
  - **An RTL source paragraph is not annotated at all, and an RTL target is not
    anchored.** *(Version 41: the second half is moot — nothing is anchored any more.)*
    `extractLine` permutes a line into visual order whenever the block
    resolves RTL *or* contains any RTL word, and the anchoring reads flat post-layout
    word indices — so a source paragraph that is RTL, or that merely carries an inline
    Hebrew/Arabic run, lays out as plain unannotated source. In the other direction, a
    *target* language written right-to-left produces rows that resolve RTL themselves;
    those get no `text-indent` (the engine only honours one on a naturally aligned
    block) and are laid out at the full measure on their own natural margin, i.e.
    flush right. Anchoring them would need an end-side first-line inset, which
    `BlockStyle` cannot express, so v1 does not attempt it.

The per-chapter auto-fallback applies to `Interlinear` too: a chapter with no
committed translation is laid out and stamped as `Both`
(`Section::effectiveLayout`), so an Interlinear cache entry only exists for a
chapter that actually has a translation.

### Version 39

Version 39 adds one field to the version 38 header:

- **`translatedSource`** is added as a 1-byte `bool` immediately after the
  `PtLayout` byte. It records *which source HTML* the pages were laid out from:
  `true` = the chapter's `.translated.html` sidecar, `false` = the plain
  original chapter HTML. The layout byte cannot express this — `Both` is what an
  untranslated chapter stamps *and* what an inline-bilingual chapter stamps — so
  without it a chapter laid out before its translation was downloaded would stay
  a cache **hit** afterwards and silently serve untranslated pages in a
  bilingual mode (and, symmetrically, a translated cache would survive the
  translation being deleted). Both halves are compared on load.

The per-chapter auto-fallback keys on the layout too: a chapter with no
committed translation is laid out and stamped as `Both`, which for an
untranslated chapter is simply the plain original (see
`Section::effectiveLayout`) — and its `translatedSource` is `false`, so the
entry stops matching the moment a translation lands.

**Version 38 was never a stable layout and nothing may claim to read it.** The
`translatedSource` byte was originally added *within* 38, so two different header
layouts exist under that number: the earlier one without the byte, the later one
with it. Because the byte sits in the middle of the header, an early-layout 38
passes a version-38 gate and then every field after the `PtLayout` byte is read
shifted by one — including the `pageCount` and LUT offsets, which are consumed
before the parameter-mismatch check could reject the entry. Version 39 exists so
that the shorter layout is rejected on its version number alone.

### Version 38

Version 38 changes what the Pre-Translation byte in the header *means*, and adds
one field:

- **The header stores a layout, not a display mode.** The byte that held the raw
  `translationDisplayMode` now holds the `PtLayout` that mode implies:
  `0 = Both`, `1 = OriginalOnly`, `2 = TranslationOnly`, `3 = SideBySide` (see
  `lib/Epub/Epub/PtLayout.h`; version 40 adds `4 = Interlinear`). Several display
  modes produce byte-identical pages and now share one cache entry: Normal and
  Interleaved both map to `Both` (Interlinear did too until version 40 gave it its
  own value), and Original Only / Page Translation / Tooltip all map to
  `OriginalOnly` (the overlay modes composite their translation at view time, so
  their main flow is original-only). Switching between two modes that share a
  layout is a cache hit only when `translationFontId` (below) also matches:
  Normal and Interleaved share `Both`, but Interleaved can lay its translated
  words out in a smaller font than Normal's, so the two are a cache hit only
  while Interleaved's own Translation Size is "Same".
  The byte occupies the same header slot as the old mode byte but is *not* the
  same value space, so v37 files must be rejected rather than reinterpreted.
- **`translationFontId`** is added as an `s32` immediately after `fontId`. It is
  the font translated text is laid out in (`0` = same as the body font),
  resolved from the *Interleaved* mode's own size setting; the Tooltip and Page
  Translation sizes are separate settings, composited at view time, and never
  reach this field. A distinct font changes word measurement and hence line
  breaking, so it belongs in the cache key. It is stamped as `0` whenever the
  (effective) layout is anything other than `Both` — `OriginalOnly`,
  `TranslationOnly`, or `SideBySide`: `OriginalOnly` drops every translated word,
  and `TranslationOnly` / `SideBySide` keep them but always lay them out in the
  body font by design, so in all three no line is ever measured in the
  translation font. Under `Both` the real id is always kept, including for a
  chapter with no committed translation — `Section::effectiveLayout` maps that
  case to `Both` too, and the chapter's own original HTML can still contain a
  block whose `lang=` differs from the book's language (see per-line font role,
  below), which IS laid out in this font when it is non-zero. The lookup
  normalizes the same way, so the two can never disagree.
- **Per-line font role.** `PageLine` serializes a `LineFontRole` byte
  (`0 = Body`, `1 = Translation`, `2 = Annotation`) immediately after
  `paragraphIdx`, so one page can mix the body font with smaller translated or
  annotation text. The role is chosen at layout time, which is also when the
  line was measured, so a cached page always redraws in the font it was
  measured with. The renderer resolves the role through a `PageFontSet`
  supplied by the app (`lib/Epub` stores roles, never font ids). A line is
  tagged `Translation` only under the `Both` layout, and only when its
  enclosing block is translated (a `lang=` mismatch against the book's primary
  language — the same block-level flag per-block hyphenation already uses) and
  `translationFontId` is non-zero (every mode but Interleaved stamps `0`, so
  their pages are all-`Body`, unaffected by this bullet). `Annotation` was
  reserved here and is emitted from version 40 on, under the `Interlinear`
  layout only.

The per-chapter auto-fallback keys on the layout too: a chapter with no
committed translation is laid out and stamped as `Both`, which for an
untranslated chapter is simply the plain original (see
`Section::effectiveLayout`).

### Version 37

Version 37 is the second reconciliation of the two numbering lines. While this
branch stood at 36, upstream `develop` had advanced its own line to its "v33",
which bundles two cache-invalidating changes — both kept here:

- **Native `<ruby>` / `<rt>`.** `TextBlock` now serializes a per-word ruby
  annotation string array immediately after the word arena (see the `TextBlock`
  pattern below). `<rp>` fallback parentheses are skipped at parse time. The
  array is written unconditionally — one length-prefixed `String` per word, empty
  for words without ruby — so every book pays 4 bytes per word even with no ruby
  present.
- **Closed-tag block splits.** A closing block tag now starts a fresh text block,
  so a closed block's style no longer leaks into following bare text. This shifts
  cached block boundaries for essentially every book.

Version 37 also **renumbers the ruby word-style bit**. Upstream shipped
`EpdFontFamily::RUBY_CONTINUE` as `64`, which is this branch's `TRANSLATED` bit
(Pre-Translation). Ruby therefore moves to bit 7 (`128`), and the reservation
that bit previously carried for the Tooltip display mode is retired — that flag
was never written nor persisted by any code. The word style byte is now full: a
further flag requires widening the persisted style (the `TextBlock` arena stores
`styles[]` as `uint8_t`) plus another version bump.

Because v37 rejects every cache written by either line, no stored style byte is
ever reinterpreted under the new numbering. The merged format carries the
Pre-Translation fields (v32–v35), the `ImageBlock` `srcPath` (upstream's "v32")
**and** the per-word ruby strings (upstream's "v33").

### Version 36

Version 36 reconciles two lines that had independently numbered their own
formats: this branch used 32–35 for the Pre-Translation fields (below), while
upstream `develop` used its own "v32" for lazy image extraction — `ImageBlock`
now serializes the book-internal source href (`srcPath`) after the cache path,
so images are header-probed at build time and extracted from the EPUB on first
render. The two "v32+" layouts are mutually unreadable, so v36 supersedes both:
the merged format carries the Pre-Translation fields **and** the `srcPath`
string, and the bump forces a rebuild of sections cached by either line.

### Version 35

Version 35 is binary-identical to version 34 in structure; it was bumped because
Side-by-Side mode (`translationMode == 5`) now lays out original and translation
paragraphs into two half-width columns instead of full-width sequential blocks.
The two columns are emitted as lockstep `PageLine` rows — the left line at
`xPos = 0` and the right line at `xPos = rightColX`, both sharing one `yPos` —
reusing the existing per-line `xPos` field, so no new fields are added. A mode-5
section cached under v34 carries an identical header cache key (`translationMode`
is still `5`) and would otherwise be served with the old single-column layout, so
the version bump is what forces those sections to rebuild. See
[Side by Side](./pre-translation.md#side-by-side).

### Version 34

Version 34 is binary-identical to version 33 in layout; it was bumped because
Side-by-Side mode now lays out an inline "not translated" marker after
originals that have no paired translation, changing the cached line content
for bilingual sections built by earlier versions.

### Version 33

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

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

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 41
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
        String rubyText[wordCount] [[comment("v37: per-word ruby annotation, empty when the word has none")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String srcPath [[comment("v36: book-internal source href; extracted on first render")]];
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
    LineFontRole fontRole [[comment("v38: which role's font this line draws in")]];
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
    s32 translationFontId [[comment("v38: font translated text is laid out in; 0 = same as fontId")]];
    s32 annotationFontId [[comment("v40: font the Interlinear annotation rows are laid out in; 0 = same as fontId; non-zero only under PtLayout::Interlinear")]];
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    PtLayout ptLayout [[comment("v38: Pre-Translation page layout (NOT the display mode); part of the cache key")]];
    bool translatedSource [[comment("v39: laid out from the .translated.html sidecar (true) or the original chapter HTML (false); part of the cache key")]];
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
  `Section::hasTranslatedHtml()` is a plain existence check on the final path. A power
  loss mid-translation leaves only a `.part`, never a truncated final file, so the
  reader never lays out from a partial.
- `Section::createSectionFile()` builds from the translated source only when
  `hasTranslatedHtml()` is true, never from a `.part`.
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
is unchanged; the layout difference is what forced the `SECTION_FILE_VERSION` bump to v35
(the header cache key was identical across that change, since the Pre-Translation header byte
kept the same value).

An original paragraph that has no paired translation renders full-width, with a short,
dimmed `tr(STR_NO_TRANSLATION)` marker appended inline after its source text so the gap is
visible. The marker words reuse the existing per-word `TRANSLATED` style bit for dimming and
add no new fields. Columns are never mirrored for RTL — the original always occupies the
left column — though per-word RTL within each half-width line is handled normally by the
line layout.
