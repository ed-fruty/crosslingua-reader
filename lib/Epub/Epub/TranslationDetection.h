#pragma once

#include <cstddef>
#include <string>

// ── "Is this content translated?" — ONE definition ────────────────────────────
//
// Three subsystems have to agree on what makes a piece of chapter content a TRANSLATION:
//
//   1. ChapterHtmlSlimParser — the layout engine. It decides per element, from a `lang=` /
//      `xml:lang=` that differs from the book's primary language, whether the words it is about
//      to lay out are translated (ChapterHtmlSlimParser::currentBlockIsTranslated). Every
//      Pre-Translation layout is built on that bit: OriginalOnly drops the translated words,
//      TranslationOnly drops the originals, SideBySide / Interlinear pair them.
//   2. Section — the per-chapter GATE. "Does this chapter have a translation at all?" decides
//      whether a filtering/pairing layout may run (Section::effectiveLayout) and is half the
//      section-cache key.
//   3. The reader UI — the Lingua submenu's "you have no translation for this chapter" refusal
//      and the reader's per-chapter fallback dialog.
//
// If (2) and (3) used a different rule from (1), the gate and the renderer would disagree: a
// chapter that claims a translation and renders none, or -- the bug this header exists to
// prevent -- a chapter that HAS one and is refused every bilingual mode. That is exactly what
// happened while the gate was `Storage.exists(<spine>.translated.html)`: it only ever saw a
// translation the READER produced, so a book translated by a Calibre plugin (which embeds the
// translated paragraphs directly in the chapter's own XHTML, `<p lang="uk">` next to the
// `lang="en"` original, with no sidecar) was permanently locked to Normal.
//
// ── The rule ──────────────────────────────────────────────────────────────────
//
// Content is translated iff it carries a language tag whose PRIMARY SUBTAG differs from the
// book's primary language. Nothing else: not a class name, not an inline colour, not `dir` --
// those are one plugin's presentation choices and the next plugin (or a user restyling the
// book) would not emit them, which would silently reintroduce the bug on the next book.
//
// Comparison (isTranslatedLangTag):
//   • Primary subtag only. `uk` and `uk-UA` are the same language; so are `en` and `en-GB`.
//     Both '-' (BCP 47) and '_' (emitted by some producers) end the primary subtag.
//   • ASCII case-insensitive: `EN`, `en` and `En` are one language.
//   • Surrounding ASCII whitespace is ignored.
//
// Unknown book language (no `<dc:language>`, or an empty one) => NOT translated. It is the only
// safe answer: with no primary language there is no way to tell which of two languages in the
// file is the original, so a filtering layout would have to guess and could drop the entire
// chapter. Answering "not translated" degrades the chapter to PtLayout::Both, which renders the
// file's full text exactly as a plain book -- nothing is hidden. It is also what the layout
// engine has always done (its `!bookPrimaryLang.empty()` guard), so gate and renderer stay in
// lock-step.
//
// Header-only for the predicate so the layout parser, the reader activities and the host unit
// tests all share the identical code path; the file scan needs expat + HalStorage and lives in
// TranslationDetection.cpp.
namespace translationdetect {

namespace detail {

inline bool isAsciiSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

// Length of the primary subtag of `s` (after skipping leading ASCII space): everything up to the
// first subtag separator, trailing space or end of string. Returns 0 for an empty/absent tag.
inline size_t primarySubtagLength(const char* s) {
  size_t n = 0;
  while (s[n] != '\0' && s[n] != '-' && s[n] != '_' && !isAsciiSpace(s[n])) {
    n++;
  }
  return n;
}

inline const char* skipLeadingSpace(const char* s) {
  while (*s != '\0' && isAsciiSpace(*s)) {
    s++;
  }
  return s;
}

}  // namespace detail

// True when `langAttr` marks content in a language other than the book's. See the rule above.
// Either argument being null/empty/whitespace-only yields false ("not translated").
inline bool isTranslatedLangTag(const char* langAttr, const char* bookPrimaryLang) {
  if (langAttr == nullptr || bookPrimaryLang == nullptr) return false;
  const char* a = detail::skipLeadingSpace(langAttr);
  const char* b = detail::skipLeadingSpace(bookPrimaryLang);
  const size_t la = detail::primarySubtagLength(a);
  const size_t lb = detail::primarySubtagLength(b);
  // No language on the element, or no language on the book: not translated (see the header note).
  if (la == 0 || lb == 0) return false;
  if (la != lb) return true;
  for (size_t i = 0; i < la; i++) {
    if (detail::lowerAscii(a[i]) != detail::lowerAscii(b[i])) return true;
  }
  return false;
}

// Does the chapter HTML at `htmlPath` contain embedded translated content?
//
// True iff some BLOCK-level element (paraboundary::isContainerBlockTag -- <p>, <div>, <li>,
// <blockquote>, <h1>..<h6>) carries a language tag isTranslatedLangTag() calls translated.
// Both the Calibre-plugin books and CrossPoint's own TranslatingHtmlRewriter emit their
// translations as block elements, so that is where a translation the layouts can actually USE
// lives.
//
// Deliberately narrower than the layout engine, which honours a `lang=` on ANY element (a
// `<span lang="fr">` inside an English paragraph tags those words translated too). The narrowing
// only ever errs toward "no translation", i.e. toward PtLayout::Both -- under which the parser
// still tags and styles such a span exactly as before. Widening it to inline elements would be
// the unsafe direction: one foreign word in an epigraph would let TranslationOnly run and render
// a near-blank chapter.
//
// Streaming SAX scan with early exit at the first translated block: a bilingual chapter usually
// answers within the first paragraph or two. A monolingual one costs a full read of the file.
// Callers must treat this as a per-chapter-load cost, never a per-frame one -- Section memoizes
// it for exactly that reason.
bool htmlHasTranslatedBlock(const std::string& htmlPath, const std::string& bookPrimaryLang);

}  // namespace translationdetect
