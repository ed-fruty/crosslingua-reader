// CrossLingua fork tests: the per-word "translated" flag. Upstream fills all eight bits of the word
// style byte (ruby follower = 64, discretionary hyphen = 128), so the flag lives in ParsedText's
// linguaTranslated array, the TextBlock's LinguaWordFlags and PageLine's Lingua fields. These tests
// pin that it survives every step from the parser to a re-loaded page, and that it does not disturb
// the two upstream bits it used to collide with.
#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class
#include "modules/lingua/modes/common/PageTextJoiner.h"
#include "modules/lingua/utils/SentencePairing.h"

// Defined in ParserLinkStubs.cpp: when set, the Hyphenator stub delegates to it.
extern std::vector<Hyphenator::BreakInfo> (*gTestBreakOffsets)(const std::string& word, bool includeFallback,
                                                               bool translated);

namespace {

static_assert(EpdFontFamily::RUBY_CONTINUE == 64, "style byte must keep upstream's ruby bit");
static_assert(TextBlock::DISCRETIONARY_HYPHEN_FLAG == 0x80, "style byte must keep upstream's hyphen bit");

struct DrawnLine {
  std::vector<std::string> words;
  std::vector<bool> translated;
};

DrawnLine readLine(const TextBlock& block) {
  DrawnLine line;
  for (uint16_t i = 0; i < block.wordCount(); i++) {
    line.words.emplace_back(block.wordText(i));
    line.translated.push_back(block.linguaWordTranslated(i));
  }
  return line;
}

std::string tmpPath(const char* tag) {
  return std::string("/tmp/crosspoint_lingua_translated_") + tag + "_" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

// Serialize a page and read it back, exactly as Section does for section.bin.
std::unique_ptr<Page> roundTrip(const Page& page) {
  const std::string path = tmpPath("page");
  {
    HalFile out;
    EXPECT_TRUE(out.open(path.c_str(), "wb"));
    EXPECT_TRUE(page.serialize(out));
  }
  HalFile in;
  EXPECT_TRUE(in.open(path.c_str(), "rb"));
  auto loaded = Page::deserialize(in);
  in.close();
  std::remove(path.c_str());
  return loaded;
}

std::vector<const PageLine*> linesOf(const Page& page) {
  std::vector<const PageLine*> lines;
  for (const auto& element : page.elements) {
    if (element->getTag() == TAG_PageLine) lines.push_back(static_cast<const PageLine*>(element.get()));
  }
  return lines;
}

class TranslatedWordsParserTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;

  void TearDown() override { std::remove(tmpPath("html").c_str()); }

  void parse(const std::string& html, const LinguaLayout layout = LinguaLayout::Both) {
    const std::string path = tmpPath("html");
    {
      std::ofstream(path) << html;
    }
    ChapterHtmlSlimParser parser{
        nullptr,
        path,
        renderer,
        0,
        1.0f,
        false,
        0,
        static_cast<uint16_t>(renderer.getScreenWidth()),
        static_cast<uint16_t>(renderer.getScreenHeight()),
        false,
        false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        true,
        "",
        "",
        0,
        {},
        nullptr,
        &cssParser,
        layout,
        "en"};
    ASSERT_TRUE(parser.beginParse());
    for (int guard = 0; guard < 10000; guard++) {
      const auto status = parser.parseStep();
      ASSERT_NE(status, ChapterHtmlSlimParser::ParseStatus::Error);
      if (status != ChapterHtmlSlimParser::ParseStatus::More) break;
    }
    parser.finishParse();
  }

  std::vector<const PageLine*> allLines() const {
    std::vector<const PageLine*> lines;
    for (const auto& page : pages) {
      for (const PageLine* line : linesOf(*page)) lines.push_back(line);
    }
    return lines;
  }

  // Draw a page and report, per drawn string, whether it was drawn as a translated word.
  std::vector<std::pair<std::string, bool>> draw(const Page& page) {
    std::vector<std::pair<std::string, bool>> log;
    renderer.drawLog = &log;
    page.render(renderer, PageFontSet(0), 0, 0);
    renderer.drawLog = nullptr;
    return log;
  }
};

// An inline <span lang> inside an original paragraph translates only ITS words: the line is mixed,
// and drawing it grays exactly those words, before and after a section.bin round trip.
TEST_F(TranslatedWordsParserTest, InlineSpanGraysOnlyItsWordsThroughSerialization) {
  parse("<html><body><p>Hello <span lang=\"uk\">Привіт світ</span> world</p></body></html>");
  ASSERT_EQ(pages.size(), 1u);
  const auto lines = linesOf(*pages[0]);
  ASSERT_EQ(lines.size(), 1u);
  const DrawnLine line = readLine(*lines[0]->getBlock());
  ASSERT_EQ(line.words, (std::vector<std::string>{"Hello", "Привіт", "світ", "world"}));
  EXPECT_EQ(line.translated, (std::vector<bool>{false, true, true, false}));
  EXPECT_TRUE(lines[0]->getBlock()->linguaTranslatedWords().any());
  EXPECT_FALSE(lines[0]->getBlock()->linguaTranslatedWords().all());

  const std::vector<std::pair<std::string, bool>> expected{
      {"Hello", false}, {"Привіт", true}, {"світ", true}, {"world", false}};
  EXPECT_EQ(draw(*pages[0]), expected);

  const auto loaded = roundTrip(*pages[0]);
  ASSERT_TRUE(loaded);
  const auto loadedLines = linesOf(*loaded);
  ASSERT_EQ(loadedLines.size(), 1u);
  const DrawnLine reloaded = readLine(*loadedLines[0]->getBlock());
  EXPECT_EQ(reloaded.words, line.words);
  EXPECT_EQ(reloaded.translated, line.translated);
  EXPECT_EQ(draw(*loaded), expected);
}

// A translated paragraph is flagged "all" (no mask), an original one "none".
TEST_F(TranslatedWordsParserTest, WholeParagraphsUseTheCompactEncodings) {
  parse("<html><body><p>Hello world</p><p lang=\"uk\">Привіт світ</p></body></html>");
  const auto lines = allLines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_FALSE(lines[0]->getBlock()->linguaTranslatedWords().any());
  EXPECT_TRUE(lines[1]->getBlock()->linguaTranslatedWords().all());

  ASSERT_EQ(pages.size(), 1u);
  const auto loaded = roundTrip(*pages[0]);
  ASSERT_TRUE(loaded);
  const auto loadedLines = linesOf(*loaded);
  ASSERT_EQ(loadedLines.size(), 2u);
  EXPECT_FALSE(loadedLines[0]->getBlock()->linguaTranslatedWords().any());
  EXPECT_TRUE(loadedLines[1]->getBlock()->linguaTranslatedWords().all());
}

// A styled block pushes an upstream style entry on top of the Lingua marker; the translated state
// must survive it, in the block itself and in a styled child of a translated container.
TEST_F(TranslatedWordsParserTest, StyledBlocksKeepTheTranslatedState) {
  parse(
      "<html><body><p lang=\"uk\" style=\"font-style:italic\">Привіт</p>"
      "<div lang=\"uk\"><p style=\"text-align:center\">світ</p></div>"
      "<p style=\"text-align:center\">Hello</p></body></html>");
  const auto lines = allLines();
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_TRUE(lines[0]->getBlock()->linguaTranslatedWords().all());
  EXPECT_TRUE(lines[1]->getBlock()->linguaTranslatedWords().all());
  EXPECT_FALSE(lines[2]->getBlock()->linguaTranslatedWords().any());
}

// Ruby inside a translated block: the follower keeps upstream's RUBY_CONTINUE bit (which used to be
// the fork's TRANSLATED bit) while every word is still flagged translated, so the ruby group draws
// its annotation once, over the leader.
TEST_F(TranslatedWordsParserTest, RubyGroupingSurvivesInsideATranslatedBlock) {
  parse("<html><body><p lang=\"ja\"><ruby>漢字<rt>かんじ</rt></ruby>です</p></body></html>");
  const auto lines = allLines();
  ASSERT_EQ(lines.size(), 1u);
  const TextBlock& block = *lines[0]->getBlock();
  ASSERT_GE(block.wordCount(), 2);
  EXPECT_STREQ(block.wordText(0), "漢");
  EXPECT_STREQ(block.wordText(1), "字");
  EXPECT_EQ(block.wordStyle(0) & EpdFontFamily::RUBY_CONTINUE, 0);
  EXPECT_NE(block.wordStyle(1) & EpdFontFamily::RUBY_CONTINUE, 0);
  ASSERT_TRUE(block.hasRuby());
  EXPECT_EQ(block.getRubyTexts()[0], "かんじ");
  EXPECT_TRUE(block.linguaTranslatedWords().all());

  int rubyDraws = 0;
  for (const auto& [text, translated] : draw(*pages[0])) {
    if (text == "かんじ") rubyDraws++;
  }
  EXPECT_EQ(rubyDraws, 1);

  const auto loaded = roundTrip(*pages[0]);
  ASSERT_TRUE(loaded);
  const TextBlock& reloaded = *linesOf(*loaded)[0]->getBlock();
  EXPECT_EQ(reloaded.wordStyle(0) & EpdFontFamily::RUBY_CONTINUE, 0);
  EXPECT_NE(reloaded.wordStyle(1) & EpdFontFamily::RUBY_CONTINUE, 0);
  EXPECT_TRUE(reloaded.linguaTranslatedWords().all());
}

// Side by Side: the translation column is flagged translated, the source column is not.
TEST_F(TranslatedWordsParserTest, SideBySideColumnsKeepTheirFlags) {
  parse("<html><body><p>Hello world</p><p lang=\"uk\">Привіт світ</p></body></html>", LinguaLayout::SideBySide);
  bool sawSource = false;
  bool sawTranslation = false;
  for (const PageLine* line : allLines()) {
    if (line->fontRole == LineFontRole::Translation) {
      sawTranslation = true;
      EXPECT_TRUE(line->getBlock()->linguaTranslatedWords().all());
    } else if (line->fontRole == LineFontRole::Body) {
      sawSource = true;
      EXPECT_FALSE(line->getBlock()->linguaTranslatedWords().any());
    }
  }
  EXPECT_TRUE(sawSource);
  EXPECT_TRUE(sawTranslation);
}

// ── ParsedText-level ────────────────────────────────────────────────────────

// Break every word of at least 8 bytes after its 4th byte with an inserted hyphen; record whether
// the hyphenator was asked for translated-language rules.
std::vector<std::pair<std::string, bool>> gBreakCalls;
std::vector<Hyphenator::BreakInfo> breakAfterFour(const std::string& word, bool, const bool translated) {
  gBreakCalls.emplace_back(word, translated);
  if (word.size() < 8) return {};
  return {Hyphenator::BreakInfo{4, true}};
}

class TranslatedWordsLayoutTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;
  void SetUp() override {
    gBreakCalls.clear();
    gTestBreakOffsets = &breakAfterFour;
  }
  void TearDown() override { gTestBreakOffsets = nullptr; }

