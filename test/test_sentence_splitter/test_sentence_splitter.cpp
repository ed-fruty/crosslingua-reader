#include <unity.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// Forward declarations — implementation lives in src/translator/SentenceSplitter.cpp.
// (The native test env has no -I for src/translator, so we declare rather than
// include the header; the struct layout below mirrors SentenceSplitter.h exactly.)
int countSentences(const std::string& text);
std::string trimToSentences(const std::string& text, int maxSentences);
std::string trimToLastSentences(const std::string& text, int maxSentences);
int countSentencesBefore(const std::string& origText, const std::string& visibleStart);

// Word-array splitter (used by TooltipOverlay). Mirror of SentenceSplitter.h.
static constexpr int TEST_MAX_SENTENCES = 50;
struct SentenceSpan {
  uint16_t startWord;
  uint16_t endWord;
};
struct SentenceSplitResult {
  SentenceSpan spans[TEST_MAX_SENTENCES];
  int count = 0;
};
SentenceSplitResult splitSentences(const char* const* words, int wordCount);

// ── Tooltip translation-unit grouping (mirror of TooltipOverlay.cpp) ─────────
//
// groupTranslationSteps is a PURE helper, but its home TU (TooltipOverlay.cpp)
// pulls GfxRenderer/Page/HalStorage and cannot link on the native host, and the
// native env's build_src_filter is fixed. So — exactly like the SentenceSplitResult
// struct mirror above — this is a byte-for-byte copy of the production function kept
// in sync by hand; it lets the collapse RULE (the photo-verified bug) be asserted on
// the host. Keep identical to src/translator/TooltipOverlay.cpp::groupTranslationSteps.
struct TooltipStep {
  int16_t firstSentence;
  int16_t lastSentence;
};
static int groupTranslationSteps(const std::vector<std::string>& sentenceTranslations, TooltipStep* out, int maxSteps) {
  const int total = static_cast<int>(sentenceTranslations.size());
  int count = 0;
  int i = 0;
  while (i < total && count < maxSteps) {
    if (sentenceTranslations[i].empty()) {
      i++;
      continue;
    }
    int j = i;
    while (j + 1 < total && !sentenceTranslations[j + 1].empty() &&
           sentenceTranslations[j + 1] == sentenceTranslations[i]) {
      j++;
    }
    out[count].firstSentence = static_cast<int16_t>(i);
    out[count].lastSentence = static_cast<int16_t>(j);
    count++;
    i = j + 1;
  }
  return count;
}

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

// ── Ukrainian guillemet + separator regression (the photo-verified bug) ──────
//
// Reported paragraph fragment. The English original splits into 2 sentences;
// the Ukrainian translation must split into 2 as well so the tooltip maps them
// 1:1. The suspected culprit was the closing guillemet »-then-period (».), but
// that boundary IS recognized. The REAL culprit is a non-breaking space (or any
// Unicode space) used as the inter-sentence separator right after the period:
// the boundary scanner only accepted ASCII whitespace, so the Ukrainian side
// counted one sentence fewer and every later tooltip mapping shifted by one.

// « = U+00AB (C2 AB), » = U+00BB (C2 BB), NBSP = U+00A0 (C2 A0).
static const char* UK_GUILLEMET_ASCII =
    "\xD0\x94\xD0\xB2\xD1\x96 \xD0\xB4\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x96 "
    "\xD1\x81\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBE\xD0\xB2\xD1\x96 \xD0\xBA\xD0\xBB\xD1\x96\xD1\x82\xD0\xBA\xD0\xB8 "
    "\xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8 \xD0\xB2\xD0\xBD\xD0\xB8\xD0\xB7, \xD0\xBE\xD0\xB4\xD0\xBD\xD0\xB0 "
    "\xD0\xB4\xD0\xBE \xD0\xB6\xD0\xBE\xD0\xB2\xD1\x82\xD0\xBE\xD1\x97 \xD0\xBB\xD1\x96\xD0\xBD\xD1\x96\xD1\x97, "
    "\xD0\xB0 \xD1\x96\xD0\xBD\xD1\x88\xD0\xB0 \xD0\xB4\xD0\xBE "
    "\xC2\xAB\xD0\x9A\xD0\xBE\xD1\x88\xD0\xBC\xD0\xB0\xD1\x80\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE "
    "\xD0\xB5\xD0\xBA\xD1\x81\xD0\xBF\xD1\x80\xD0\xB5\xD1\x81\xD0\xB0\xC2\xBB. "
    "\xD0\xA2\xD1\x83\xD1\x82, \xD0\xBD\xD0\xB0\xD0\xB3\xD0\xBE\xD1\x80\xD1\x96, "
    "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xBE \xD1\x82\xD1\x80\xD0\xB8 "
    "\xD0\xBC\xD0\xB0\xD0\xB3\xD0\xB0\xD0\xB7\xD0\xB8\xD0\xBD\xD0\xB8.";

