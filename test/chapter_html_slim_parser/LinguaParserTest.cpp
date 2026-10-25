// CrossLingua fork tests for ChapterHtmlSlimParser: paragraph indexing and the Interlinear
// annotation carry. Kept out of ChapterHtmlSlimParserTest.cpp so upstream test changes merge cleanly.
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "src/ReaderFontSizes.h"

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class
#include "../../src/fontIds.h"

namespace {

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

// <ul>/<ol> open a layout block of their own, but it stays empty: the first <li> reuses it and
// inherits the index it claimed, so a list costs one index per item and nothing for the container.
TEST_F(ParagraphIndexTest, ListContainersConsumeNoIndexOfTheirOwn) {
  EXPECT_EQ(paragraphCount("<html><body>"
                           "<p>Before.</p>"
                           "<ul><li>One.</li><li>Two.</li></ul>"
                           "<ol><li><p>Three.</p></li></ol>"
                           "<p>After.</p>"
                           "</body></html>"),
            5);
}

// LinguaLayout::Interlinear: an annotation longer than the strips its own sentence owns must not
// lose its tail -- it is CARRIED in the paragraph stream for the next sentence to place. The stub
// renderer measures 8px per character for EVERY font id, so here the annotation face is exactly as
// wide as the body face: the worst case on real hardware, and the one a larger Annotation Size
// setting approaches.
//
// The assertions read the STREAM rather than the emitted rows on purpose: what the stream shows is
// exactly the contract -- how much text a call consumed, and how much it handed on.
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

  // ParsedText is move-less (it owns a WordStore arena), so the fixture fills one in place.
  static void fillTranslation(ParsedText& block) {
    for (uint16_t i = 0; i < WORD_COUNT; i++) {
      block.addWord("abcdef", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false);
    }
  }

  ParsedText makeStream() const {
    BlockStyle annStyle;
    annStyle.alignment = CssTextAlign::Left;
    annStyle.textIndent = 0;
    annStyle.textIndentDefined = true;
    return ParsedText(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, annStyle,
                      /*paragraphIndentSpaces=*/0);
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
  ParsedText translation;
  fillTranslation(translation);
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
  ParsedText translation;
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
  ParsedText translation;
  fillTranslation(translation);
  ParsedText stream = makeStream();

  ChapterHtmlSlimParser::InterlinearBands bands = oneStripBand();
  bands.slots = 0;
  run(InterlinearAnnotation{0, 0, WORD_COUNT}, translation, bands, stream);

  EXPECT_EQ(stream.size(), static_cast<size_t>(WORD_COUNT));
}

// The carry is what the NEXT sentence spends its slots on: called repeatedly with one strip each
// time, the stream drains to empty and no word is ever lost.
TEST_F(InterlinearCarryTest, DrainsTheCarryOverFollowingStrips) {
  ParsedText translation;
  fillTranslation(translation);
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

// ── Lingua hooks in upstream's layout pass ──────────────────────────────────
//
// Elements are fed straight to the expat callbacks, as upstream's ChapterBlockDecorationTest does.
class LinguaLayoutHooksTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  std::string filepath = "unused.xhtml";
  std::vector<std::unique_ptr<Page>> pages;
  std::unique_ptr<ChapterHtmlSlimParser> parser;

  void start(const LinguaLayout layout, const int fontId = 0) {
    pages.clear();
    parser = std::make_unique<ChapterHtmlSlimParser>(
        nullptr, filepath, renderer, fontId, 1.0f, false, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
        static_cast<uint16_t>(renderer.getScreenHeight()), false, false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); }, true,
        "", "", 0, std::vector<std::string>{}, nullptr, &cssParser, layout, "en");
    // No file behind it: beginParse fails at the open, after setting up the layout state the
    // callbacks below need (as upstream's ChapterBlockDecorationTest relies on).
    parser->beginParse();
  }
  void open(const char* tag, const char* style = nullptr, const char* lang = nullptr) {
    const XML_Char* attributes[5] = {};
    int n = 0;
    if (style) {
      attributes[n++] = "style";
      attributes[n++] = style;
    }
    if (lang) {
      attributes[n++] = "lang";
      attributes[n++] = lang;
    }
    ChapterHtmlSlimParser::startElement(parser.get(), tag, n ? attributes : nullptr);
  }
  void text(const std::string& value) {
    ChapterHtmlSlimParser::characterData(parser.get(), value.c_str(), static_cast<int>(value.size()));
  }
  void close(const char* tag) { ChapterHtmlSlimParser::endElement(parser.get(), tag); }
  void finish() { ASSERT_TRUE(parser->finishParse()); }
  std::vector<const PageElement*> elementsWithTag(const PageElementTag tag) const {
    std::vector<const PageElement*> found;
    for (const auto& page : pages) {
      for (const auto& element : page->elements) {
        if (element->getTag() == tag) found.push_back(element.get());
      }
    }
    return found;
  }
  const PageLine* lineStarting(const char* word) const {
    for (const PageElement* element : elementsWithTag(TAG_PageLine)) {
      const auto* line = static_cast<const PageLine*>(element);
      if (line->getBlock()->wordCount() > 0 && std::string(line->getBlock()->wordText(0)) == word) return line;
    }
    return nullptr;
  }
};

