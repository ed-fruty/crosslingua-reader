#pragma once

#include <cstring>

// ── Paragraph-boundary predicate (single source of truth) ─────────────────────
//
// Two code paths independently decide "where does paragraph N end":
//   1. ChapterHtmlSlimParser  — the layout parser; its per-paragraph counter is
//      the authority every consumer aligns to (PageLine::paragraphIdx,
//      Page::first/lastParagraphIdx).
//   2. PageTranslationOverlay's PageTranslationParseCtx — a SAX reparse of the same HTML that maps
//      translation paragraphs back to the layout parser's indices.
//
// If those two disagree on the boundary rule by even one tag, their paragraph
// counters drift and translations attach to the wrong paragraph. This header
// captures ONE definition of the rule so the reparser can mirror the layout
// parser byte-for-byte.
//
// ── The rule (as ChapterHtmlSlimParser implements it today) ───────────────────
//
// A new paragraph begins when one of these appears and the current text block
// already has content:
//   • a CONTAINER block tag — <p> <h1>..<h6> <li> <blockquote> <div>. Opening it
//     starts a paragraph; its matching close ends that paragraph.
//   • a HARD BREAK <br/>. It is an EMPTY element (no scope to open), so it ends
//     the current paragraph IN PLACE and the following text continues a new
//     paragraph within the SAME enclosing container. Reparsers must special-case
//     it (flush-in-place) rather than treat it as a container open.
//
// Additional decisions the layout parser makes, which the reparser must respect
// so its counts stay in lock-step (documented here as the shared contract; the
// reparser enforces them against the text it accumulates):
//   • EMPTY / WHITESPACE-ONLY block => NO counter increment. An opened container
//     whose text (after trimming leading/trailing ASCII space + newline) is empty
//     is reused, not counted. Reparsers must drop whitespace-only paragraphs.
//   • <table> => exactly ONE paragraph unit ("[Table omitted]" placeholder); its
//     contents are skipped, so it never contributes more than one index.
//   • <img> with alt text => exactly ONE paragraph unit ("[Image: ...]"); a src
//     image contributes no text paragraph.
//   • <li> => one paragraph, prefixed with a bullet glyph in layout (the bullet
//     does not change the boundary count).
//
// Header-only + inline so both lib/Epub and src/ can include it without a new
// translation unit. From lib/Epub sources include as "ParagraphBoundary.h";
// from src/ include as <Epub/parsers/ParagraphBoundary.h>.
namespace paraboundary {

// Container block tags: <p> <h1>..<h6> <li> <blockquote> <div>. Opening one
// begins a paragraph; closing it ends that paragraph. Excludes <br/> (empty,
// no scope) — see isHardBreak(). This is exactly the block-tag set the overlay
// reparser tracks via its inBlock / blockDepth state.
inline bool isContainerBlockTag(const char* name) {
  return strcmp(name, "p") == 0 || strcmp(name, "div") == 0 || strcmp(name, "li") == 0 ||
         strcmp(name, "blockquote") == 0 || strcmp(name, "h1") == 0 || strcmp(name, "h2") == 0 ||
         strcmp(name, "h3") == 0 || strcmp(name, "h4") == 0 || strcmp(name, "h5") == 0 || strcmp(name, "h6") == 0;
}

// <br/> — the hard line break. Not a container: it flushes the current paragraph
// in place and the next text starts a new paragraph inside the same element.
inline bool isHardBreak(const char* name) { return strcmp(name, "br") == 0; }

// The full set of tags that delimit a paragraph boundary in the layout parser:
// every container block tag plus <br/>. This is the union that
// ChapterHtmlSlimParser historically computed as HEADER_TAGS ∪ BLOCK_TAGS.
inline bool isParagraphBlockTag(const char* name) { return isContainerBlockTag(name) || isHardBreak(name); }

}  // namespace paraboundary