// First sentence text (through the closing guillemet + period) — the split point.
static const char* UK_FIRST_SENTENCE =
    "\xD0\x94\xD0\xB2\xD1\x96 \xD0\xB4\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x96 "
    "\xD1\x81\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBE\xD0\xB2\xD1\x96 \xD0\xBA\xD0\xBB\xD1\x96\xD1\x82\xD0\xBA\xD0\xB8 "
    "\xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8 \xD0\xB2\xD0\xBD\xD0\xB8\xD0\xB7, \xD0\xBE\xD0\xB4\xD0\xBD\xD0\xB0 "
    "\xD0\xB4\xD0\xBE \xD0\xB6\xD0\xBE\xD0\xB2\xD1\x82\xD0\xBE\xD1\x97 \xD0\xBB\xD1\x96\xD0\xBD\xD1\x96\xD1\x97, "
    "\xD0\xB0 \xD1\x96\xD0\xBD\xD1\x88\xD0\xB0 \xD0\xB4\xD0\xBE "
    "\xC2\xAB\xD0\x9A\xD0\xBE\xD1\x88\xD0\xBC\xD0\xB0\xD1\x80\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE "
    "\xD0\xB5\xD0\xBA\xD1\x81\xD0\xBF\xD1\x80\xD0\xB5\xD1\x81\xD0\xB0\xC2\xBB.";

static const char* EN_SOURCE =
    "Two additional stairwells led down, one to the yellow line and the other "
    "to the Nightmare Express. There were three shops up here.";

// The English source splits into exactly 2 sentences.
void test_countSentences_en_source_two(void) { TEST_ASSERT_EQUAL(2, countSentences(EN_SOURCE)); }

// Ukrainian with an ASCII-space separator: closing guillemet »-then-period is a
// recognized boundary (this is what the user *thought* was broken — it is not).
void test_countSentences_uk_guillemet_ascii(void) { TEST_ASSERT_EQUAL(2, countSentences(UK_GUILLEMET_ASCII)); }

// ...and the split lands right after «...експреса». (the closing guillemet+period).
void test_trimToSentences_uk_split_point(void) {
  TEST_ASSERT_EQUAL_STRING(UK_FIRST_SENTENCE, trimToSentences(UK_GUILLEMET_ASCII, 1).c_str());
}

// THE bug: same text, but the inter-sentence separator is a non-breaking space
// (U+00A0) instead of an ASCII space — exactly what the translation service /
// EPUB emits. Must STILL be 2 sentences. (Pre-fix this returned 1.)
void test_countSentences_uk_guillemet_nbsp(void) {
  std::string s = UK_FIRST_SENTENCE;
  s += "\xC2\xA0";  // NBSP separator right after the closing guillemet + period
  s += "\xD0\xA2\xD1\x83\xD1\x82, \xD0\xBD\xD0\xB0\xD0\xB3\xD0\xBE\xD1\x80\xD1\x96, "
       "\xD0\xB1\xD1\x83\xD0\xBB\xD0\xBE \xD1\x82\xD1\x80\xD0\xB8 "
       "\xD0\xBC\xD0\xB0\xD0\xB3\xD0\xB0\xD0\xB7\xD0\xB8\xD0\xBD\xD0\xB8.";
  TEST_ASSERT_EQUAL(2, countSentences(s));
}

// Same hole, minimal ASCII case: "Wort." + NBSP + "Next." must be 2 sentences.
void test_countSentences_nbsp_separator(void) { TEST_ASSERT_EQUAL(2, countSentences("Wort.\xC2\xA0Next.")); }

