// A word still buffered when a block element opens ("text<ol>", "text<div>") belongs to the block
// before it. It must not be carried into the new block and glued to that block's first word.
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"

namespace {

class BlockStartWordFlushTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;

  std::string htmlPath() const {
    return std::string("/tmp/crosspoint_blockflush_") +
           ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".xhtml";
  }
  void TearDown() override { std::remove(htmlPath().c_str()); }

  // Lays `body` out (left-aligned, no CSS, as the dictionary view does) and returns each line's text.
  // Words are joined with a space only where layout left a gap, so glued fragments read as one word.
  std::vector<std::string> lines(const std::string& body) {
    const std::string path = htmlPath();
    {
      std::ofstream(path) << "<html><body>" << body << "</body></html>";
    }
    std::vector<std::string> out;
    ChapterHtmlSlimParser parser{nullptr,
                                 path,
                                 renderer,
                                 0,
                                 1.0f,
                                 false,
                                 /*paragraphAlignment=LEFT_ALIGN*/ 1,
                                 static_cast<uint16_t>(renderer.getScreenWidth()),
                                 static_cast<uint16_t>(renderer.getScreenHeight()),
                                 false,
                                 false,
                                 [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
                                   for (const auto& element : page->elements) {
                                     if (element->getTag() != TAG_PageLine) continue;
                                     const auto& block = *static_cast<const PageLine&>(*element).getBlock();
                                     std::string text;
                                     for (uint16_t i = 0; i < block.wordCount(); ++i) {
                                       if (i > 0) {
                                         const int prevEnd =
                                             block.wordXpos(i - 1) +
                                             renderer.getTextAdvanceX(0, block.wordText(i - 1), block.wordStyle(i - 1));
                                         if (block.wordXpos(i) > prevEnd) text += ' ';
                                       }
                                       text += block.wordText(i);
                                     }
                                     out.push_back(text);
                                   }
                                 },
                                 /*embeddedStyle=*/false,
                                 "",
                                 "",
                                 /*imageRendering=*/2};
    EXPECT_TRUE(parser.parseAndBuildPages());
    return out;
  }
};

using Lines = std::vector<std::string>;

// The WikDict StarDict entry that showed the bug: "game" had no trailing whitespace before <ol>.
TEST_F(BlockStartWordFlushTest, WikDictEntryKeepsTheWordBeforeTheList) {
  EXPECT_EQ(lines("<div>/<font color=\"gray\">\xCB\x88\x62\x65\xC9\xAAs.b\xC9\x91l</font>/<br/>\n"
                  "<div><font class=\"grammar\" color=\"green\">noun</font></div>ball game<ol>"
                  "<li><div>\xD0\xB1\xD0\xB5\xD0\xB9\xD1\x81\xD0\xB1\xD0\xBE\xCC\x81\xD0\xBB</div></li>"
                  "<li><div>\xD0\xB1\xD0\xB5\xD0\xB9\xD1\x81\xD0\xB1\xD0\xBE\xD0\xBB</div></li></ol></div>"),
            (Lines{"/\xCB\x88\x62\x65\xC9\xAAs.b\xC9\x91l/", "noun", "ball game",
                   "1. \xD0\xB1\xD0\xB5\xD0\xB9\xD1\x81\xD0\xB1\xD0\xBE\xCC\x81\xD0\xBB",
                   "2. \xD0\xB1\xD0\xB5\xD0\xB9\xD1\x81\xD0\xB1\xD0\xBE\xD0\xBB"}));
}

TEST_F(BlockStartWordFlushTest, WordBeforeOrderedList) {
  EXPECT_EQ(lines("<div>text<ol><li>x</li></ol></div>"), (Lines{"text", "1. x"}));
}

TEST_F(BlockStartWordFlushTest, WordBeforeUnorderedList) {
  EXPECT_EQ(lines("<div>text<ul><li>x</li></ul></div>"), (Lines{"text", "\xE2\x80\xA2 x"}));
}

TEST_F(BlockStartWordFlushTest, WordBeforeListItem) {
  EXPECT_EQ(lines("<ul><li>a<ul><li>b</li></ul></li></ul>"), (Lines{"\xE2\x80\xA2 a", "\xE2\x80\xA2 b"}));
}

TEST_F(BlockStartWordFlushTest, WordBeforeDiv) {
  EXPECT_EQ(lines("<div>text<div>x</div></div>"), (Lines{"text", "x"}));
}

TEST_F(BlockStartWordFlushTest, WordBeforeParagraph) {
  EXPECT_EQ(lines("<div>text<p>x</p></div>"), (Lines{"text", "x"}));
}

TEST_F(BlockStartWordFlushTest, WordBeforeHeading) {
  EXPECT_EQ(lines("<div>text<h2>x</h2></div>"), (Lines{"text", "x"}));
}

// Control: an inline element continues the same word.
TEST_F(BlockStartWordFlushTest, InlineElementContinuesTheWord) {
  EXPECT_EQ(lines("<p>foo<b>bar</b></p>"), (Lines{"foobar"}));
}

}  // namespace
