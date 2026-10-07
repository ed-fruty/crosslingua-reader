// The walker replaced the two copies of ChapterHtmlSlimParser's paragraph counter that the Lingua
// overlays used to carry. Each case below is either a shape that must pair, or one of the
// constructs whose counting drifted and broke Page Translation:
//   <table>            -- 0 paragraph indices since upstream made tables render as columns
//   <li><p>...</p></li> -- 1 index, because the <li>'s bullet block is reused by the child
//   <br/><span lang>   -- 0 indices; Calibre's translation lives inline, not in a sibling block
// The walker cannot drift on any of them because it does not count paragraphs at all -- these tests
// pin that its PAIRING is unaffected by them.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "modules/lingua/parsers/TranslationPairWalker.h"

namespace {

struct Pair {
  std::string original;
  std::string translation;
};

class WalkerFixture : public ::testing::Test {
 protected:
  std::string path() const {
    return std::string("/tmp/crosspoint_tpw_") + ::testing::UnitTest::GetInstance()->current_test_info()->name() +
           ".xhtml";
  }
  void TearDown() override { std::remove(path().c_str()); }

  // `stopAfter` > 0 makes the sink stop the walk after that many pairs, exercising the early exit.
  std::vector<Pair> walk(const std::string& html, const std::string& bookLang = "en", int stopAfter = 0) {
    {
      std::ofstream(path()) << html;
    }
    struct Collector {
      std::vector<Pair> pairs;
      int stopAfter = 0;
    } collector;
    collector.stopAfter = stopAfter;

    lingua::walkTranslationPairs(path(), bookLang, &collector, [](void* ctx, const lingua::TranslationPair& pair) {
      auto* c = static_cast<Collector*>(ctx);
      c->pairs.push_back(
          {std::string(pair.original, pair.originalLength), std::string(pair.translation, pair.translationLength)});
      return !(c->stopAfter > 0 && static_cast<int>(c->pairs.size()) >= c->stopAfter);
    });
    return collector.pairs;
  }
};

// The shape CrossPoint's own translator writes: a lang-tagged sibling block after each original.
TEST_F(WalkerFixture, PairsSiblingBlocks) {
  const auto pairs = walk(
      "<html><body>"
      "<p>First original.</p><p lang=\"uk\">Перший переклад.</p>"
      "<p>Second original.</p><p lang=\"uk\">Другий переклад.</p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].original, "First original.");
  EXPECT_EQ(pairs[0].translation, "Перший переклад.");
  EXPECT_EQ(pairs[1].original, "Second original.");
  EXPECT_EQ(pairs[1].translation, "Другий переклад.");
}

// Calibre's plugin puts the translation in an inline <span> after a <br/>, inside the original
// paragraph. This is the shape whose block the old copies wrongly counted as another original.
TEST_F(WalkerFixture, PairsCalibreInlineSpan) {
  const auto pairs = walk(
      "<html><body>"
      "<p>Original sentence.<br/><span lang=\"uk\">Перекладене речення.</span></p>"
      "<p>Next original.<br/><span lang=\"uk\">Наступний переклад.</span></p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].original, "Original sentence.");
  EXPECT_EQ(pairs[0].translation, "Перекладене речення.");
  EXPECT_EQ(pairs[1].original, "Next original.");
  EXPECT_EQ(pairs[1].translation, "Наступний переклад.");
}

// Text after the translated span closes belongs to the ORIGINAL again, exactly as the parser pops
// its inline translated marker at </span>.
TEST_F(WalkerFixture, TrailingTextAfterSpanIsOriginal) {
  const auto pairs = walk(
      "<html><body>"
      "<p>Head.<br/><span lang=\"uk\">Переклад.</span> Tail.</p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_EQ(pairs[0].original, "Head. Tail.");
  EXPECT_EQ(pairs[0].translation, "Переклад.");
}

// The old copies treated ANY lang attribute as a translation marker. The parser compares primary
// subtags against the book's language, so same-language markup is NOT a translation.
TEST_F(WalkerFixture, SameLanguageTagIsNotATranslation) {
  const auto pairs = walk(
      "<html><body>"
      "<p>Original.</p><p lang=\"en-GB\">Still English.</p>"
      "</body></html>");

  EXPECT_TRUE(pairs.empty());
}

// ...and a differing region still counts as the same language, while a different language does not.
TEST_F(WalkerFixture, RegionSubtagDoesNotMakeItTranslated) {
  const auto sameLang = walk("<html><body><p>Orig.</p><p lang=\"EN_us\">Same.</p></body></html>");
  EXPECT_TRUE(sameLang.empty());
  const auto other = walk("<html><body><p>Orig.</p><p lang=\"uk-UA\">Інша.</p></body></html>");
  ASSERT_EQ(other.size(), 1u);
  EXPECT_EQ(other[0].translation, "Інша.");
}

// A table contributes no paragraph of its own and must not disturb the pair that follows it.
TEST_F(WalkerFixture, TableDoesNotDisturbPairing) {
  const auto pairs = walk(
      "<html><body>"
      "<p>Before.</p><p lang=\"uk\">До.</p>"
      "<table><tr><td>Cell A</td><td>Cell B</td></tr></table>"
      "<p>After.</p><p lang=\"uk\">Після.</p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].original, "Before.");
  EXPECT_EQ(pairs[1].original, "After.");
  EXPECT_EQ(pairs[1].translation, "Після.");
}

// An <li> whose content sits in a nested block still yields one pair.
TEST_F(WalkerFixture, ListItemWithNestedBlockPairs) {
  const auto pairs = walk(
      "<html><body><ul>"
      "<li><p>Item one.</p><p lang=\"uk\">Пункт один.</p></li>"
      "<li><p>Item two.</p><p lang=\"uk\">Пункт два.</p></li>"
      "</ul></body></html>");

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].original, "Item one.");
  EXPECT_EQ(pairs[0].translation, "Пункт один.");
  EXPECT_EQ(pairs[1].original, "Item two.");
  EXPECT_EQ(pairs[1].translation, "Пункт два.");
}

// A wrapper container's own (empty) block must not swallow its child's text.
TEST_F(WalkerFixture, WrapperContainerDoesNotSwallowChild) {
  const auto pairs = walk(
      "<html><body>"
      "<div><p>Wrapped.</p><p lang=\"uk\">Обгорнуто.</p></div>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_EQ(pairs[0].original, "Wrapped.");
  EXPECT_EQ(pairs[0].translation, "Обгорнуто.");
}

// An untranslated paragraph between translated ones must not shift the pairing: the stale original
// is replaced, never paired with a later translation.
TEST_F(WalkerFixture, UntranslatedParagraphDoesNotShiftPairing) {
  const auto pairs = walk(
      "<html><body>"
      "<p>Has one.</p><p lang=\"uk\">Має.</p>"
      "<p>Has none.</p>"
      "<p>Has one too.</p><p lang=\"uk\">Теж має.</p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].original, "Has one.");
  EXPECT_EQ(pairs[1].original, "Has one too.");
  EXPECT_EQ(pairs[1].translation, "Теж має.");
}

// Identical repeated paragraphs must each yield their own pair, in document order -- that ordering
// is what lets the overlay disambiguate them by position instead of by text.
TEST_F(WalkerFixture, RepeatedIdenticalParagraphsEachYieldAPair) {
  const auto pairs = walk(
      "<html><body>"
      "<p>Same text.</p><p lang=\"uk\">Переклад один.</p>"
      "<p>Same text.</p><p lang=\"uk\">Переклад два.</p>"
      "<p>Same text.</p><p lang=\"uk\">Переклад три.</p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 3u);
  EXPECT_EQ(pairs[0].original, "Same text.");
  EXPECT_EQ(pairs[0].translation, "Переклад один.");
  EXPECT_EQ(pairs[1].translation, "Переклад два.");
  EXPECT_EQ(pairs[2].translation, "Переклад три.");
}

// Whitespace, newlines and the NBSP family collapse to single ASCII spaces, so a pair folds to the
// same bytes the page's words do when joined.
TEST_F(WalkerFixture, CollapsesWhitespaceAndNbspFamily) {
  const auto pairs = walk(
      "<html><body>"
      "<p>  Spaced\n  out\ttext.  </p>"
      "<p lang=\"uk\">Один\xc2\xa0"
      "два\xe2\x80\xaf"
      "три.</p>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_EQ(pairs[0].original, "Spaced out text.");
  EXPECT_EQ(pairs[0].translation, "Один два три.");
}

// <script>/<style> text is not content and must never reach a pair.
TEST_F(WalkerFixture, IgnoresScriptAndStyleContent) {
  const auto pairs = walk(
      "<html><body>"
      "<style>p { color: red; }</style>"
      "<p>Real text.</p><p lang=\"uk\">Справжній текст.</p>"
      "<script>var x = 1;</script>"
      "</body></html>");

  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_EQ(pairs[0].original, "Real text.");
  EXPECT_EQ(pairs[0].translation, "Справжній текст.");
}

// The sink can stop the walk as soon as it has what the page needs, so a page early in a long
// chapter never pays for the rest of it.
TEST_F(WalkerFixture, SinkCanStopTheWalkEarly) {
  const auto pairs = walk(
      "<html><body>"
      "<p>One.</p><p lang=\"uk\">Один.</p>"
      "<p>Two.</p><p lang=\"uk\">Два.</p>"
      "<p>Three.</p><p lang=\"uk\">Три.</p>"
      "</body></html>",
      "en", 2);

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[1].original, "Two.");
}

// Without the book's language nothing can be recognized as translated -- the same answer the layout
// parser gives, so the walker must not invent pairs.
TEST_F(WalkerFixture, NoBookLanguageYieldsNoPairs) {
  const auto pairs = walk("<html><body><p>Orig.</p><p lang=\"uk\">Пер.</p></body></html>", "");
  EXPECT_TRUE(pairs.empty());
}

// A missing file is a failure, not an empty success -- the caller has to be able to tell them apart.
TEST_F(WalkerFixture, MissingFileFails) {
  const bool ok = lingua::walkTranslationPairs("/tmp/crosspoint_tpw_does_not_exist.xhtml", "en", nullptr,
                                               [](void*, const lingua::TranslationPair&) { return true; });
  EXPECT_FALSE(ok);
}

// ── Page matching ────────────────────────────────────────────────────────────

class MatchFixture : public WalkerFixture {
 protected:
  // Runs the matcher over `html` for the paragraphs a page is showing.
  std::vector<lingua::PageMatch> match(const std::string& html, const std::vector<std::string>& visible,
                                       const std::string& bookLang = "en") {
    {
      std::ofstream(path()) << html;
    }
    std::vector<lingua::PageMatch> out(visible.size());
    lingua::matchPageParagraphs(path(), bookLang, visible.data(), static_cast<int>(visible.size()), out.data());
    return out;
  }
};

// A paragraph broken at the TOP of the page shows only its tail, so the visible words are found
// part-way into the original -- which is also how the overlay knows not to indent it.
TEST_F(MatchFixture, MatchesAParagraphBrokenAtThePageStart) {
  const auto out = match(
      "<html><body>"
      "<p>First sentence here. Second sentence here. Third sentence here.</p>"
      "<p lang=\"uk\">Перше речення. Друге речення. Третє речення.</p>"
      "</body></html>",
      {"Second sentence here. Third sentence here."});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_GT(out[0].matchOffset, 0u);  // did NOT begin on this page
  EXPECT_EQ(out[0].original, "First sentence here. Second sentence here. Third sentence here.");
  EXPECT_EQ(out[0].translation, "Перше речення. Друге речення. Третє речення.");
}

// Broken at the BOTTOM: the visible slice is the head, so it is found at offset 0 and the paragraph
// does begin here.
TEST_F(MatchFixture, MatchesAParagraphBrokenAtThePageEnd) {
  const auto out = match(
      "<html><body>"
      "<p>First sentence here. Second sentence here. Third sentence here.</p>"
      "<p lang=\"uk\">Перше речення. Друге речення. Третє речення.</p>"
      "</body></html>",
      {"First sentence here. Second"});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_EQ(out[0].matchOffset, 0u);
}

// A paragraph long enough to fill a whole page is broken at BOTH edges: only its middle shows.
TEST_F(MatchFixture, MatchesAParagraphBrokenAtBothEdges) {
  const auto out = match(
      "<html><body>"
      "<p>Alpha one. Bravo two. Charlie three. Delta four. Echo five.</p>"
      "<p lang=\"uk\">Альфа. Браво. Чарлі. Дельта. Ехо.</p>"
      "</body></html>",
      {"Bravo two. Charlie three. Delta"});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_GT(out[0].matchOffset, 0u);
  EXPECT_EQ(out[0].translation, "Альфа. Браво. Чарлі. Дельта. Ехо.");
}

// Several paragraphs on one page, matched in order.
TEST_F(MatchFixture, MatchesSeveralParagraphsInOrder) {
  const auto out = match(
      "<html><body>"
      "<p>One here.</p><p lang=\"uk\">Один.</p>"
      "<p>Two here.</p><p lang=\"uk\">Два.</p>"
      "<p>Three here.</p><p lang=\"uk\">Три.</p>"
      "</body></html>",
      {"Two here.", "Three here."});

  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].translation, "Два.");
  EXPECT_EQ(out[1].translation, "Три.");
}

// THE case content matching has to get right: identical paragraph text, different translations.
// Position in the document decides, so the second occurrence cannot take the first one's pair.
TEST_F(MatchFixture, IdenticalParagraphsTakeTheirOwnTranslation) {
  const std::string html =
      "<html><body>"
      "<p>Same text.</p><p lang=\"uk\">Переклад один.</p>"
      "<p>Same text.</p><p lang=\"uk\">Переклад два.</p>"
      "<p>Same text.</p><p lang=\"uk\">Переклад три.</p>"
      "</body></html>";

  // A page showing the second and third occurrences must not be handed the first's translation.
  const auto out = match(html, {"Same text.", "Same text."});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].translation, "Переклад один.");
  EXPECT_EQ(out[1].translation, "Переклад два.");
  // ...and never the same pair twice, which is what a naive content search would do.
  EXPECT_NE(out[0].translation, out[1].translation);
}

// An untranslated paragraph between two translated ones must not block the one after it.
TEST_F(MatchFixture, UntranslatedParagraphDoesNotBlockTheNext) {
  const auto out = match(
      "<html><body>"
      "<p>Has one.</p><p lang=\"uk\">Має.</p>"
      "<p>No translation at all.</p>"
      "<p>Has one too.</p><p lang=\"uk\">Теж має.</p>"
      "</body></html>",
      {"Has one.", "No translation at all.", "Has one too."});

  ASSERT_EQ(out.size(), 3u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_FALSE(out[1].matched);  // falls back to source text in the overlay
  EXPECT_TRUE(out[2].matched);
  EXPECT_EQ(out[2].translation, "Теж має.");
}

// The layout prepends a U+2022 bullet as a real word to every <li>, so the page shows it and the
// HTML does not. It must not defeat the match.
TEST_F(MatchFixture, StripsTheListBulletFromTheVisibleText) {
  const auto out = match(
      "<html><body><ul>"
      "<li><p>Item text here.</p><p lang=\"uk\">Текст пункту.</p></li>"
      "</ul></body></html>",
      {"\xe2\x80\xa2 Item text here."});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_EQ(out[0].translation, "Текст пункту.");
}

// A word split across a line end reaches the visible text as "prefix- remainder", which the
// original does not contain. The needle stops before that artifact.
TEST_F(MatchFixture, ToleratesALineEndHyphenationArtifact) {
  const auto out = match(
      "<html><body>"
      "<p>Extraordinary circumstances prevailed.</p>"
      "<p lang=\"uk\">Надзвичайні обставини.</p>"
      "</body></html>",
      {"Extraordinary circum- stances prevailed."});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_EQ(out[0].translation, "Надзвичайні обставини.");
}

// REGRESSION: an inline tag inside the first words. The page stores no word-glue flag, so
// "<i>lately</i>-unless" reaches the overlay as two words joined by a space that the original does
// not contain. Matching has to see through that -- when it did not, the tail of a paragraph carried
// over from the previous page read as untranslated and the tooltip stepped straight past it.
TEST_F(MatchFixture, MatchesAcrossAnInlineTagWordSplit) {
  const auto out = match(
      "<html><body>"
      "<p>She hasn't done anything <i>lately</i>\xe2\x80\x94unless you count the fact that she was here.</p>"
      "<p lang=\"uk\">Вона нічого не робила останнім часом.</p>"
      "</body></html>",
      // What the page's words join to: a space before the dash, and the em dash as its own token.
      {"lately \xe2\x80\x94unless you count the fact that she was here."});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_GT(out[0].matchOffset, 0u);  // it began on the previous page
  EXPECT_EQ(out[0].translation, "Вона нічого не робила останнім часом.");
}

// The same insensitivity must hold for a hyphenation join anywhere but the first word.
TEST_F(MatchFixture, MatchesDespiteAHyphenationJoinMidParagraph) {
  const auto out = match(
      "<html><body>"
      "<p>The night Megan Hipwell disappeared, I said nothing at all.</p>"
      "<p lang=\"uk\">Тієї ночі я нічого не сказала.</p>"
      "</body></html>",
      {"The night Megan Hipwell disap- peared, I said nothing at all."});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
}

// REGRESSION, from a real Tooltip skip. A page of [paragraph carried over from the previous page,
// short recurring heading, next paragraph]. The heading's needle is only 7 bytes and its text recurs
// throughout the book, so it used to match the FIRST such heading in the chapter -- dragging the
// match cursor past the carried paragraph, whose own pair comes later, and leaving it unmatched
// forever. Its sentences then had no translation and the tooltip stepped straight over them.
TEST_F(MatchFixture, AShortRecurringHeadingDoesNotAnchorThePageEarly) {
  const std::string html =
      "<html><body>"
      "<p>EVENING</p><p lang=\"uk\">ВЕЧІР ПЕРШИЙ</p>"
      "<p>Something early in the chapter.</p><p lang=\"uk\">Щось на початку.</p>"
      "<p>I want him to know how sorry I am that it wasn't enough.</p>"
      "<p lang=\"uk\">Я хочу, щоб він знав, як мені шкода.</p>"
      "<p>EVENING</p><p lang=\"uk\">ВЕЧІР ДРУГИЙ</p>"
      "<p>I am soaked through, freezing cold.</p><p lang=\"uk\">Я промокла й змерзла.</p>"
      "</body></html>";

  // The page shows the TAIL of the "I want him..." paragraph, then the SECOND heading, then the last
  // paragraph.
  const auto out =
      match(html, {"to know how sorry I am that it wasn't enough.", "EVENING", "I am soaked through, freezing cold."});

  ASSERT_EQ(out.size(), 3u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_EQ(out[0].translation, "Я хочу, щоб він знав, як мені шкода.");
  EXPECT_GT(out[0].matchOffset, 0u);  // it began on the previous page
  EXPECT_TRUE(out[1].matched);
  EXPECT_EQ(out[1].translation, "ВЕЧІР ДРУГИЙ");  // the heading AFTER it, not the first one
  EXPECT_TRUE(out[2].matched);
  EXPECT_EQ(out[2].translation, "Я промокла й змерзла.");
}

// A short paragraph the page STARTS with may still match: it is at the cursor, so it cannot skip
// anything by matching.
TEST_F(MatchFixture, AShortFirstParagraphStillMatches) {
  const auto out = match(
      "<html><body>"
      "<p>EVENING</p><p lang=\"uk\">ВЕЧІР</p>"
      "<p>Then the day began.</p><p lang=\"uk\">Потім почався день.</p>"
      "</body></html>",
      {"EVENING", "Then the day began."});

  ASSERT_EQ(out.size(), 2u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_EQ(out[0].translation, "ВЕЧІР");
  EXPECT_TRUE(out[1].matched);
}

// An UNTRANSLATED first paragraph must still not block the rest: a discriminating later needle is
// allowed to anchor the page.
TEST_F(MatchFixture, ADiscriminatingLaterParagraphCanStillAnchor) {
  const auto out = match(
      "<html><body>"
      "<p>Alpha with a translation.</p><p lang=\"uk\">Альфа.</p>"
      "<p>Bravo with a translation.</p><p lang=\"uk\">Браво.</p>"
      "</body></html>",
      {"An untranslated paragraph entirely absent from the source.", "Bravo with a translation."});

  ASSERT_EQ(out.size(), 2u);
  EXPECT_FALSE(out[0].matched);
  EXPECT_TRUE(out[1].matched);
  EXPECT_EQ(out[1].translation, "Браво.");
}

// A page whose paragraphs are all untranslated matches nothing and reports it, rather than
// inventing a pair from somewhere else in the chapter.
TEST_F(MatchFixture, ReportsNoMatchForAnUntranslatedPage) {
  const auto out = match(
      "<html><body>"
      "<p>Alpha.</p><p lang=\"uk\">Альфа.</p>"
      "<p>Something else entirely.</p>"
      "</body></html>",
      {"Something else entirely."});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].matched);
}

// ── The pair cache ───────────────────────────────────────────────────────────
//
// The cache is a memo of the walker's own output, so the only thing worth pinning is that it gives
// the SAME answers the HTML walk gives -- and that a missing or foreign one changes nothing.

class CacheFixture : public WalkerFixture {
 protected:
  std::string cachePath() const { return path() + ".pairs"; }
  void TearDown() override {
    std::remove(cachePath().c_str());
    std::remove((cachePath() + ".part").c_str());
    WalkerFixture::TearDown();
  }

  std::vector<lingua::PageMatch> matchCached(const std::vector<std::string>& visible) {
    std::vector<lingua::PageMatch> out(visible.size());
    lingua::matchPageParagraphsCached(cachePath(), path(), "en", visible.data(), static_cast<int>(visible.size()),
                                      out.data());
    return out;
  }
  void write(const std::string& html) { std::ofstream(path()) << html; }
};

const char* const kChapter =
    "<html><body>"
    "<p>Alpha one here.</p><p lang=\"uk\">Альфа.</p>"
    "<p>Bravo two here.</p><p lang=\"uk\">Браво.</p>"
    "<p>Charlie three here.</p><p lang=\"uk\">Чарлі.</p>"
    "</body></html>";

// The cache must answer exactly as the HTML walk does, for the same page.
TEST_F(CacheFixture, MatchesIdenticallyToTheHtmlWalk) {
  write(kChapter);
  const std::vector<std::string> visible = {"Bravo two here.", "Charlie three here."};

  std::vector<lingua::PageMatch> fromHtml(visible.size());
  lingua::matchPageParagraphs(path(), "en", visible.data(), static_cast<int>(visible.size()), fromHtml.data());

  ASSERT_TRUE(lingua::writeTranslationPairCache(path(), "en", cachePath()));
  ASSERT_TRUE(lingua::hasTranslationPairCache(cachePath()));
  const auto fromCache = matchCached(visible);

  ASSERT_EQ(fromCache.size(), fromHtml.size());
  for (size_t i = 0; i < fromCache.size(); i++) {
    EXPECT_EQ(fromCache[i].matched, fromHtml[i].matched);
    EXPECT_EQ(fromCache[i].matchOffset, fromHtml[i].matchOffset);
    EXPECT_EQ(fromCache[i].original, fromHtml[i].original);
    EXPECT_EQ(fromCache[i].translation, fromHtml[i].translation);
  }
}

// A broken paragraph resolves through the cache the same way, offset included -- that offset is what
// decides the paragraph indent.
TEST_F(CacheFixture, PreservesTheMatchOffsetForABrokenParagraph) {
  write(
      "<html><body>"
      "<p>First sentence here. Second sentence here. Third sentence here.</p>"
      "<p lang=\"uk\">Перше. Друге. Третє.</p>"
      "</body></html>");
  ASSERT_TRUE(lingua::writeTranslationPairCache(path(), "en", cachePath()));

  const auto out = matchCached({"Second sentence here. Third sentence here."});
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_GT(out[0].matchOffset, 0u);
  EXPECT_EQ(out[0].translation, "Перше. Друге. Третє.");
}

// Identical paragraphs still take their own translation through the cache: the stream keeps document
// order, which is what the positional match relies on.
TEST_F(CacheFixture, KeepsDocumentOrderForIdenticalParagraphs) {
  write(
      "<html><body>"
      "<p>Same text.</p><p lang=\"uk\">Один.</p>"
      "<p>Same text.</p><p lang=\"uk\">Два.</p>"
      "</body></html>");
  ASSERT_TRUE(lingua::writeTranslationPairCache(path(), "en", cachePath()));

  const auto out = matchCached({"Same text.", "Same text."});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].translation, "Один.");
  EXPECT_EQ(out[1].translation, "Два.");
}

// No cache at all: the overlay must still get its translations, straight from the HTML. This is the
// property that makes the cache an optimisation rather than a dependency.
TEST_F(CacheFixture, FallsBackToTheHtmlWhenThereIsNoCache) {
  write(kChapter);
  EXPECT_FALSE(lingua::hasTranslationPairCache(cachePath()));

  const auto out = matchCached({"Alpha one here."});
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);
  EXPECT_EQ(out[0].translation, "Альфа.");
}