// Other Unicode inter-sentence separators the scanner must also honour.
void test_countSentences_narrow_nbsp_separator(void) {
  // U+202F NARROW NO-BREAK SPACE (E2 80 AF)
  TEST_ASSERT_EQUAL(2, countSentences("Wort.\xE2\x80\xAF"
                                      "Next."));
}
void test_countSentences_en_space_separator(void) {
  // U+2002 EN SPACE (E2 80 82)
  TEST_ASSERT_EQUAL(2, countSentences("Wort.\xE2\x80\x82"
                                      "Next."));
}
void test_countSentences_em_space_separator(void) {
  // U+2003 EM SPACE (E2 80 83)
  TEST_ASSERT_EQUAL(2, countSentences("Wort.\xE2\x80\x83"
                                      "Next."));
}

// The fix must not create false boundaries: a Unicode space with no preceding
// terminator is just whitespace, not a sentence break.
void test_countSentences_unicode_space_no_false_break(void) {
  // "Wort" + NBSP + "weiter." → one sentence.
  TEST_ASSERT_EQUAL(1, countSentences("Wort\xC2\xA0weiter."));
}

// trimToLastSentences must also see the NBSP boundary.
void test_trimToLastSentences_nbsp(void) {
  TEST_ASSERT_EQUAL_STRING("Next.", trimToLastSentences("Wort.\xC2\xA0Next.", 1).c_str());
}

// countSentencesBefore folds both sides first (NBSP→space), so it already saw
// the boundary — assert it stays correct after the fix.
void test_countSentencesBefore_nbsp(void) { TEST_ASSERT_EQUAL(1, countSentencesBefore("Wort.\xC2\xA0Next.", "Next.")); }

// ── Guillemet / quote edge cases (boundary must survive quote nesting) ───────
void test_countSentences_terminator_inside_guillemet(void) {
  // «Речення.» Далі. → 2 (period inside guillemets, closing » after it)
  TEST_ASSERT_EQUAL(2, countSentences("\xC2\xAB\xD0\xA0\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBD\xD1\x8F.\xC2\xBB "
                                      "\xD0\x94\xD0\xB0\xD0\xBB\xD1\x96."));
}
void test_countSentences_question_inside_guillemet(void) {
  // «Що?» Далі. → 2
  TEST_ASSERT_EQUAL(2, countSentences("\xC2\xAB\xD0\xA9\xD0\xBE?\xC2\xBB "
                                      "\xD0\x94\xD0\xB0\xD0\xBB\xD1\x96."));
}
void test_countSentences_ellipsis_inside_guillemet(void) {
  // «Слово…» Далі. → 2 (ellipsis terminates; closing » skipped)
  TEST_ASSERT_EQUAL(2, countSentences("\xC2\xAB\xD0\xA1\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xBE\xE2\x80\xA6\xC2\xBB "
                                      "\xD0\x94\xD0\xB0\xD0\xBB\xD1\x96."));
}
void test_countSentences_ellipsis_then_guillemet_nbsp(void) {
  // Слово…»<NBSP>Далі. → 2 (ellipsis + closing » + NBSP separator)
  TEST_ASSERT_EQUAL(2, countSentences("\xD0\xA1\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xBE\xE2\x80\xA6\xC2\xBB\xC2\xA0"
                                      "\xD0\x94\xD0\xB0\xD0\xBB\xD1\x96."));
}
// German reversed guillemets »…« (both fold to ") with NBSP separator → 2.
void test_countSentences_de_reversed_guillemets_nbsp(void) {
  TEST_ASSERT_EQUAL(2, countSentences("\xC2\xBB"
                                      "Wort.\xC2\xAB\xC2\xA0"
                                      "Weiter."));
}

// ── Word-array splitter (Tooltip path): existing behaviour must be preserved ──
static SentenceSplitResult splitWords(std::initializer_list<const char*> words) {
  const char* arr[TEST_MAX_SENTENCES];
  int n = 0;
  for (const char* w : words) arr[n++] = w;
  return splitSentences(arr, n);
}

