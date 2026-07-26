#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>

// The real headers. `[env:native]` puts src/activities/reader and lib/Epub/Epub on the include path,
// and both files are deliberately free of Arduino/SD/Epub deps so the production code -- not a copy
// of it -- is what runs here. Three things are under test, all of them the pure parts of the
// reposition machinery: the length-discriminated progress record (does a record written by older
// firmware still decode, and does the new one round-trip?), the fallback page-ratio remap (is its
// denominator handling safe?), and the paragraph-anchor rule (which pages can be anchored, and how
// far can resolving one move the reader?).
#include "ParagraphAnchor.h"
#include "ReaderPosition.h"

using ReaderPosition::decode;
using ReaderPosition::encode;
using ReaderPosition::Record;
using ReaderPosition::remapByPageRatio;

// --- progress record ---------------------------------------------------------------------------

static void test_record_round_trips_every_field() {
  const Record in{7, 42, 100, 311, true};
  uint8_t buf[ReaderPosition::RECORD_SIZE_MAX];
  const size_t len = encode(in, buf);
  TEST_ASSERT_EQUAL_size_t(ReaderPosition::RECORD_SIZE_V3, len);

  Record out;
  TEST_ASSERT_TRUE(decode(buf, len, out));
  TEST_ASSERT_EQUAL_INT(7, out.spineIndex);
  TEST_ASSERT_EQUAL_INT(42, out.pageNumber);
  TEST_ASSERT_EQUAL_INT(100, out.pageCount);
  TEST_ASSERT_EQUAL_UINT16(311, out.paragraphAnchor);
  TEST_ASSERT_TRUE(out.translatedSource);
}

static void test_record_round_trips_16bit_extremes() {
  const Record in{0xFFFF, 0xFFFE, 0xFFFF, 0xFFFF, false};
  uint8_t buf[ReaderPosition::RECORD_SIZE_MAX];
  Record out;
  TEST_ASSERT_TRUE(decode(buf, encode(in, buf), out));
  TEST_ASSERT_EQUAL_INT(0xFFFF, out.spineIndex);
  TEST_ASSERT_EQUAL_INT(0xFFFE, out.pageNumber);
  TEST_ASSERT_EQUAL_INT(0xFFFF, out.pageCount);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, out.paragraphAnchor);
  TEST_ASSERT_FALSE(out.translatedSource);
}

// A 6-byte record is what every shipped firmware wrote. It must still yield the spine, the page and
// the ratio denominator -- i.e. degrade to exactly the old behaviour -- and must NOT invent an
// anchor, which would send the reader to whatever page paragraph 0 resolves to.
static void test_legacy_six_byte_record_keeps_page_and_count() {
  const uint8_t legacy[6] = {0x05, 0x00, 0x28, 0x00, 0x64, 0x00};  // spine 5, page 40, count 100
  Record out;
  TEST_ASSERT_TRUE(decode(legacy, sizeof(legacy), out));
  TEST_ASSERT_EQUAL_INT(5, out.spineIndex);
  TEST_ASSERT_EQUAL_INT(40, out.pageNumber);
  TEST_ASSERT_EQUAL_INT(100, out.pageCount);
  TEST_ASSERT_EQUAL_UINT16(0, out.paragraphAnchor);
  TEST_ASSERT_FALSE(out.translatedSource);
}

// The oldest 4-byte record has no chapter total either: no anchor AND no denominator, so a
// reposition is skipped entirely and the saved page is used as-is. Still never page 1 of the book.
static void test_legacy_four_byte_record_keeps_page() {
  const uint8_t legacy[4] = {0x03, 0x00, 0x11, 0x00};  // spine 3, page 17
  Record out;
  TEST_ASSERT_TRUE(decode(legacy, sizeof(legacy), out));
  TEST_ASSERT_EQUAL_INT(3, out.spineIndex);
  TEST_ASSERT_EQUAL_INT(17, out.pageNumber);
  TEST_ASSERT_EQUAL_INT(0, out.pageCount);
  TEST_ASSERT_EQUAL_UINT16(0, out.paragraphAnchor);
}

