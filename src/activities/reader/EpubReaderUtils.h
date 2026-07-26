#pragma once

#include <Epub.h>
#include <Logging.h>

#include "ProgressFile.h"
#include "ReaderPosition.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
//
// `paragraphAnchor` (0 = none) is the layout-INDEPENDENT half of the record: a page number is only
// meaningful under the pagination it was measured in, so when the chapter is re-laid-out under a new
// font, spacing, orientation or translation-display layout it is the anchor -- not the page number,
// and not the page/pageCount ratio -- that gets the reader back to where they were. See
// Section::paragraphAnchorForPage() and EpubReaderActivity::render().
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount, uint16_t paragraphAnchor = 0,
                         bool translatedSource = false) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const ReaderPosition::Record record{spineIndex, pageNumber, pageCount, paragraphAnchor, translatedSource};
  uint8_t data[ReaderPosition::RECORD_SIZE_MAX];
  const size_t len = ReaderPosition::encode(record, data);
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, len)) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d para=%u", spineIndex, pageNumber, paragraphAnchor);
  return true;
}

}  // namespace EpubReaderUtils