// Abbreviations must NOT create a boundary (existing classifyTerminator logic).
void test_splitSentences_abbreviation_no_split(void) {
  SentenceSplitResult r = splitWords({"Dr.", "Smith", "arrived."});
  TEST_ASSERT_EQUAL(1, r.count);
}
// Sanity: two real sentences split into two spans.
void test_splitSentences_two_sentences(void) {
  SentenceSplitResult r = splitWords({"Hello.", "World."});
  TEST_ASSERT_EQUAL(2, r.count);
}
// Closing guillemet + period on a word is a boundary (the reported ». token).
void test_splitSentences_guillemet_period(void) {
  // "«експреса»." + "Тут." → 2 spans.
  SentenceSplitResult r =
      splitWords({"\xC2\xAB\xD0\xB5\xD0\xBA\xD1\x81\xD0\xBF\xD1\x80\xD0\xB5\xD1\x81\xD0\xB0\xC2\xBB.",
                  "\xD0\xA2\xD1\x83\xD1\x82."});
  TEST_ASSERT_EQUAL(2, r.count);
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

// ── Tooltip grouping: merged-sentence collapse (the photo-verified PT_TOOLTIP bug) ──
//
// Engine merged TWO English source sentences into ONE Ukrainian sentence, so both
// source sentences resolve to the SAME translation string. Pre-fix the tooltip showed
// that identical translation twice (once per underlined source sentence). Grouping must
// collapse them into ONE step whose span covers BOTH source sentences, drawn once.
static TooltipStep g_steps[TEST_MAX_SENTENCES];

// « = U+00AB (C2 AB), » = U+00BB (C2 BB). The merged Ukrainian sentence, joined with «і».
static const char* UK_MERGED =
    "\xC2\xAB\xD0\x9B\xD0\xB0\xD1\x81\xD0\xBA\xD0\xB0\xD0\xB2\xD0\xBE \xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD0\xB8\xD0\xBC"
    "\xD0\xBE \xD0\xBD\xD0\xB0 \xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD1\x86\xD1\x96\xD1\x8E 83\xC2\xBB, "
    "\xE2\x80\x94 \xD1\x81\xD0\xBA\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB2 \xD0\x9A\xD1\x83\xD0\xBB\xD1\x8C\xD0\xB3\xD0\xB0"
    "\xD0\xB2\xD0\xB8\xD0\xB9 \xD0\xA0\xD1\x96\xD1\x87\xD0\xB0\xD1\x80\xD0\xB4 \xD1\x96 \xD0\xB2\xD1\x96\xD0\xB4"
    "\xD0\xBA\xD0\xBB\xD0\xB0\xD0\xB2 \xD0\xBA\xD0\xBD\xD0\xB8\xD0\xB3\xD1\x83.";

// THE fix: two source sentences with the SAME translation → one step spanning [0,1].
void test_groupSteps_photo_case_two_into_one(void) {
  std::vector<std::string> tr = {UK_MERGED, UK_MERGED};  // both merged source sentences
  const int n = groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES);
  TEST_ASSERT_EQUAL(1, n);                         // ONE step, not two identical ones
  TEST_ASSERT_EQUAL(0, g_steps[0].firstSentence);  // span covers the FIRST source sentence...
  TEST_ASSERT_EQUAL(1, g_steps[0].lastSentence);   // ...through the SECOND — both underlined
}

// The merged span (contiguous partition) covers every word of both source sentences.
void test_groupSteps_photo_span_covers_both_sources(void) {
  // The English originals, tokenized as laid-out page words.
  SentenceSplitResult sp = splitWords(
      {"\"Welcome", "to", "station", "83,\"", "Limp", "Richard", "said.", "He", "put", "his", "book", "down."});
  TEST_ASSERT_EQUAL(2, sp.count);  // engine sees 2 English sentences (SentenceSplitter SSOT)

  std::vector<std::string> tr = {UK_MERGED, UK_MERGED};
  const int n = groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES);
  TEST_ASSERT_EQUAL(1, n);
  // Merged underline span = [spans[first].startWord, spans[last].endWord) = every word.
  const uint16_t startWord = sp.spans[g_steps[0].firstSentence].startWord;
  const uint16_t endWord = sp.spans[g_steps[0].lastSentence].endWord;
  TEST_ASSERT_EQUAL(0, startWord);
  TEST_ASSERT_EQUAL(12, endWord);  // all 12 words underlined as one unit
}

