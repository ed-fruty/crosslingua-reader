#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ── Canonical text fold (shared text core) ───────────────────────────────────
//
// One single-pass, allocation-light normalization used to compare book text
// that reaches us through different code paths (raw HTML via expat vs. words
// laid out for the page) and to feed sentence-boundary detection. Callers must
// apply it LAZILY (never eagerly on every paragraph at parse time).
//
// The fold is deliberately lossy: it maps every visual variant of a punctuation
// mark onto a single canonical byte so that two spellings of "the same" text
// compare equal. It is used ONLY for matching / boundary detection — never for
// display, which always uses the original text.
namespace textnorm {

// Single-byte sentinel that every ellipsis form ("..", "...", U+2026 …) folds
// to. 0x01 (SOH) never occurs in real book text, keeps the fold output one byte
// shorter than any input, and lets sentence-terminator recognition treat the
// ellipsis distinctly from a lone period. Because match comparisons run over
// two strings that were both produced by foldForMatch(), the exact value only
// needs to be stable, not printable.
inline constexpr char ELLIPSIS_SENTINEL = '\x01';

// Canonical punctuation + whitespace fold. Single O(n) pass keyed on the UTF-8
// lead bytes 0xC2 / 0xCA / 0xE2; every other byte takes a cheap ASCII branch.
// UTF-8 safe: any multi-byte sequence that is not in the fold set is copied
// through verbatim, and a truncated trailing lead byte is emitted literally.
//
// Folds performed:
//   double quotes  " “ ” „ ‟ « »            -> ASCII '"'
//   single quotes  ' ‘ ’ ‚ ʼ               -> ASCII '\''
//   dashes         ‐ – — (and U+2212 −)     -> ASCII '-'
//   ellipsis       "..." (>=2 dots) / …     -> ELLIPSIS_SENTINEL
//   NBSP (U+00A0)                           -> single space (treated as ws)
//   soft hyphen (U+00AD)                    -> dropped (invisible in page words)
//   runs of ASCII/NBSP whitespace           -> single space, both ends trimmed
//
// `limit` caps the output length in bytes (`< 0` = uncapped, `0` = empty). The
// cap is checked before each unit, so a folded multi-byte unit that straddles
// the cap can carry the output a few bytes past `limit`, but a UTF-8 sequence is
// never split.
std::string foldForMatch(const std::string& text, int limit = -1);

// In-place variant of foldForMatch(). The fold never grows a string (every
// transform shrinks or preserves length), so this rewrites `s` with a two-
// pointer pass and does zero heap allocation. Semantics are identical to
// foldForMatch(); prefer this when the caller already owns a mutable buffer.
void foldForMatchInPlace(std::string& s, int limit = -1);

// ── Needle building for page-to-source matching ──────────────────────────────
//
// Locating the text a PAGE shows inside the SOURCE it came from has two artifacts to survive, and
// both bite in more than one place (the Lingua pair matcher and countSentencesBefore), so the rule
// lives here rather than in each caller:
//
//   1. WORD GLUE. A serialized page carries no "this word continues the previous one" flag, so a
//      word split by an inline tag ("<i>lately</i>-unless") arrives as two words that the caller
//      joins with a space the source does not contain. Collapsing whitespace is not enough; the
//      space has to be GONE on both sides. Hence the No-Spaces pair below.
//   2. LINE-BREAK HYPHENATION. Layout splits a word at a line end into "prefix-" + "remainder", so
//      the page carries a "prefix- remainder" the unsplit source lacks. It is recognizable as a '-'
//      glued to the preceding character and followed by a space -- a real spaced dash has spaces on
//      BOTH sides -- and the needle simply stops there. The kept prefix always covers at least the
//      first word, since a line-end split never lands on the first token.

// foldForMatch() with every space removed: the HAYSTACK side of such a match.
std::string foldForMatchNoSpaces(const std::string& text, int limit = -1);

// The NEEDLE side: folded, truncated before the first hyphenation artifact, then de-spaced. Compare
// it against foldForMatchNoSpaces() output. May come back shorter than `limit` -- a caller that
// needs a discriminating needle should reject a very short result.
std::string foldForNeedle(const std::string& text, int limit = -1);

// Length in bytes of a canonical sentence terminator starting at `text[i]`, or
// 0 if `text[i]` does not begin one. Recognizes ASCII '.', '!', '?', the
// ellipsis sentinel (all 1 byte), and the CJK terminators 。(U+3002),
// ！(U+FF01) and ？(U+FF1F) (3 bytes). Intended to run over foldForMatch()
// output; the CJK forms are unaffected by the fold, so it also works on raw
// UTF-8. This is the ONE terminator primitive that word-span and char-offset
// sentence scanners are expected to wrap.
int terminatorLenAt(const std::string& text, size_t i);

// Length in bytes of a closing quote / bracket starting at `text[i]`, or 0 if
// none. Meant to run over foldForMatch() output, where every fancy quote has
// already been folded to ASCII, so only " ' ) ] need to be recognized. Used to
// skip trailing quotes when locating a sentence terminator.
int closingQuoteLenAt(const std::string& text, size_t i);

// Length in bytes of an inter-token whitespace unit starting at `text[i]`, or 0
// if `text[i]` does not begin one. This is the SSOT for "what separates words /
// sentences" and is the same set foldForMatch() collapses to a single space:
//   • ASCII space, tab, CR, LF                         (1 byte)
//   • U+00A0 NO-BREAK SPACE                             (2 bytes, C2 A0)
//   • U+2000..U+200A general-punctuation spaces         (3 bytes, E2 80 80..8A)
//   • U+202F NARROW NO-BREAK SPACE                      (3 bytes, E2 80 AF)
//   • U+205F MEDIUM MATHEMATICAL SPACE                  (3 bytes, E2 81 9F)
// Works on raw UTF-8 (the multi-byte forms survive verbatim if the fold is not
// applied first). Recognizing these is why a sentence terminator followed by an
// NBSP-style separator — common in translation-service and EPUB text — is still
// seen as a sentence boundary. Truncated trailing lead bytes report 0.
int whitespaceLenAt(const std::string& text, size_t i);

}  // namespace textnorm