static void test_unknown_record_length_is_rejected_without_touching_output() {
  const uint8_t junk[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
  Record out{9, 9, 9, 9, true};
  TEST_ASSERT_FALSE(decode(junk, 5, out));
  TEST_ASSERT_FALSE(decode(junk, 8, out));
  TEST_ASSERT_FALSE(decode(junk, 0, out));
  // Untouched, so the caller keeps whatever it had rather than a position built from garbage.
  TEST_ASSERT_EQUAL_INT(9, out.spineIndex);
  TEST_ASSERT_EQUAL_INT(9, out.pageNumber);
  TEST_ASSERT_EQUAL_INT(9, out.pageCount);
  TEST_ASSERT_EQUAL_UINT16(9, out.paragraphAnchor);
  TEST_ASSERT_TRUE(out.translatedSource);
}

static void test_flags_byte_ignores_unknown_bits() {
  uint8_t buf[ReaderPosition::RECORD_SIZE_MAX] = {0, 0, 1, 0, 10, 0, 4, 0, 0xFE};  // no bit0 set
  Record out;
  TEST_ASSERT_TRUE(decode(buf, ReaderPosition::RECORD_SIZE_V3, out));
  TEST_ASSERT_FALSE(out.translatedSource);
  buf[8] = 0xFF;  // bit0 set among unknown bits
  TEST_ASSERT_TRUE(decode(buf, ReaderPosition::RECORD_SIZE_V3, out));
  TEST_ASSERT_TRUE(out.translatedSource);
}

// --- fallback page-ratio remap -----------------------------------------------------------------

// The reported bug in its arithmetic form. The reader is 40% into a 100-page chapter at 14pt; the
// chapter re-paginates to 130 pages at 18pt. Remapped against the real old total the landing is 52
// (40%); against a *build watermark* (page 40 + a 5-page look-ahead = 45), which is what the old
// capture sites recorded mid-chapter, it is 115 of 130 -- 88% in, near the chapter's end.
static void test_ratio_remap_grows_with_the_chapter() { TEST_ASSERT_EQUAL_INT(52, remapByPageRatio(40, 100, 130)); }

static void test_watermark_denominator_is_what_teleported_the_reader() {
  // Documents the failure the callers must never reproduce: a watermark denominator is ~= the
  // current page, so the ratio is ~1.0 regardless of how deep the reader actually was.
  TEST_ASSERT_EQUAL_INT(115, remapByPageRatio(40, 45, 130));
}

static void test_ratio_remap_shrinks_with_the_chapter() { TEST_ASSERT_EQUAL_INT(31, remapByPageRatio(40, 100, 78)); }

static void test_ratio_remap_is_a_no_op_when_pagination_is_unchanged() {
  TEST_ASSERT_EQUAL_INT(40, remapByPageRatio(40, 100, 100));
}

static void test_ratio_remap_keeps_the_page_when_a_total_is_unusable() {
  // No saved total (legacy 4-byte record) and no new total (a still-empty build) must both leave the
  // page alone rather than collapse it to 0.
  TEST_ASSERT_EQUAL_INT(40, remapByPageRatio(40, 0, 130));
  TEST_ASSERT_EQUAL_INT(40, remapByPageRatio(40, 100, 0));
  TEST_ASSERT_EQUAL_INT(40, remapByPageRatio(40, -1, 130));
}

static void test_ratio_remap_never_leaves_the_new_page_range() {
  // A saved page at or past its own total (a stale record) must clamp to the last page, not run off
  // the end and hit render()'s out-of-bounds screen.
  TEST_ASSERT_EQUAL_INT(129, remapByPageRatio(100, 100, 130));
  TEST_ASSERT_EQUAL_INT(129, remapByPageRatio(400, 100, 130));
  TEST_ASSERT_EQUAL_INT(0, remapByPageRatio(0, 100, 130));
  TEST_ASSERT_EQUAL_INT(0, remapByPageRatio(-3, 100, 130));
}

static void test_ratio_remap_keeps_the_first_page_first() {
  // Page 0 is the chapter's top under every pagination; it must never drift.
  for (int newTotal = 1; newTotal <= 500; newTotal += 37) {
    TEST_ASSERT_EQUAL_INT(0, remapByPageRatio(0, 100, newTotal));
  }
}

// --- paragraph anchor ----------------------------------------------------------------------------

namespace {

// Drives ParagraphAnchor::* in exactly the order Section::paragraphAnchorForPage() does, with a flat
// array standing in for the section.bin paragraph LUT (entry[p] = the count of <p> open tags the
// parser had seen when page p was flushed). Section reads those entries one at a time, from RAM
// during a build and from SD afterwards, which is the only reason it is not this function.
std::optional<uint16_t> anchorForPage(const uint16_t* lut, const size_t count, const uint16_t page) {
  if (page == 0 || page >= count) return std::nullopt;
  const uint16_t here = lut[page];
  if (here == 0) return std::nullopt;
  const uint16_t prev = lut[page - 1];
  if (const uint16_t opening = ParagraphAnchor::openingAnchor(prev, here)) return opening;
  if (!ParagraphAnchor::guardNeeded(page)) return here;
  const uint16_t spanning = ParagraphAnchor::spanningAnchor(here, lut[ParagraphAnchor::guardPage(page)]);
  if (spanning == 0) return std::nullopt;
  return spanning;
}

// Mirrors Section::findPageForParagraphIndex(): the first page whose entry has reached the index,
// i.e. the page the anchored paragraph starts on.
std::optional<uint16_t> pageForAnchor(const uint16_t* lut, const size_t count, const uint16_t index) {
  for (size_t i = 0; i < count; i++) {
    if (lut[i] >= index) return static_cast<uint16_t>(i);
  }
  return std::nullopt;
}

// Ordinary prose: three paragraphs finish on every page.
constexpr uint16_t PROSE[] = {3, 6, 9, 12, 15, 18, 21, 24};

}  // namespace

// The property the whole anchored path rests on: under an UNCHANGED pagination the anchor resolves
// back to the very page it was taken from, so a rebuild that did not re-paginate (Delete Cache) puts
// the reader back exactly where they were.
static void test_anchor_round_trips_on_an_unchanged_layout() {
  for (uint16_t page = 1; page < std::size(PROSE); page++) {
    const auto anchor = anchorForPage(PROSE, std::size(PROSE), page);
    TEST_ASSERT_TRUE(anchor.has_value());
    const auto landing = pageForAnchor(PROSE, std::size(PROSE), *anchor);
    TEST_ASSERT_TRUE(landing.has_value());
    TEST_ASSERT_EQUAL_UINT16(page, *landing);
  }
}

// The anchor must be the FIRST paragraph the page opens, not the last one on it. Page 2 of PROSE
// holds paragraphs 7, 8 and 9; anchoring on 9 would, after any re-layout that pushes 9 onto a later
// page, skip 7 and 8 entirely -- content the reader had not read.
static void test_anchor_is_the_first_paragraph_the_page_opens() {
  TEST_ASSERT_EQUAL_UINT16(7, *anchorForPage(PROSE, std::size(PROSE), 2));
  TEST_ASSERT_EQUAL_UINT16(4, *anchorForPage(PROSE, std::size(PROSE), 1));
}

// Same chapter re-laid-out at a larger font (two paragraphs per page instead of three). The reader
// was at the top of old page 2, which begins at paragraph 7; they must land on the new page that
// begins at paragraph 7, never past it.
static void test_anchor_lands_on_the_same_paragraph_after_growing() {
  constexpr uint16_t BIGGER[] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
  const auto anchor = anchorForPage(PROSE, std::size(PROSE), 2);
  TEST_ASSERT_EQUAL_UINT16(7, *anchor);
  const auto landing = pageForAnchor(BIGGER, std::size(BIGGER), *anchor);
  TEST_ASSERT_EQUAL_UINT16(3, *landing);  // BIGGER page 3 holds paragraphs 7 and 8
}

// The shrinking direction, which was one of the original bugs: four paragraphs per page now.
static void test_anchor_lands_on_the_same_paragraph_after_shrinking() {
  constexpr uint16_t SMALLER[] = {4, 8, 12, 16, 20, 24};
  const auto anchor = anchorForPage(PROSE, std::size(PROSE), 4);  // old page 4 opens paragraph 13
  TEST_ASSERT_EQUAL_UINT16(13, *anchor);
  const auto landing = pageForAnchor(SMALLER, std::size(SMALLER), *anchor);
  TEST_ASSERT_EQUAL_UINT16(3, *landing);  // SMALLER page 3 holds paragraphs 13-16
}

// Page 0 is the chapter top under every pagination, so the saved page number already survives the
// re-layout and the anchor must stay out of it -- the only index page 0 could offer is the last
// paragraph on it, which under a larger font starts on page 1.
static void test_page_zero_is_never_anchored() {
  TEST_ASSERT_FALSE(anchorForPage(PROSE, std::size(PROSE), 0).has_value());
}

// A chapter that marks its paragraphs with <div> opens no <p> at all, so every entry is 0 and no
// page can be anchored. This is the case the page-ratio fallback exists for; it must not be
// papered over with a bogus anchor of 0, which would resolve to the chapter's first page.
static void test_a_chapter_without_p_elements_is_never_anchored() {
  constexpr uint16_t NO_PARAGRAPHS[] = {0, 0, 0, 0, 0, 0};
  for (uint16_t page = 0; page < std::size(NO_PARAGRAPHS); page++) {
    TEST_ASSERT_FALSE(anchorForPage(NO_PARAGRAPHS, std::size(NO_PARAGRAPHS), page).has_value());
  }
}

// A page that opens no paragraph of its own (it lies inside one that began earlier) still anchors,
// on the paragraph running through it -- that is most of the anchor's coverage on a long-paragraph
// book, and under Interlinear, where every paragraph is roughly twice as tall. Resolving it steps
// BACK to the paragraph's first page, which is a re-read and never a skip, and the step is bounded.
static void test_a_page_inside_a_paragraph_anchors_within_the_drift_bound() {
  // Paragraph 4 spans pages 3, 4 and 5; paragraph 5 opens on page 6.
  constexpr uint16_t LONG_PARAGRAPH[] = {1, 2, 3, 4, 4, 4, 5, 6};
  for (uint16_t page = 3; page <= 5; page++) {
    const auto anchor = anchorForPage(LONG_PARAGRAPH, std::size(LONG_PARAGRAPH), page);
    TEST_ASSERT_TRUE(anchor.has_value());
    const auto landing = pageForAnchor(LONG_PARAGRAPH, std::size(LONG_PARAGRAPH), *anchor);
    TEST_ASSERT_TRUE(landing.has_value());
    TEST_ASSERT_TRUE(*landing <= page);
    TEST_ASSERT_TRUE(page - *landing <= ParagraphAnchor::MAX_BACKWARD_DRIFT_PAGES);
  }
}

// ...and the degenerate chapter laid out as one enormous <p> is refused rather than teleporting the
// reader to the chapter top. Pages 1 and 2 are still inside the bound (nothing can start before page
// 0); everything past that is out.
static void test_one_enormous_paragraph_is_refused_past_the_drift_bound() {
  constexpr uint16_t ONE_PARAGRAPH[] = {1, 1, 1, 1, 1, 1, 1, 1};
  for (uint16_t page = 1; page <= ParagraphAnchor::MAX_BACKWARD_DRIFT_PAGES; page++) {
    TEST_ASSERT_TRUE(anchorForPage(ONE_PARAGRAPH, std::size(ONE_PARAGRAPH), page).has_value());
  }
  for (uint16_t page = ParagraphAnchor::MAX_BACKWARD_DRIFT_PAGES + 1; page < std::size(ONE_PARAGRAPH); page++) {
    TEST_ASSERT_FALSE(anchorForPage(ONE_PARAGRAPH, std::size(ONE_PARAGRAPH), page).has_value());
  }
}

// A page whose paragraph began just inside the bound is kept; one page further back is dropped.
static void test_the_drift_bound_is_exactly_max_backward_drift_pages() {
  // Paragraph 2 begins on page 2 and runs to page 4: page 4 is 2 pages after it began (kept),
  // page 5 would be 3 (dropped).
  constexpr uint16_t SPANNING[] = {1, 1, 2, 2, 2, 2, 3};
  TEST_ASSERT_EQUAL_UINT16(2, *anchorForPage(SPANNING, std::size(SPANNING), 4));
  TEST_ASSERT_FALSE(anchorForPage(SPANNING, std::size(SPANNING), 5).has_value());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_record_round_trips_every_field);
  RUN_TEST(test_record_round_trips_16bit_extremes);
  RUN_TEST(test_legacy_six_byte_record_keeps_page_and_count);
  RUN_TEST(test_legacy_four_byte_record_keeps_page);
  RUN_TEST(test_unknown_record_length_is_rejected_without_touching_output);
  RUN_TEST(test_flags_byte_ignores_unknown_bits);
  RUN_TEST(test_ratio_remap_grows_with_the_chapter);
  RUN_TEST(test_watermark_denominator_is_what_teleported_the_reader);
  RUN_TEST(test_ratio_remap_shrinks_with_the_chapter);
  RUN_TEST(test_ratio_remap_is_a_no_op_when_pagination_is_unchanged);
  RUN_TEST(test_ratio_remap_keeps_the_page_when_a_total_is_unusable);
  RUN_TEST(test_ratio_remap_never_leaves_the_new_page_range);
  RUN_TEST(test_ratio_remap_keeps_the_first_page_first);
  RUN_TEST(test_anchor_round_trips_on_an_unchanged_layout);
  RUN_TEST(test_anchor_is_the_first_paragraph_the_page_opens);
  RUN_TEST(test_anchor_lands_on_the_same_paragraph_after_growing);
  RUN_TEST(test_anchor_lands_on_the_same_paragraph_after_shrinking);
  RUN_TEST(test_page_zero_is_never_anchored);
  RUN_TEST(test_a_chapter_without_p_elements_is_never_anchored);
  RUN_TEST(test_a_page_inside_a_paragraph_anchors_within_the_drift_bound);
  RUN_TEST(test_one_enormous_paragraph_is_refused_past_the_drift_bound);
  RUN_TEST(test_the_drift_bound_is_exactly_max_backward_drift_pages);
  return UNITY_END();
}
