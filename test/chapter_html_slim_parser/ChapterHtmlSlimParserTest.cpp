#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(false); }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

// ── Paragraph indexing ───────────────────────────────────────────────────────
//
// PageLine::paragraphIdx is the ONLY paragraph numbering in the system (the Lingua overlays match by
// text, not by index), and it is what groups a page's lines into paragraphs. Two constructs used to
// alias it: a <table> left the index latch set, so the paragraph after a table shared the index of
// the one before it, and an <img> alt-text fallback claimed no index at all and kept the previous
// paragraph's.

class ParagraphIndexTest : public ::testing::Test {
 protected:
  std::string filepath;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};

  std::string htmlPath() const {
    return std::string("/tmp/crosspoint_paraidx_") + ::testing::UnitTest::GetInstance()->current_test_info()->name() +
           ".xhtml";
  }
  void SetUp() override { filepath = htmlPath(); }
  void TearDown() override { std::remove(htmlPath().c_str()); }

  // Lays `html` out and returns how many paragraph indices the chapter consumed.
  int paragraphCount(const std::string& html) {
    {
      std::ofstream(htmlPath()) << html;
    }
    ChapterHtmlSlimParser parser{nullptr,
                                 filepath,
                                 renderer,
                                 0,
                                 1.0f,
                                 false,
                                 0,
                                 static_cast<uint16_t>(renderer.getScreenWidth()),
                                 static_cast<uint16_t>(renderer.getScreenHeight()),
                                 false,
                                 false,
                                 [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) {},
                                 true,
                                 "",
                                 "",
                                 0,
                                 {},
                                 nullptr,
                                 &cssParser};
    EXPECT_TRUE(parser.beginParse());
    for (int guard = 0; guard < 10000; guard++) {
      const auto status = parser.parseStep();
      EXPECT_NE(status, ChapterHtmlSlimParser::ParseStatus::Error);
      if (status != ChapterHtmlSlimParser::ParseStatus::More) break;
    }
    parser.finishParse();
    return parser.paragraphCounter;
  }
};

// The paragraph after a table must get an index of its own. A table itself consumes none -- its rows
// inherit the index of the paragraph before it -- but it must not stop the NEXT one from claiming.
TEST_F(ParagraphIndexTest, ParagraphAfterATableClaimsItsOwnIndex) {
  EXPECT_EQ(paragraphCount("<html><body>"
                           "<p>Before the table.</p>"
                           "<table><tr><td>Cell A</td><td>Cell B</td></tr></table>"
                           "<p>After the table.</p>"
                           "</body></html>"),
            2);
}

// ...including across several tables, where the leak used to collapse every intervening paragraph.
TEST_F(ParagraphIndexTest, ParagraphsBetweenTablesEachClaimAnIndex) {
  EXPECT_EQ(paragraphCount("<html><body>"
                           "<p>One.</p>"
                           "<table><tr><td>A</td></tr></table>"
                           "<p>Two.</p>"
                           "<table><tr><td>B</td></tr></table>"
                           "<p>Three.</p>"
                           "</body></html>"),
            3);
}

// An undecodable image with alt text lays that text out as its own paragraph (the host harness
// reports every format unsupported, which is the fallback path), so it claims its own index.
TEST_F(ParagraphIndexTest, ImageAltTextClaimsItsOwnIndex) {
  EXPECT_EQ(paragraphCount("<html><body>"
                           "<p>Before.</p>"
                           "<img src=\"figure.gif\" alt=\"A figure\"/>"
                           "<p>After.</p>"
                           "</body></html>"),
            3);
}