  static ParsedText makeText(const bool hyphenation, const bool rtl = false) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    style.isRtl = rtl;
    style.directionDefined = true;
    return ParsedText(hyphenation, /*focusReadingEnabled=*/false, style, /*paragraphIndentSpaces=*/0);
  }
  static void add(ParsedText& text, const char* word, const bool translated) {
    text.setLinguaTranslatedInput(translated);
    text.addWord(word, EpdFontFamily::REGULAR);
    text.setLinguaTranslatedInput(false);
  }
};

// A translated word split by hyphenation: both halves stay translated, the prefix carries upstream's
// discretionary-hyphen bit (masked out of wordStyle), the hyphenator was asked for the translated
// language for that word only, and all of it survives PageLine serialization.
TEST_F(TranslatedWordsLayoutTest, HyphenSplitKeepsTheFlagAndTheDiscretionaryHyphen) {
  ParsedText text = makeText(/*hyphenation=*/true);
  add(text, "one", false);
  add(text, "abcdefgh", true);
  add(text, "twothree", false);

  // 72px: "one abcd-" (24 + 4 + 40) fits, "one abcdefgh" (92) does not.
  std::vector<std::unique_ptr<TextBlock>> blocks;
  text.layoutAndExtractLines(renderer, 0, 72,
                             [&](std::unique_ptr<TextBlock> block) { blocks.push_back(std::move(block)); });
  ASSERT_GE(blocks.size(), 2u);

  std::vector<std::string> words;
  std::vector<bool> translated;
  std::vector<bool> hyphen;
  for (const auto& block : blocks) {
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      words.emplace_back(block->wordText(i));
      translated.push_back(block->linguaWordTranslated(i));
      hyphen.push_back(block->wordHasDiscretionaryHyphen(i));
      EXPECT_EQ(block->wordStyle(i) & TextBlock::DISCRETIONARY_HYPHEN_FLAG, 0);
    }
  }
  ASSERT_EQ(words, (std::vector<std::string>{"one", "abcd-", "efgh", "twothree"}));
  EXPECT_EQ(translated, (std::vector<bool>{false, true, true, false}));
  EXPECT_EQ(hyphen, (std::vector<bool>{false, true, false, false}));

  bool askedForTranslatedRules = false;
  for (const auto& [word, asTranslated] : gBreakCalls) {
    EXPECT_EQ(asTranslated, word == "abcdefgh") << word;
    askedForTranslatedRules = askedForTranslatedRules || asTranslated;
  }
  EXPECT_TRUE(askedForTranslatedRules);

  // The same split of an ORIGINAL word asks for the book's rules and flags neither half.
  {
    gBreakCalls.clear();
    ParsedText original = makeText(/*hyphenation=*/true);
    add(original, "abcdefgh", false);
    std::vector<std::unique_ptr<TextBlock>> originalBlocks;
    original.layoutAndExtractLines(
        renderer, 0, 48, [&](std::unique_ptr<TextBlock> block) { originalBlocks.push_back(std::move(block)); });
    ASSERT_EQ(originalBlocks.size(), 2u);
    EXPECT_STREQ(originalBlocks[0]->wordText(0), "abcd-");
    EXPECT_TRUE(originalBlocks[0]->wordHasDiscretionaryHyphen(0));
    for (const auto& block : originalBlocks) EXPECT_FALSE(block->linguaTranslatedWords().any());
    ASSERT_FALSE(gBreakCalls.empty());
    for (const auto& call : gBreakCalls) EXPECT_FALSE(call.second);
  }

  // Round-trip every line through PageLine, which owns the translated-word persistence.
  Page page;
  for (auto& block : blocks) page.elements.push_back(std::make_unique<PageLine>(std::move(block), 0, 0));
  const auto loaded = roundTrip(page);
  ASSERT_TRUE(loaded);
  std::vector<std::string> loadedWords;
  std::vector<bool> loadedTranslated;
  std::vector<bool> loadedHyphen;
  for (const PageLine* line : linesOf(*loaded)) {
    const TextBlock& block = *line->getBlock();
    for (uint16_t i = 0; i < block.wordCount(); i++) {
      loadedWords.emplace_back(block.wordText(i));
      loadedTranslated.push_back(block.linguaWordTranslated(i));
      loadedHyphen.push_back(block.wordHasDiscretionaryHyphen(i));
    }
  }
  EXPECT_EQ(loadedWords, words);
  EXPECT_EQ(loadedTranslated, translated);
  EXPECT_EQ(loadedHyphen, hyphen);
}

