#include <unity.h>

#include <cstdint>
#include <cstring>

// The real header. `[env:native]` puts src/activities/reader on the include path, and
// ReaderPosition.h is deliberately free of Arduino/SD/Epub deps so the production code -- not a
// copy of it -- is what runs here. The two things under test are the pieces of the reposition
// machinery that are pure arithmetic: the length-discriminated progress record (does a record
// written by older firmware still decode, and does the new one round-trip?) and the fallback
// page-ratio remap (is its denominator handling safe?).
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
  return UNITY_END();
}