// An image with no alt text lays out nothing, so it consumes no index either.
TEST_F(ParagraphIndexTest, ImageWithoutAltConsumesNoIndex) {
  EXPECT_EQ(paragraphCount("<html><body>"
                           "<p>Before.</p>"
                           "<img src=\"figure.gif\"/>"
                           "<p>After.</p>"
                           "</body></html>"),
            2);
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

// LinguaLayout::Interlinear: an annotation longer than the strips its own sentence owns must not
// lose its tail -- it is CARRIED in the paragraph stream for the next sentence to place. The stub
// renderer measures 8px per character for EVERY font id, so here the annotation face is exactly as
// wide as the body face: the worst case on real hardware, and the one a larger Annotation Size
// setting approaches.
//
// The assertions read the STREAM rather than the emitted rows on purpose: this harness link-stubs
// TextBlock's constructor (ParserLinkStubs.cpp), so a laid-out row always reports wordCount() == 0
// and cannot be counted. What the stream shows is exactly the contract that changed -- how much text
// a call consumed, and how much it handed on.
class InterlinearCarryTest : public ::testing::Test {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  // 6 chars * 8px + a 4px gap = 52px per word, so a 480px strip holds 9 and a 30-word annotation
  // needs four of them.
  static constexpr uint16_t WORD_COUNT = 30;
  static constexpr uint16_t WORDS_PER_STRIP = 9;

  ParsedText makeTranslation() const {
    ParsedText block(/*extraParagraphSpacing=*/false);
    for (uint16_t i = 0; i < WORD_COUNT; i++) {
      block.addWord("abcdef", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false);
    }
    return block;
  }

  ParsedText makeStream() const {
    BlockStyle annStyle;
    annStyle.alignment = CssTextAlign::Left;
    annStyle.textIndent = 0;
    annStyle.textIndentDefined = true;
    return ParsedText(/*extraParagraphSpacing=*/false, /*hyphenationEnabled=*/false,
                      /*focusReadingEnabled=*/false, annStyle);
  }

  ChapterHtmlSlimParser::InterlinearBands oneStripBand() const {
    ChapterHtmlSlimParser::InterlinearBands bands;
    bands.srcLines = nullptr;  // srcLineStart() reads 0 for a null list, i.e. a left-aligned block
    bands.firstLine = 0;
    bands.slots = 1;
    bands.headX = 0;
    bands.tailX = static_cast<int16_t>(renderer.getScreenWidth());
    bands.floorX = 0;
    return bands;
  }

  void run(const InterlinearAnnotation& annotation, const ParsedText& translation,
           const ChapterHtmlSlimParser::InterlinearBands& bands, ParsedText& stream) {
    std::vector<ChapterHtmlSlimParser::InterlinearRun> runs;
    parser.buildAnnotationRuns(annotation, translation, bands, static_cast<uint16_t>(renderer.getScreenWidth()), 1,
                               runs, stream);
  }
};

// A one-line sentence whose translation needs four strips: the call places what one strip holds and
// hands the rest on instead of dropping it.
TEST_F(InterlinearCarryTest, LeavesTheUnplacedTailInTheStream) {
  const ParsedText translation = makeTranslation();
  ASSERT_EQ(translation.size(), static_cast<size_t>(WORD_COUNT));  // the fixture itself must be sound

  ParsedText stream = makeStream();
  run(InterlinearAnnotation{0, 0, WORD_COUNT}, translation, oneStripBand(), stream);

  EXPECT_EQ(stream.size(), static_cast<size_t>(WORD_COUNT - WORDS_PER_STRIP));
}

// REGRESSION, found on device: a strip too cramped for the next token must be SKIPPED, not consumed.
// extractNextLine consumes inside itself, so extracting a row and then rejecting it as too wide
// destroyed those words -- the carry cannot requeue what has already left the stream. Here floorX
// leaves 76px of a 480px strip and the only token needs 160px.
TEST_F(InterlinearCarryTest, DoesNotConsumeAWordThatCannotFitTheStrip) {
  ParsedText translation(/*extraParagraphSpacing=*/false);
  translation.addWord("twentycharacterslong", EpdFontFamily::REGULAR, /*underline=*/false,
                      /*attachToPrevious=*/false);
  ParsedText stream = makeStream();

  ChapterHtmlSlimParser::InterlinearBands bands = oneStripBand();
  bands.floorX = 400;  // ink already on this strip: only 76px of room is left after the gap
  run(InterlinearAnnotation{0, 0, 1}, translation, bands, stream);

  EXPECT_EQ(stream.size(), 1u) << "the word must still be queued for a strip that can hold it";
}

// A sentence layout never placed (its tracked word reports NOT_PLACED, which renderInterlinear
// models here as a band with no slots) must still hand its translation to the stream. Skipping such
// a sentence outright is how its text used to disappear.
TEST_F(InterlinearCarryTest, QueuesTheSpanEvenWithNoSlotsToPlaceItIn) {
  const ParsedText translation = makeTranslation();
  ParsedText stream = makeStream();

  ChapterHtmlSlimParser::InterlinearBands bands = oneStripBand();
  bands.slots = 0;
  run(InterlinearAnnotation{0, 0, WORD_COUNT}, translation, bands, stream);

  EXPECT_EQ(stream.size(), static_cast<size_t>(WORD_COUNT));
}

// The carry is what the NEXT sentence spends its slots on: called repeatedly with one strip each
// time, the stream drains to empty and no word is ever lost.
TEST_F(InterlinearCarryTest, DrainsTheCarryOverFollowingStrips) {
  const ParsedText translation = makeTranslation();
  ParsedText stream = makeStream();

  // The first sentence owns the words; every later strip belongs to a sentence with none of its own,
  // which must still run so the inherited carry gets placed.
  run(InterlinearAnnotation{0, 0, WORD_COUNT}, translation, oneStripBand(), stream);
  size_t guard = 0;
  while (!stream.isEmpty() && guard++ < 16) {
    const size_t before = stream.size();
    run(InterlinearAnnotation{0, 0, 0}, translation, oneStripBand(), stream);
    ASSERT_LT(stream.size(), before) << "a strip that places nothing would loop forever";
  }

  EXPECT_TRUE(stream.isEmpty());
}

}  // namespace