// The fork's per-line API (extractNextLine -> consumeWords) keeps the flags aligned with the words
// left in the stream.
TEST_F(TranslatedWordsLayoutTest, ExtractNextLineConsumesFlagsInStep) {
  ParsedText text = makeText(/*hyphenation=*/false);
  const std::vector<std::pair<const char*, bool>> input{{"aa", false}, {"bb", true}, {"cc", true},
                                                        {"dd", false}, {"ee", true}, {"ff", false}};
  for (const auto& [word, translated] : input) add(text, word, translated);

  std::vector<std::pair<std::string, bool>> seen;
  for (int guard = 0; guard < 16 && !text.isEmpty(); guard++) {
    // 40px holds two 16px words and their 4px gap.
    text.extractNextLine(renderer, 0, 40, [&](std::unique_ptr<TextBlock> block) {
      for (uint16_t i = 0; i < block->wordCount(); i++)
        seen.emplace_back(block->wordText(i), block->linguaWordTranslated(i));
    });
  }
  ASSERT_EQ(seen.size(), input.size());
  for (size_t i = 0; i < input.size(); i++) {
    EXPECT_EQ(seen[i].first, input[i].first);
    EXPECT_EQ(seen[i].second, input[i].second) << input[i].first;
  }
}

// A BiDi-reordered line stores the flags in VISUAL order, like every other TextBlock array.
TEST_F(TranslatedWordsLayoutTest, BidiReorderCarriesTheFlagWithItsWord) {
  ParsedText text = makeText(/*hyphenation=*/false, /*rtl=*/true);
  add(text, "שלום", false);
  add(text, "hello", true);
  add(text, "עולם", false);
  std::vector<std::unique_ptr<TextBlock>> blocks;
  text.layoutAndExtractLines(renderer, 0, 480,
                             [&](std::unique_ptr<TextBlock> block) { blocks.push_back(std::move(block)); });
  ASSERT_EQ(blocks.size(), 1u);
  const DrawnLine line = readLine(*blocks[0]);
  ASSERT_EQ(line.words.size(), 3u);
  for (size_t i = 0; i < line.words.size(); i++) {
    EXPECT_EQ(line.translated[i], line.words[i] == "hello") << line.words[i];
  }
}

