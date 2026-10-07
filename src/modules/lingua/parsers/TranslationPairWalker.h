#pragma once

#include <cstddef>
#include <string>

// ── Translation pair walker ──────────────────────────────────────────────────
//
// ONE place that reads a chapter's HTML and hands back its (original, translation) paragraph pairs,
// shared by both Lingua overlay display modes.
//
// It deliberately does NOT number paragraphs. The two overlays used to carry a copy each of
// ChapterHtmlSlimParser's paragraph-counting rules so they could ask "which HTML paragraph is index
// N", and those copies drifted from the parser: upstream made tables render as columns (0 indices
// where the copies counted 1), made an <li> with a nested block reuse its bullet block (1 where they
// counted 2), and classified a <br>-opened block by its first word (0 for a Calibre translation
// where they counted 1). A drifted index attached a translation to the wrong paragraph or to none,
// which is what surfaced as source text plus a spurious "not translated" marker.
//
// Removing the counter removes that whole class of bug rather than pinning it with a test: the
// overlays match a pair to a paragraph by its TEXT against the words the page actually shows, and
// the page's own PageLine::paragraphIdx (stamped by the parser, cached in section.bin) stays the
// only paragraph numbering in the system.
//
// ── The pairing rule ─────────────────────────────────────────────────────────
//
// Both shapes in the wild fall out of one rule, applied per container block:
//
//   SIBLING (what CrossPoint's own translator writes, and Calibre's block mode):
//     <p>Original.</p><p lang="uk">Переклад.</p>
//     The first block yields original text only and is held; the second yields translation text
//     only and pairs with it.
//
//   INLINE (Calibre's <br/><span> mode):
//     <p>Original.<br/><span lang="uk">Переклад.</span></p>
//     One block yields both, so it pairs with itself.
//
// "Is this a translation" is asked with lingua::content::isTranslatedLangTag against the book's
// primary language -- the SAME predicate ChapterHtmlSlimParser uses (it reads `lang` / `xml:lang`
// and skips html/body). The old copies treated ANY lang attribute as a translation marker, so a
// `<p lang="en">` inside an `en` book was wrongly read as translated; this does not.
//
// An original with no translation is simply never emitted: the overlays already derive untranslated
// source text from the page itself.
namespace lingua {

// One pair, valid ONLY for the duration of the callback -- the walker reuses its buffers.
struct TranslationPair {
  const char* original = nullptr;
  size_t originalLength = 0;
  const char* translation = nullptr;
  size_t translationLength = 0;
};

// Return false to stop the walk early, e.g. once every paragraph the page shows has been matched.
using TranslationPairSink = bool (*)(void* ctx, const TranslationPair& pair);

// Walks the HTML at `htmlPath` -- a reader-written `.translated.html` sidecar, or the chapter's own
// XHTML for a book whose translation is embedded -- and calls `sink` for every pair in document
// order. `bookPrimaryLang` is the book's content.opf language; an empty one means nothing can be
// recognized as translated, so no pair is emitted (the same answer the parser gives).
//
// Peak heap is one paragraph's original plus one translation, both capped (see MAX_RUN_BYTES in the
// .cpp), so a long chapter costs no more than a short one. Returns false only when the file could
// not be read or parsed; an early stop from the sink is a success.
bool walkTranslationPairs(const std::string& htmlPath, const std::string& bookPrimaryLang, void* ctx,
                          TranslationPairSink sink);

// ── Matching a page's paragraphs to their pairs ───────────────────────────────
//
// What both overlays actually want: for each paragraph the PAGE shows, the pair whose original that
// paragraph is a slice of. Shared so the matching rule -- not just the walk -- has one
// implementation.
//
// The match is a SUBSTRING search, not a prefix one: a paragraph broken across a page edge shows
// only its middle, so its visible words appear somewhere inside the full original. `matchOffset` is
// where they were found, which is also the answer to "does this paragraph BEGIN on this page"
// (offset 0) -- the indent depends on it and nothing else can tell.
//
// Matching walks both sides in document order and never goes backwards, so several paragraphs with
// IDENTICAL text pair with their own translation by position rather than by content. A visible
// paragraph that matches nothing (an untranslated one, or a table's row text) is simply left
// unmatched and skipped over; it does not block the ones after it.
struct PageMatch {
  bool matched = false;
  size_t matchOffset = 0;   // offset of the visible slice inside `original`
  std::string original;     // the FULL original paragraph, needed for the sentence window
  std::string translation;  // the FULL translation, which the caller then slices
};

// `visibleTexts` are the page's paragraphs in reading order, each its words joined by single spaces.
// Fills `out` (same length) and returns false only when the HTML could not be read. The walk stops
// as soon as every paragraph has matched, so a page early in a long chapter does not pay for the
// rest of it.
bool matchPageParagraphs(const std::string& htmlPath, const std::string& bookPrimaryLang,
                         const std::string* visibleTexts, int count, PageMatch* out);

// ── The pair cache ───────────────────────────────────────────────────────────
//
// A chapter's pairs do not change, so walking its HTML for every page is wasted work: the walk
// starts at byte 0 and the deeper into the chapter the page is, the more markup it re-parses.
//
// The cache is a MEMO OF THIS WALKER'S OUTPUT -- the same pairs, pre-parsed, written once per
// chapter. It is deliberately not a second way of deriving them: whatever it returns, the walker
// returned first. That is the difference between a cache and the duplicated logic that caused the
// drift bug in the first place.
//
// It is also purely an optimisation. Nothing waits for it: a page whose cache is missing matches
// straight from the HTML exactly as before, so translations are never unavailable because the cache
// has not been built yet.
//
// Keyed on the SOURCE (the sidecar or the chapter's own XHTML), not on any render setting, so it
// outlives every section.bin invalidation.

// Walks the whole chapter and writes its pairs to `cachePath`, committed by an atomic rename so a
// truncated stream can never be read as "no more pairs". Returns false on any read/write failure.
bool writeTranslationPairCache(const std::string& htmlPath, const std::string& bookPrimaryLang,
                               const std::string& cachePath);

// True when `cachePath` holds a complete, current-format pair cache.
bool hasTranslationPairCache(const std::string& cachePath);

// matchPageParagraphs, but reading the pairs from `cachePath` when it holds a usable cache and
// falling back to the HTML walk when it does not. This is what the overlays call.
bool matchPageParagraphsCached(const std::string& cachePath, const std::string& htmlPath,
                               const std::string& bookPrimaryLang, const std::string* visibleTexts, int count,
                               PageMatch* out);

}  // namespace lingua
