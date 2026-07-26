#pragma once
#include <cstddef>
#include <cstdint>

// Device-free position bookkeeping for the EPUB reader: the on-disk progress record and the
// arithmetic of the fallback page-ratio remap. Deliberately free of Arduino / SD / Epub headers so
// [env:native] can compile and test it on the host (test/test_reposition).
namespace ReaderPosition {

// progress.bin carries no version byte, so its LENGTH is the format tag. Every historic length
// still decodes, and a shorter record simply leaves the newer fields at their "absent" defaults --
// which degrades a reposition to exactly the behaviour that length shipped with, never to page 1 of
// the book. Appending a field means adding a length here and leaving the older ones readable.
constexpr size_t RECORD_SIZE_V1 = 4;  // spineIndex + pageNumber
constexpr size_t RECORD_SIZE_V2 = 6;  // + chapter page count (denominator of the page-ratio remap)
constexpr size_t RECORD_SIZE_V3 = 9;  // + paragraph anchor + flags
constexpr size_t RECORD_SIZE_MAX = RECORD_SIZE_V3;

// Flags byte (RECORD_SIZE_V3 and up).
// The paragraph anchor counts <p> elements in the SOURCE HTML the pages were laid out from. The
// bilingual `.translated.html` sidecar has its own (roughly doubled) count, so an anchor whose
// source has since flipped -- a translation downloaded or deleted between sessions -- is
// meaningless and must be discarded rather than trusted.
constexpr uint8_t FLAG_TRANSLATED_SOURCE = 0x01;

struct Record {
  int spineIndex = 0;
  int pageNumber = 0;
  // Chapter total at save time, 0 when absent. Only the denominator of the fallback remap.
  int pageCount = 0;
  // Layout-independent reading anchor: the 1-based count of <p> elements opened by the end of the
  // saved page. 0 = absent (chapter has no <p> elements, or the anchor could not identify the page).
  uint16_t paragraphAnchor = 0;
  bool translatedSource = false;
};

// Writes RECORD_SIZE_MAX bytes and returns that length. `out` must have room for RECORD_SIZE_MAX.
inline size_t encode(const Record& r, uint8_t* out) {
  out[0] = static_cast<uint8_t>(r.spineIndex & 0xFF);
  out[1] = static_cast<uint8_t>((r.spineIndex >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>(r.pageNumber & 0xFF);
  out[3] = static_cast<uint8_t>((r.pageNumber >> 8) & 0xFF);
  out[4] = static_cast<uint8_t>(r.pageCount & 0xFF);
  out[5] = static_cast<uint8_t>((r.pageCount >> 8) & 0xFF);
  out[6] = static_cast<uint8_t>(r.paragraphAnchor & 0xFF);
  out[7] = static_cast<uint8_t>((r.paragraphAnchor >> 8) & 0xFF);
  out[8] = r.translatedSource ? FLAG_TRANSLATED_SOURCE : 0;
  return RECORD_SIZE_MAX;
}

// Returns false when `len` is not a known record length, leaving `out` untouched: an unrecognised
// record reads as "no saved progress", which reopens the book at its text-reference start rather
// than at a position decoded from garbage.
inline bool decode(const uint8_t* data, const size_t len, Record& out) {
  if (len != RECORD_SIZE_V1 && len != RECORD_SIZE_V2 && len != RECORD_SIZE_V3) {
    return false;
  }
  out.spineIndex = data[0] | (data[1] << 8);
  out.pageNumber = data[2] | (data[3] << 8);
  if (len >= RECORD_SIZE_V2) {
    out.pageCount = data[4] | (data[5] << 8);
  }
  if (len >= RECORD_SIZE_V3) {
    out.paragraphAnchor = static_cast<uint16_t>(data[6] | (data[7] << 8));
    out.translatedSource = (data[8] & FLAG_TRANSLATED_SOURCE) != 0;
  }
  return true;
}

// Fallback remap for a chapter that re-paginated with no usable paragraph anchor: scale the page
// number by the ratio of the two chapter totals. Returns oldPage unchanged when either total is
// unusable or the pagination did not change, and always lands inside [0, newTotalPages - 1].
//
// Both totals must be whole-chapter counts. Feeding it a still-building section's `pageCount` (a
// build watermark of roughly currentPage + BUILD_WINDOW_AHEAD) makes the ratio ~1.0 and throws the
// reader to the end of the chapter -- see the callers, which pass estimatedTotalPages().
inline int remapByPageRatio(const int oldPage, const int oldTotalPages, const int newTotalPages) {
  if (oldTotalPages <= 0 || newTotalPages <= 0 || oldTotalPages == newTotalPages) {
    return oldPage;
  }
  if (oldPage <= 0) {
    return 0;
  }
  const float progress = static_cast<float>(oldPage) / static_cast<float>(oldTotalPages);
  int newPage = static_cast<int>(progress * static_cast<float>(newTotalPages));
  if (newPage < 0) {
    newPage = 0;
  }
  if (newPage >= newTotalPages) {
    newPage = newTotalPages - 1;
  }
  return newPage;
}

}  // namespace ReaderPosition