// ── Page text joining (Lingua overlays match page text against the chapter HTML) ──────────────

std::string joinPage(const Page& page) {
  std::string text;
  lingua::PageTextJoiner joiner;
  for (const PageLine* line : linesOf(page)) {
    for (uint16_t i = 0; i < line->getBlock()->wordCount(); i++) joiner.append(text, *line->getBlock(), i);
  }
  return text;
}

// Pieces of one source word -- here an inline tag splitting "Bonjour"; on device also upstream's
// "self-" + "evident" hyphen split, which this harness's stubbed hyphen predicate does not make --
// touch in the source, so the join must not put a space between them (the walker cuts a
// "self- evident" needle at the hyphen).
TEST_F(TranslatedWordsParserTest, JoinerGluesPiecesThatTouchInTheSource) {
  parse("<html><body><p>A truth, Bo<i>n</i>jour friend</p></body></html>");
  ASSERT_EQ(pages.size(), 1u);
  const auto lines = linesOf(*pages[0]);
  ASSERT_EQ(lines.size(), 1u);
  const DrawnLine line = readLine(*lines[0]->getBlock());
  ASSERT_EQ(line.words, (std::vector<std::string>{"A", "truth,", "Bo", "n", "jour", "friend"}));
  EXPECT_EQ(joinPage(*pages[0]), "A truth, Bonjour friend");
}