// Distinct translations must NOT collapse (normal 1:1 mapping is unchanged).
void test_groupSteps_distinct_no_merge(void) {
  std::vector<std::string> tr = {"Alpha.", "Beta."};
  const int n = groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL(0, g_steps[0].firstSentence);
  TEST_ASSERT_EQUAL(0, g_steps[0].lastSentence);
  TEST_ASSERT_EQUAL(1, g_steps[1].firstSentence);
  TEST_ASSERT_EQUAL(1, g_steps[1].lastSentence);
}

// Three consecutive source sentences merged into one translation → one step [0,2].
void test_groupSteps_three_way_merge(void) {
  std::vector<std::string> tr = {"Same.", "Same.", "Same."};
  const int n = groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(0, g_steps[0].firstSentence);
  TEST_ASSERT_EQUAL(2, g_steps[0].lastSentence);
}

// An untranslated sentence is not a step and terminates a run — even between equal
// translations (a page-boundary partial sentence must not be swallowed into a group).
void test_groupSteps_empty_breaks_run(void) {
  std::vector<std::string> tr = {"Same.", "", "Same."};
  const int n = groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL(0, g_steps[0].firstSentence);
  TEST_ASSERT_EQUAL(0, g_steps[0].lastSentence);
  TEST_ASSERT_EQUAL(2, g_steps[1].firstSentence);
  TEST_ASSERT_EQUAL(2, g_steps[1].lastSentence);
}

// Nothing translated on the page → zero steps (render shows the dim marker instead).
void test_groupSteps_all_empty_zero_steps(void) {
  std::vector<std::string> tr = {"", "", ""};
  TEST_ASSERT_EQUAL(0, groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES));
}

// Inverse case: one source sentence whose translation is the concatenation of several
// engine sentences is a single non-empty string → exactly one step (drawn once).
void test_groupSteps_one_source_concatenated_translation(void) {
  std::vector<std::string> tr = {"First part. Second part. Third part."};
  const int n = groupTranslationSteps(tr, g_steps, TEST_MAX_SENTENCES);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(0, g_steps[0].firstSentence);
  TEST_ASSERT_EQUAL(0, g_steps[0].lastSentence);
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();

  RUN_TEST(test_countSentences_empty);
  RUN_TEST(test_countSentences_single);
  RUN_TEST(test_countSentences_multi);
  RUN_TEST(test_countSentences_ellipsis_not_counted);
  RUN_TEST(test_countSentences_closing_quote);
  RUN_TEST(test_countSentences_no_terminator);

  RUN_TEST(test_countSentences_en_source_two);
  RUN_TEST(test_countSentences_uk_guillemet_ascii);
  RUN_TEST(test_trimToSentences_uk_split_point);
  RUN_TEST(test_countSentences_uk_guillemet_nbsp);
  RUN_TEST(test_countSentences_nbsp_separator);
  RUN_TEST(test_countSentences_narrow_nbsp_separator);
  RUN_TEST(test_countSentences_en_space_separator);
  RUN_TEST(test_countSentences_em_space_separator);
  RUN_TEST(test_countSentences_unicode_space_no_false_break);
  RUN_TEST(test_trimToLastSentences_nbsp);
  RUN_TEST(test_countSentencesBefore_nbsp);
  RUN_TEST(test_countSentences_terminator_inside_guillemet);
  RUN_TEST(test_countSentences_question_inside_guillemet);
  RUN_TEST(test_countSentences_ellipsis_inside_guillemet);
  RUN_TEST(test_countSentences_ellipsis_then_guillemet_nbsp);
  RUN_TEST(test_countSentences_de_reversed_guillemets_nbsp);
  RUN_TEST(test_splitSentences_abbreviation_no_split);
  RUN_TEST(test_splitSentences_two_sentences);
  RUN_TEST(test_splitSentences_guillemet_period);

  RUN_TEST(test_groupSteps_photo_case_two_into_one);
  RUN_TEST(test_groupSteps_photo_span_covers_both_sources);
  RUN_TEST(test_groupSteps_distinct_no_merge);
  RUN_TEST(test_groupSteps_three_way_merge);
  RUN_TEST(test_groupSteps_empty_breaks_run);
  RUN_TEST(test_groupSteps_all_empty_zero_steps);
  RUN_TEST(test_groupSteps_one_source_concatenated_translation);

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