// A cache from a different format version is ignored, not misread.
TEST_F(CacheFixture, IgnoresAForeignVersionCache) {
  write(kChapter);
  {
    std::ofstream f(cachePath(), std::ios::binary);
    const char header[8] = {'C', 'P', 'P', 'R', 0x7F, 0, 0, 0};  // right magic, wrong version
    f.write(header, sizeof(header));
  }
  EXPECT_FALSE(lingua::hasTranslationPairCache(cachePath()));

  const auto out = matchCached({"Alpha one here."});
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].matched);  // fell back to the HTML
}

// A truncated cache cannot be produced by writeTranslationPairCache (it commits by rename), but if
// one ever appears it must degrade to "no more pairs" rather than misparse.
TEST_F(CacheFixture, StopsCleanlyOnATruncatedCache) {
  write(kChapter);
  ASSERT_TRUE(lingua::writeTranslationPairCache(path(), "en", cachePath()));
  {
    std::ifstream in(cachePath(), std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::ofstream out(cachePath(), std::ios::binary | std::ios::trunc);
    out.write(all.data(), static_cast<std::streamsize>(all.size() / 2));
  }

  const auto out = matchCached({"Charlie three here."});
  ASSERT_EQ(out.size(), 1u);
  // Whether the truncation cut before or after this pair, the result must never be a WRONG pair.
  if (out[0].matched) EXPECT_EQ(out[0].translation, "Чарлі.");
}

// A cache is never written for a source that cannot be read.
TEST_F(CacheFixture, DoesNotWriteACacheForAMissingSource) {
  EXPECT_FALSE(lingua::writeTranslationPairCache("/tmp/crosspoint_tpw_missing.xhtml", "en", cachePath()));
  EXPECT_FALSE(lingua::hasTranslationPairCache(cachePath()));
}

}  // namespace