// A line-end hyphenation split joins back into the source word, without the inserted hyphen.
TEST_F(TranslatedWordsLayoutTest, JoinerDropsTheDiscretionaryHyphenAcrossLines) {
  ParsedText text = makeText(/*hyphenation=*/true);
  text.addWord("one", EpdFontFamily::REGULAR, false, false, /*visibleTextOffset=*/0);
  text.addWord("abcdefgh", EpdFontFamily::REGULAR, false, false, /*visibleTextOffset=*/4);
  Page page;
  text.layoutAndExtractLines(renderer, 0, 72, [&](std::unique_ptr<TextBlock> block) {
    page.elements.push_back(std::make_unique<PageLine>(std::move(block), 0, 0));
  });
  ASSERT_EQ(linesOf(page).size(), 2u);
  EXPECT_STREQ(linesOf(page)[0]->getBlock()->wordText(1), "abcd-");
  EXPECT_EQ(joinPage(page), "one abcdefgh");
}

// ── Tooltip sentence keys: page words vs the source's whitespace tokens ──────────────────────────

std::string sourceKey(const std::vector<const char*>& sourceTokens) {
  return sentenceKey(sourceTokens.data(), 0, static_cast<int>(sourceTokens.size()));
}
std::string pageKey(const std::vector<const char*>& pageWords, const std::vector<WordGlue>& glue) {
  return sentenceKeyGlued(pageWords.data(), glue.data(), 0, static_cast<int>(pageWords.size()));
}
constexpr WordGlue S = WordGlue::Separate;
constexpr WordGlue G = WordGlue::Glued;
constexpr WordGlue D = WordGlue::GluedDropHyphen;

TEST(TranslatedWordsSentenceKeyTest, HyphenatedCompoundMatchesTheSourceToken) {
  const std::vector<const char*> page{"It", "was", "a", "well-", "known", "fact."};
  const std::string source = sourceKey({"It", "was", "a", "well-known", "fact."});
  EXPECT_EQ(pageKey(page, {S, S, S, S, G, S}), source);
  // The bug this guards: keyed word by word, the page drops the real hyphen ("wellknown").
  EXPECT_NE(sentenceKey(page.data(), 0, static_cast<int>(page.size())), source);
}