// The default EdsLab family has built-in sizes, so headings and CSS font sizes scale under it too.
TEST_F(LinguaLayoutHooksTest, EdsLabBlockFontScaleUsesItsBuiltinSizes) {
  renderer.setFontVariantProvider({&renderer, [](void* ctx, const int fontId, const float scale) {
                                     const int variant = builtinReaderFontVariant(fontId, scale);
                                     return static_cast<GfxRenderer*>(ctx)->getFontMap().count(variant) ? variant : 0;
                                   }});
  for (const int id : {EDSLAB_12_FONT_ID, EDSLAB_14_FONT_ID, EDSLAB_16_FONT_ID, EDSLAB_18_FONT_ID}) {
    renderer.fontMap.emplace(id, EpdFontFamily(nullptr));
  }
  start(LinguaLayout::Both, EDSLAB_14_FONT_ID);
  EXPECT_EQ(parser->fontIdForScale(1.0f), EDSLAB_14_FONT_ID);
  EXPECT_EQ(parser->fontIdForScale(2.0f), EDSLAB_18_FONT_ID);
  EXPECT_EQ(parser->fontIdForScale(1.17f), EDSLAB_16_FONT_ID);
  EXPECT_EQ(parser->fontIdForScale(0.83f), EDSLAB_12_FONT_ID);
}

// A bordered block whose words the layout drops draws no empty frame; the same block shown draws one.
TEST_F(LinguaLayoutHooksTest, FilteredBorderedBlockDrawsNoFrame) {
  for (const LinguaLayout layout : {LinguaLayout::OriginalOnly, LinguaLayout::Both}) {
    start(layout);
    open("div", "border: 2px solid black", "uk");
    open("p");
    text("Привіт");
    close("p");
    close("div");
    open("p");
    text("Hello");
    close("p");
    finish();
    EXPECT_EQ(elementsWithTag(TAG_PageBorderBox).size(), layout == LinguaLayout::Both ? 1u : 0u);
  }
}

// A <br> in a block whose words were all dropped is not a blank line: Calibre's
// "Orig.<br/><span lang>Trans.</span>" under TranslationOnly, verse under OriginalOnly.
TEST_F(LinguaLayoutHooksTest, BreakInAFilteredBlockAddsNoBlankLine) {
  const auto translationY = [this](const bool withOriginal) {
    start(LinguaLayout::TranslationOnly);
    open("p", nullptr, "uk");
    text("Перший");
    close("p");
    open("p");
    if (withOriginal) {
      text("Orig.");
      open("br");
      close("br");
    }
    open("span", nullptr, "uk");
    text("Переклад");
    close("span");
    close("p");
    finish();
    const PageLine* line = lineStarting("Переклад");
    return line ? static_cast<int>(line->yPos) : -1;
  };
  EXPECT_GT(translationY(false), 0);
  EXPECT_EQ(translationY(true), translationY(false));

  const auto nextY = [this](const bool withVerse) {
    start(LinguaLayout::OriginalOnly);
    open("p");
    text("First");
    close("p");
    if (withVerse) {
      open("p", nullptr, "uk");
      text("a");
      open("br");
      close("br");
      text("b");
      close("p");
    }
    open("p");
    text("Next");
    close("p");
    finish();
    const PageLine* line = lineStarting("Next");
    return line ? static_cast<int>(line->yPos) : -1;
  };
  EXPECT_GT(nextY(false), 0);
  EXPECT_EQ(nextY(true), nextY(false));
}

// An original the pairing layout buffered is laid out at its OWN inline size when it turns out to
// be unpaired, not at the size of the block that was open when it was flushed.
TEST_F(LinguaLayoutHooksTest, BufferedOriginalKeepsItsInlineSize) {
  renderer.setFontVariantProvider({&renderer, [](void* ctx, const int fontId, const float scale) {
                                     const int variant = fontId * 1000 + static_cast<int>(scale * 100 + 0.5f);
                                     static_cast<GfxRenderer*>(ctx)->fontMap.emplace(variant, EpdFontFamily(nullptr));
                                     return variant;
                                   }});
  start(LinguaLayout::Interlinear, 7);
  open("p");
  open("span", "font-size: 2em");
  text("Big");
  close("span");
  close("p");
  open("p");
  text("Plain");
  close("p");
  finish();
  const PageLine* big = lineStarting("Big");
  ASSERT_NE(big, nullptr);
  EXPECT_EQ(big->getBlock()->getBlockStyle().fontId, 7200);
  const PageLine* plain = lineStarting("Plain");
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(plain->getBlock()->getBlockStyle().fontId, 0);
}

// A dropped word has no initial letter: a translated drop-cap span under OriginalOnly is not drawn.
TEST_F(LinguaLayoutHooksTest, FilteredSpanIsNotCapturedAsADropCap) {
  start(LinguaLayout::OriginalOnly);
  open("p");
  open("span", "float: left; font-size: 3em", "uk");
  text("Т");
  close("span");
  text("Hello world");
  close("p");
  finish();
  EXPECT_TRUE(elementsWithTag(TAG_PageDropCap).empty());
  EXPECT_NE(lineStarting("Hello"), nullptr);
}

}  // namespace
