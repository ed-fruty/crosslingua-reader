#pragma once

#include <cstdint>

// Which HTML a section was laid out from, as far as its visible-text offsets are concerned.
//
// Visible-text offsets (page LUT, word source ranges) are positions in the chapter's ORIGINAL text:
// progress, bookmarks, clippings and KOReader sync all store them, and KOReader's XPath resolver
// counts them on the book's own XHTML. A reader-produced `.translated.html` sidecar has to produce
// the same numbers for the same original text, or every stored position drifts once a chapter is
// translated.
//
//   Original      the chapter's own XHTML (embedded bilingual books included): counted as is.
//   SidecarV2     a sidecar whose root element carries SIDECAR_FORMAT_ATTR="2" (the parser reads it at
//                 the root, before any body text). The rewriter adds no characters of its
//                 own and keeps any translation it replaced as a `hidden` block, so the original
//                 text is byte-identical; the parser does not count the inserted translation blocks
//                 (INSERTED_TRANSLATION_ATTR), whose words get a frozen offset and no source range.
//   LegacySidecar a sidecar without the marker (written before v2). Its offsets cannot be mapped to
//                 the original, so the whole section counts from LEGACY_SIDECAR_OFFSET_BASE: its
//                 offsets stay consistent with each other (reposition, highlights on this chapter)
//                 but can never be mistaken for an original-text position, and the paragraph /
//                 list-item LUTs are written empty.
enum class LinguaSourceKind : uint8_t { Original = 0, SidecarV2 = 1, LegacySidecar = 2 };

namespace lingua {

// Written by TranslationHtmlRewriter on the root element: `<html ... data-crosslingua-sidecar="2">`.
constexpr char SIDECAR_FORMAT_ATTR[] = "data-crosslingua-sidecar";
constexpr char SIDECAR_FORMAT_V2[] = "2";
// The attribute TranslationHtmlRewriter puts on every translation block it inserts.
constexpr char INSERTED_TRANSLATION_ATTR[] = "data-translation";

constexpr uint32_t LEGACY_SIDECAR_OFFSET_BASE = 0x80000000u;

// True when `offset` belongs to a LegacySidecar section's offset space. UINT32_MAX ("unknown") does
// not; 0 (a chapter's first page) belongs to every space.
constexpr bool isLegacySidecarOffset(const uint32_t offset) {
  return offset != UINT32_MAX && offset >= LEGACY_SIDECAR_OFFSET_BASE;
}

// Whether a section laid out from `kind` can resolve a stored visible-text `offset`.
constexpr bool sourceAcceptsVisibleTextOffset(const LinguaSourceKind kind, const uint32_t offset) {
  return offset == 0 || (kind == LinguaSourceKind::LegacySidecar) == isLegacySidecarOffset(offset);
}

// Inside a v2 sidecar, consecutive pages of one inserted translation start at the same offset.
// Among such a run [first, last], an offset lookup takes the caller's page `hint` when it is one of
// them (a reopen lands where the reader left), else the first (never past text the reader has not
// reached).
constexpr uint16_t pickTiePage(const uint16_t first, const uint16_t last, const int hint) {
  return hint >= static_cast<int>(first) && hint <= static_cast<int>(last) ? static_cast<uint16_t>(hint) : first;
}

}  // namespace lingua
