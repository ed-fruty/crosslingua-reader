#include <unity.h>

#include <string>

// Forward declarations — implementation lives in src/translator/SentenceSplitter.cpp
int countSentences(const std::string& text);
std::string trimToSentences(const std::string& text, int maxSentences);
std::string trimToLastSentences(const std::string& text, int maxSentences);
int countSentencesBefore(const std::string& origText, const std::string& visibleStart);

void setUp(void) {}
void tearDown(void) {}

// countSentences tests
void test_countSentences_empty(void) { TEST_ASSERT_EQUAL(0, countSentences("")); }
void test_countSentences_single(void) { TEST_ASSERT_EQUAL(1, countSentences("Hello world.")); }
void test_countSentences_multi(void) { TEST_ASSERT_EQUAL(3, countSentences("Hello. World! Are you there?")); }
void test_countSentences_ellipsis_not_counted(void) {
  // "..." detection skips dots followed by another dot, so positions 4 and 5 are skipped.
  // Position 6 (the last dot of "...") is followed by a space → counts as a sentence terminator.
  // Plus the final "." after "see" → total 2. This matches fork's algorithm exactly.
  TEST_ASSERT_EQUAL(2, countSentences("Wait... I see."));
}
void test_countSentences_closing_quote(void) {
  // Sentence ends with `."` — closing quote should not break detection.
  TEST_ASSERT_EQUAL(2, countSentences("\"Hello.\" \"World.\""));
}
void test_countSentences_no_terminator(void) {
  // Non-empty text without terminator → returns 1 (at-least-one rule from fork).
  TEST_ASSERT_EQUAL(1, countSentences("Hello world without period"));
}

// trimToSentences tests
void test_trimToSentences_first2(void) { TEST_ASSERT_EQUAL_STRING("A. B.", trimToSentences("A. B. C.", 2).c_str()); }
void test_trimToSentences_more_than_exists(void) {
  TEST_ASSERT_EQUAL_STRING("A. B.", trimToSentences("A. B.", 5).c_str());
}
void test_trimToSentences_zero(void) { TEST_ASSERT_EQUAL_STRING("", trimToSentences("A. B.", 0).c_str()); }

// trimToLastSentences tests
void test_trimToLastSentences_last2(void) {
  TEST_ASSERT_EQUAL_STRING("B. C.", trimToLastSentences("A. B. C.", 2).c_str());
}
void test_trimToLastSentences_more_than_exists(void) {
  TEST_ASSERT_EQUAL_STRING("A. B.", trimToLastSentences("A. B.", 5).c_str());
}

// countSentencesBefore tests
void test_countSentencesBefore_match(void) {
  // visibleStart "C." appears after "A. B. " → 2 sentences before
  int n = countSentencesBefore("A. B. C. D.", "C. D.");
  TEST_ASSERT_EQUAL(2, n);
}
void test_countSentencesBefore_no_match(void) { TEST_ASSERT_EQUAL(0, countSentencesBefore("A. B.", "X.")); }
void test_countSentencesBefore_start_of_text(void) {
  // visibleStart starts at the beginning → 0 sentences before
  TEST_ASSERT_EQUAL(0, countSentencesBefore("A. B. C.", "A. B."));
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();

  RUN_TEST(test_countSentences_empty);
  RUN_TEST(test_countSentences_single);
  RUN_TEST(test_countSentences_multi);
  RUN_TEST(test_countSentences_ellipsis_not_counted);
  RUN_TEST(test_countSentences_closing_quote);
  RUN_TEST(test_countSentences_no_terminator);

  RUN_TEST(test_trimToSentences_first2);
  RUN_TEST(test_trimToSentences_more_than_exists);
  RUN_TEST(test_trimToSentences_zero);

  RUN_TEST(test_trimToLastSentences_last2);
  RUN_TEST(test_trimToLastSentences_more_than_exists);

  RUN_TEST(test_countSentencesBefore_match);
  RUN_TEST(test_countSentencesBefore_no_match);
  RUN_TEST(test_countSentencesBefore_start_of_text);

  return UNITY_END();
}