TEST(TranslatedWordsSentenceKeyTest, DashCompoundMatchesTheSourceToken) {
  EXPECT_EQ(pageKey({"He", "said—", "and", "left."}, {S, S, G, S}), sourceKey({"He", "said—and", "left."}));
}

TEST(TranslatedWordsSentenceKeyTest, CompoundAtSentenceStart) {
  EXPECT_EQ(pageKey({"Self-", "evident", "truths", "hold."}, {S, G, S, S}),
            sourceKey({"Self-evident", "truths", "hold."}));
}

TEST(TranslatedWordsSentenceKeyTest, InlineTagSplitAndNbspToken) {
  // "Bo<i>n</i>jour" glued; an NBSP token separates like the source tokenizer's whitespace does.
  EXPECT_EQ(pageKey({"Bo", "n", "jour", "\xc2\xa0", "ami."}, {S, G, G, G, G}), sourceKey({"Bonjour", "ami."}));
}

TEST(TranslatedWordsSentenceKeyTest, DiscretionaryLineEndSplitDropsOnlyTheInsertedHyphen) {
  EXPECT_EQ(pageKey({"Quadrat-", "kilometer", "big."}, {S, D, S}), sourceKey({"Quadratkilometer", "big."}));
}

// End to end: glue read from real laid-out TextBlocks across a discretionary line-end split.
TEST_F(TranslatedWordsLayoutTest, SentenceKeyFromLaidOutLinesMatchesTheSource) {
  ParsedText text = makeText(/*hyphenation=*/true);
  text.addWord("one", EpdFontFamily::REGULAR, false, false, /*visibleTextOffset=*/0);
  text.addWord("abcdefgh", EpdFontFamily::REGULAR, false, false, /*visibleTextOffset=*/4);
  std::vector<std::unique_ptr<TextBlock>> blocks;
  text.layoutAndExtractLines(renderer, 0, 72,
                             [&](std::unique_ptr<TextBlock> block) { blocks.push_back(std::move(block)); });
  ASSERT_EQ(blocks.size(), 2u);
  std::vector<const char*> words;
  std::vector<WordGlue> glue;
  lingua::PageTextJoiner joiner;
  for (const auto& block : blocks) {
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      words.push_back(block->wordText(i));
      glue.push_back(joiner.next(*block, i));
    }
  }
  ASSERT_EQ(words.size(), 3u);
  EXPECT_EQ(pageKey(words, glue), sourceKey({"one", "abcdefgh"}));
}

// Interlinear re-emits a translation into annotation rows WITHOUT the flag: the rows are drawn in
// their own line role's ink, never the Interleaved gray.
TEST(TranslatedWordsInterlinearTest, AnnotationRowsDropTheFlag) {
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  const std::string filepath = "unused.xhtml";
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
  ParsedText translation;
  for (const char* word : {"Привіт", "світ"}) {
    translation.setLinguaTranslatedInput(true);
    translation.addWord(word, EpdFontFamily::REGULAR);
    translation.setLinguaTranslatedInput(false);
  }
  ASSERT_TRUE(translation.linguaWordTranslatedAt(0));

  BlockStyle annStyle;
  annStyle.alignment = CssTextAlign::Left;
  annStyle.textIndentDefined = true;
  ParsedText stream(false, false, annStyle, 0);
  ChapterHtmlSlimParser::InterlinearBands bands;
  bands.srcLines = nullptr;
  bands.firstLine = 0;
  bands.slots = 1;
  bands.headX = 0;
  bands.tailX = static_cast<int16_t>(renderer.getScreenWidth());
  bands.floorX = 0;
  std::vector<ChapterHtmlSlimParser::InterlinearRun> runs;
  parser.buildAnnotationRuns(InterlinearAnnotation{0, 0, 2}, translation, bands,
                             static_cast<uint16_t>(renderer.getScreenWidth()), 1, runs, stream);
  size_t rowWords = 0;
  for (const auto& run : runs) {
    if (!run.row) continue;
    rowWords += run.row->wordCount();
    EXPECT_FALSE(run.row->linguaTranslatedWords().any());
  }
  EXPECT_EQ(rowWords, 2u);
}

}  // namespace
