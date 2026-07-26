#pragma once
#include <cstdint>

// The pure decision half of Section::paragraphAnchorForPage(): which paragraph index (if any) a
// rendered page should be anchored to for a reposition. Split out from Section.cpp -- which can only
// run on the device (SD, Epub, the renderer) -- so `[env:native]` can test the rule itself against a
// synthetic LUT instead of a hand-copied paraphrase of it. See test/test_reposition.
//
// Inputs are paragraph-LUT entries: entry[p] is the number of <p> open tags the parser had seen when
// page p was flushed (ChapterHtmlSlimParser::xpathParagraphIndex). A page break happens partway
// through laying out a block, so entry[p] is the index of the LAST paragraph with content on page p,
// and that paragraph usually continues onto page p+1. Indices are 1-based; 0 means "no <p> has been
// opened yet".
//
// Every anchor resolves back to a page via "the first page whose entry is >= the anchor", i.e. the
// page the anchored paragraph STARTS on (Section::findPageForParagraphIndex). 0 is the "no anchor"
// answer everywhere, matching ReaderPosition::Record::paragraphAnchor.
namespace ParagraphAnchor {

// How many pages a reposition may send the reader BACK when the saved page opens no paragraph of its
// own and the only handle is the paragraph running through it. Landing at that paragraph's start is
// then a re-read, never a skip -- but on a chapter laid out as one enormous <p> it would be a
// teleport to the chapter top, so the step is bounded and anything longer is refused (the caller
// falls back to the page ratio). 2 covers a paragraph spanning up to three pages, which is already
// well past normal prose and past Interlinear's roughly doubled line count.
constexpr uint16_t MAX_BACKWARD_DRIFT_PAGES = 2;

// Preferred anchor: the FIRST paragraph that opens on the saved page, which is entry[page - 1] + 1.
// Returns 0 when the page opens no new paragraph (entry did not advance), in which case the caller
// falls through to spanningAnchor() below.
//
// Not entry[page] -- that is the LAST paragraph on the page, so after a re-layout the reader would
// land where it starts and SKIP everything above it on the old page (a whole page's worth when the
// page held one long paragraph followed by a short one). The first opening paragraph is at most one
// paragraph tail below the top of the old page. Both choices resolve back to `page` exactly under an
// unchanged pagination, so the exact round-trip costs nothing.
constexpr uint16_t openingAnchor(const uint16_t prev, const uint16_t here) {
  return here > prev ? static_cast<uint16_t>(prev + 1) : 0;
}

// Whether the saved page is far enough into the chapter for the drift bound below to need checking.
// Nothing can start earlier than page 0, so a page within MAX_BACKWARD_DRIFT_PAGES of the top is
// bounded by construction -- worth knowing because reading the guard entry costs an SD read on a
// finalized section.
constexpr bool guardNeeded(const uint16_t page) { return page > MAX_BACKWARD_DRIFT_PAGES; }

// The LUT entry that bounds the drift: the last page that must still be BELOW `here` for the
// paragraph to have started within MAX_BACKWARD_DRIFT_PAGES pages of the saved one. Only valid when
// guardNeeded(page).
constexpr uint16_t guardPage(const uint16_t page) { return static_cast<uint16_t>(page - MAX_BACKWARD_DRIFT_PAGES - 1); }

// Fallback anchor for a page that opens no paragraph of its own: the paragraph running through it,
// accepted only when the guard entry shows it started within the drift bound. Returns 0 when the
// chapter has opened no <p> at all by this page, or when the paragraph started further back.
constexpr uint16_t spanningAnchor(const uint16_t here, const uint16_t guardIndex) {
  return (here != 0 && guardIndex < here) ? here : 0;
}

}  // namespace ParagraphAnchor
