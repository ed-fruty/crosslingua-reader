#include <unity.h>

// The real header. `[env:native]` puts lib/Epub/Epub on the include path, and the language
// predicate is deliberately header-only and free of Arduino/SD/expat deps so the production code --
// not a copy of it -- is what runs here. The predicate is the whole contract between the layout
// engine (ChapterHtmlSlimParser::currentBlockIsTranslated) and the per-chapter gate
// (Section::hasTranslation): if they ever disagree, a chapter either claims a translation and
// renders none, or -- the bug this was written for -- has one and is refused every bilingual mode.
#include "TranslationDetection.h"

using translationdetect::isTranslatedLangTag;

// --- the basic rule ------------------------------------------------------------------------------

static void test_a_different_language_is_translated() {
  // The author's Calibre-plugin book: <dc:language>en</dc:language>, <p lang="uk"> translations.
  TEST_ASSERT_TRUE(isTranslatedLangTag("uk", "en"));
}

static void test_the_book_language_is_not_translated() {
  // The same book tags its originals lang="en"; those must stay originals.
  TEST_ASSERT_FALSE(isTranslatedLangTag("en", "en"));
}

// --- region subtags ------------------------------------------------------------------------------

static void test_a_region_subtag_does_not_make_it_a_translation() {
  TEST_ASSERT_FALSE(isTranslatedLangTag("en-GB", "en"));
  TEST_ASSERT_FALSE(isTranslatedLangTag("en", "en-US"));
  TEST_ASSERT_FALSE(isTranslatedLangTag("en-GB", "en-US"));
}

static void test_a_region_subtag_does_not_hide_a_translation() {
  TEST_ASSERT_TRUE(isTranslatedLangTag("uk-UA", "en"));
  TEST_ASSERT_TRUE(isTranslatedLangTag("uk", "en-GB"));
}

static void test_an_underscore_also_separates_the_primary_subtag() {
  // Some producers emit en_US rather than BCP 47's en-US.
  TEST_ASSERT_FALSE(isTranslatedLangTag("en_US", "en"));
  TEST_ASSERT_TRUE(isTranslatedLangTag("uk_UA", "en"));
}

static void test_a_longer_primary_subtag_is_a_different_language() {
  // "en" vs "enm" (Middle English) must not compare equal by prefix.
  TEST_ASSERT_TRUE(isTranslatedLangTag("enm", "en"));
  TEST_ASSERT_TRUE(isTranslatedLangTag("en", "enm"));
}

// --- case and whitespace -------------------------------------------------------------------------

static void test_case_is_ignored() {
  TEST_ASSERT_FALSE(isTranslatedLangTag("EN", "en"));
  TEST_ASSERT_FALSE(isTranslatedLangTag("En", "eN-gb"));
  TEST_ASSERT_TRUE(isTranslatedLangTag("UK", "en"));
}

static void test_surrounding_whitespace_is_ignored() {
  TEST_ASSERT_FALSE(isTranslatedLangTag(" en ", "en"));
  TEST_ASSERT_TRUE(isTranslatedLangTag("\tuk\n", "en"));
}

// --- the "cannot know" cases ---------------------------------------------------------------------

static void test_no_language_on_the_element_is_not_translated() {
  TEST_ASSERT_FALSE(isTranslatedLangTag(nullptr, "en"));
  TEST_ASSERT_FALSE(isTranslatedLangTag("", "en"));
  TEST_ASSERT_FALSE(isTranslatedLangTag("   ", "en"));
}

static void test_an_unknown_book_language_is_never_translated() {
  // A book with no (or an empty) <dc:language> gives no way to tell which of two languages is the
  // original, so nothing may be classified as a translation: the chapter degrades to LinguaLayout::Both
  // and renders its full text, instead of a filtering layout guessing and blanking it.
  TEST_ASSERT_FALSE(isTranslatedLangTag("uk", ""));
  TEST_ASSERT_FALSE(isTranslatedLangTag("uk", nullptr));
  TEST_ASSERT_FALSE(isTranslatedLangTag("uk", " "));
}

// --- presentation attributes are NOT the signal ----------------------------------------------------

static void test_only_the_language_decides() {
  // The sample book's translated paragraphs happen to be
  //   <p class="subsq" dir="auto" lang="uk" style="color:#5A5A5A">
  // but the class, dir and grey colour are that plugin's styling, not a semantic marker. The
  // predicate takes the language tag alone, so a plugin that styles differently still works and a
  // styled ORIGINAL is never mistaken for a translation. Both facts are exercised by passing the
  // language values those paragraphs carry, with nothing else available to key on.
  TEST_ASSERT_TRUE(isTranslatedLangTag("uk", "en"));   // styled translation
  TEST_ASSERT_FALSE(isTranslatedLangTag("en", "en"));  // identically styled original
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_different_language_is_translated);
  RUN_TEST(test_the_book_language_is_not_translated);
  RUN_TEST(test_a_region_subtag_does_not_make_it_a_translation);
  RUN_TEST(test_a_region_subtag_does_not_hide_a_translation);
  RUN_TEST(test_an_underscore_also_separates_the_primary_subtag);
  RUN_TEST(test_a_longer_primary_subtag_is_a_different_language);
  RUN_TEST(test_case_is_ignored);
  RUN_TEST(test_surrounding_whitespace_is_ignored);
  RUN_TEST(test_no_language_on_the_element_is_not_translated);
  RUN_TEST(test_an_unknown_book_language_is_never_translated);
  RUN_TEST(test_only_the_language_decides);
  return UNITY_END();
}

void setUp() {}
void tearDown() {}
