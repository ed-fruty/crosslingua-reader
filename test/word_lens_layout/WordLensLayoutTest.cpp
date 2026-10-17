// CrossLingua Word Lens: the strip layout (LinguaLayout::WordLens), gloss placement and collision
// handling, and the reader's hard-word navigation. The stub renderer measures 8 px per glyph and
// 16 px per line for every font.
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "modules/lingua/modes/word_lens/WordLensNavigation.h"
#include "modules/lingua/modes/word_lens/WordLensStrip.h"
#include "modules/lingua/wordlens/LensText.h"

using namespace lingua::wordlens;

namespace {

constexpr int GLYPH = 8;
constexpr int LINE = 16;

// Hard words by key, with the strip gloss each gets.
class FakeGlosses final : public WordLensGlossSource {
 public:
  std::map<std::string, std::pair<uint16_t, std::string>> words;
  std::vector<std::pair<std::string, bool>> asked;  // key, sentenceInitial
  bool keepEmpty = true;
  bool keepEmptyStrips() const override { return keepEmpty; }

  bool decide(const char* word, const size_t length, const bool sentenceInitial, uint16_t& rank, std::string& gloss) {
    char key[LOOKUP_KEY_MAX_BYTES + 1];
    TokenInfo info;
    const size_t n = normalizeLookupKey(word, length, key, sizeof(key), info);
    if (n == 0) return false;
    asked.emplace_back(std::string(key, n), sentenceInitial);
    const auto it = words.find(std::string(key, n));
    if (it == words.end()) return false;
    if (info.capitalized && !info.allCaps && !sentenceInitial) return false;
    rank = it->second.first;
    gloss = it->second.second;
    return true;
  }

  bool glossFor(const char* word, const size_t length, const bool sentenceInitial, WordLensGloss& out) override {
    std::string gloss;
    if (!decide(word, length, sentenceInitial, out.rank, gloss)) return false;
    std::snprintf(out.text, sizeof(out.text), "%s", gloss.c_str());
    return true;
  }

  // Navigation's entry lookup: the pack, with no hard-word rules.
  static bool entry(void* ctx, const char* word, const size_t length, uint16_t& rank, uint32_t& entryOffset) {
    char key[LOOKUP_KEY_MAX_BYTES + 1];
    TokenInfo info;
    const size_t n = normalizeLookupKey(word, length, key, sizeof(key), info);
    const auto& words = static_cast<FakeGlosses*>(ctx)->words;
    const auto it = words.find(std::string(key, n));
    if (n == 0 || it == words.end()) return false;
    rank = it->second.first;
    entryOffset = static_cast<uint32_t>(std::distance(words.begin(), it));
    return true;
  }
};

struct Line {
  int16_t x;
  int16_t y;
  LineFontRole role;
  std::vector<std::string> words;
  std::vector<int16_t> xs;
};

class WordLensLayoutTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  std::vector<std::unique_ptr<Page>> pages;

  std::string htmlPath() const {
    return std::string("/tmp/crosspoint_wordlens_") + ::testing::UnitTest::GetInstance()->current_test_info()->name() +
           ".xhtml";
  }
  void TearDown() override { std::remove(htmlPath().c_str()); }

  // Lay `body` out at `width` px and return every line of every page in order.
  std::vector<Line> layout(const std::string& body, const LinguaLayout layoutKind, WordLensGlossSource* source,
                           const uint16_t width = 240, const uint16_t height = 800) {
    {
      std::ofstream(htmlPath()) << "<html><body>" << body << "</body></html>";
    }
    pages.clear();
    const std::string path = htmlPath();
    ChapterHtmlSlimParser parser{
        nullptr,
        path,
        renderer,
        0,
        1.0f,
        false,
        0,
        width,
        height,
        false,
        false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pages.push_back(std::move(page)); },
        false,
        "",
        "",
        0,
        {},
        nullptr,
        &cssParser,
        layoutKind,
        "en",
        0,
        /*annotationFontId=*/1};
    parser.setParagraphIndentSpaces(0);
    parser.setWordLensSource(source);
    EXPECT_TRUE(parser.parseAndBuildPages());
    std::vector<Line> out;
    int pageIndex = 0;
    for (const auto& page : pages) {
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const auto& pl = static_cast<const PageLine&>(*el);
        Line line{pl.xPos, static_cast<int16_t>(pl.yPos + pageIndex * 10000), pl.fontRole, {}, {}};
        for (uint16_t w = 0; w < pl.getBlock()->wordCount(); w++) {
          line.words.emplace_back(pl.getBlock()->wordText(w));
          line.xs.push_back(pl.getBlock()->wordXpos(w));
        }
        out.push_back(std::move(line));
      }
      pageIndex++;
    }
    return out;
  }

  // The glosses a strip shows (its empty marker words left out).
  static std::vector<std::string> shown(const Line& strip) {
    std::vector<std::string> out;
    for (const auto& w : strip.words)
      if (!w.empty()) out.push_back(w);
    return out;
  }

  static std::vector<Line> bodyOnly(const std::vector<Line>& lines) {
    std::vector<Line> out;
    for (const auto& l : lines)
      if (l.role == LineFontRole::Body) out.push_back(l);
    return out;
  }
};

constexpr const char* PROSE =
    "<p>The keeper whispered softly to the frightened mare, and the mare listened. Then the old keeper "
    "whispered again, and the night was quiet around the stable and the meadow beyond it.</p>"
    "<p>Second paragraph with an aardvark in it.</p>";

FakeGlosses proseGlosses() {
  FakeGlosses g;
  g.words["whispered"] = {6200, "шептал"};
  g.words["frightened"] = {7000, "испуганный"};
  g.words["mare"] = {9000, "кобыла"};
  g.words["meadow"] = {8000, "луг"};
  g.words["aardvark"] = {24000, "трубкозуб"};
  return g;
}

// ── Layout ───────────────────────────────────────────────────────────────────

TEST_F(WordLensLayoutTest, SourceLinesAreTheOriginalOnlyLines) {
  FakeGlosses glosses = proseGlosses();
  const auto original = layout(PROSE, LinguaLayout::OriginalOnly, nullptr);
  const auto lensed = bodyOnly(layout(PROSE, LinguaLayout::WordLens, &glosses));
  ASSERT_EQ(lensed.size(), original.size());
  for (size_t i = 0; i < original.size(); i++) {
    EXPECT_EQ(lensed[i].words, original[i].words) << "line " << i;
    EXPECT_EQ(lensed[i].xs, original[i].xs) << "line " << i;
    EXPECT_EQ(lensed[i].x, original[i].x) << "line " << i;
  }
}

TEST_F(WordLensLayoutTest, EverySourceLineGetsAStripSlot) {
  FakeGlosses glosses = proseGlosses();
  const auto original = layout(PROSE, LinguaLayout::OriginalOnly, nullptr);
  const auto lines = layout(PROSE, LinguaLayout::WordLens, &glosses);
  const auto body = bodyOnly(lines);
  // Uniform pitch: within a paragraph each source line sits one strip + one line below the last.
  ASSERT_GE(body.size(), 3u);
  EXPECT_EQ(body[0].y, original[0].y + LINE);
  EXPECT_EQ(body[1].y - body[0].y, LINE + LINE);
  // A strip line sits exactly one strip above the source line it annotates.
  for (size_t i = 0; i + 1 < lines.size(); i++) {
    if (lines[i].role != LineFontRole::Annotation) continue;
    EXPECT_EQ(lines[i + 1].role, LineFontRole::Body);
    EXPECT_EQ(lines[i + 1].y - lines[i].y, LINE);
  }
}

TEST_F(WordLensLayoutTest, HiddenEmptyStripsLeaveOnlyLinesWithHardWords) {
  FakeGlosses glosses = proseGlosses();
  glosses.keepEmpty = false;
  const auto original = layout(PROSE, LinguaLayout::OriginalOnly, nullptr);
  const auto lines = layout(PROSE, LinguaLayout::WordLens, &glosses);
  const auto body = bodyOnly(lines);
  ASSERT_EQ(body.size(), original.size());
  size_t strips = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].role == LineFontRole::Annotation) {
      strips++;
      continue;
    }
    // A source line with no strip right above it sits one line below the previous line.
    const bool hasStrip = i > 0 && lines[i - 1].role == LineFontRole::Annotation;
    if (!hasStrip && i > 0 && lines[i - 1].role == LineFontRole::Body && lines[i - 1].y / 10000 == lines[i].y / 10000) {
      EXPECT_EQ(lines[i].y - lines[i - 1].y, LINE) << "line " << i;
    }
  }
  EXPECT_GT(strips, 0u);
  EXPECT_LT(strips, body.size());  // some lines carry no hard word
  // Fewer strips, so the chapter is shorter than with every strip kept.
  FakeGlosses keep = proseGlosses();
  const auto kept = layout(PROSE, LinguaLayout::WordLens, &keep);
  EXPECT_LT(lines.back().y, kept.back().y);
}

TEST_F(WordLensLayoutTest, NoSourceMeansNoStrips) {
  const auto original = layout(PROSE, LinguaLayout::OriginalOnly, nullptr);
  const auto lensed = layout(PROSE, LinguaLayout::WordLens, nullptr);
  ASSERT_EQ(lensed.size(), original.size());
  for (size_t i = 0; i < original.size(); i++) {
    EXPECT_EQ(lensed[i].y, original[i].y);
    EXPECT_EQ(lensed[i].role, LineFontRole::Body);
  }
}

TEST_F(WordLensLayoutTest, GlossesAreCentredOverTheirWords) {
  FakeGlosses glosses;
  glosses.words["mare"] = {9000, "ab"};
  const auto lines = layout("<p>The quiet mare, slept.</p>", LinguaLayout::WordLens, &glosses);
  ASSERT_EQ(lines.size(), 2u);
  ASSERT_EQ(lines[0].role, LineFontRole::Annotation);
  ASSERT_EQ(shown(lines[0]), std::vector<std::string>{"ab"});
  // "mare," starts after "The quiet " (10 glyphs + the space gaps); its letters exclude the comma.
  const auto& src = lines[1];
  size_t w = 0;
  while (w < src.words.size() && src.words[w] != "mare,") w++;
  ASSERT_LT(w, src.words.size());
  const int wordCentre = src.xs[w] + 4 * GLYPH / 2;
  const int glossCentre = lines[0].xs[0] + 2 * GLYPH / 2;
  EXPECT_EQ(glossCentre, wordCentre);
}

TEST_F(WordLensLayoutTest, OnlyHardWordsAreGlossed) {
  FakeGlosses glosses = proseGlosses();
  const auto lines = layout(PROSE, LinguaLayout::WordLens, &glosses);
  std::vector<std::string> strip;
  for (const auto& l : lines)
    if (l.role == LineFontRole::Annotation) {
      const auto glosses = shown(l);
      strip.insert(strip.end(), glosses.begin(), glosses.end());
    }
  for (const auto& g : strip) {
    bool known = false;
    for (const auto& [k, v] : glosses.words) known = known || g.rfind(v.second.substr(0, 3), 0) == 0;
    EXPECT_TRUE(known) << g;
  }
  EXPECT_FALSE(strip.empty());
}

TEST_F(WordLensLayoutTest, RubyWordsKeepTheirOwnAnnotation) {
  FakeGlosses glosses;
  glosses.words["mare"] = {9000, "кобыла"};
  const auto lines =
      layout("<p>A <ruby>mare<rt>uma</rt></ruby> and a mare.</p>", LinguaLayout::WordLens, &glosses, 400);
  size_t strips = 0;
  size_t glossesShown = 0;
  for (const auto& l : lines) {
    if (l.role != LineFontRole::Annotation) continue;
    strips++;
    glossesShown += shown(l).size();
  }
  EXPECT_EQ(strips, 1u);
  EXPECT_EQ(glossesShown, 1u);  // the second, plain "mare" only
}

TEST_F(WordLensLayoutTest, CapitalisedWordsOpenSentencesOrAreNames) {
  FakeGlosses glosses;
  glosses.words["mare"] = {9000, "кобыла"};
  layout("<p>Mare ran. Mare stopped, and Mare sat.</p>", LinguaLayout::WordLens, &glosses, 400);
  std::vector<bool> initial;
  for (const auto& [key, isInitial] : glosses.asked)
    if (key == "mare") initial.push_back(isInitial);
  EXPECT_EQ(initial, (std::vector<bool>{true, true, false}));
}

TEST_F(WordLensLayoutTest, StripAndLineNeverSplitAcrossPages) {
  FakeGlosses glosses = proseGlosses();
  // 3 groups of 32 px fit a 100 px page.
  const auto lines = layout(PROSE, LinguaLayout::WordLens, &glosses, 240, 100);
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].role != LineFontRole::Annotation) continue;
    ASSERT_LT(i + 1, lines.size());
    EXPECT_EQ(lines[i + 1].y / 10000, lines[i].y / 10000) << "strip on another page than its line";
  }
  for (const auto& page : pages) EXPECT_FALSE(page->elements.empty());
}

// ── Strip placement ──────────────────────────────────────────────────────────

int measureGlyphs(void*, const char* text) {
  int n = 0;
  for (; *text; text++) n += (static_cast<uint8_t>(*text) & 0xC0) != 0x80;
  return n * GLYPH;
}

StripGloss gloss(const int wordX, const int wordWidth, const uint16_t rank, const char* text) {
  StripGloss g;
  g.wordX = static_cast<int16_t>(wordX);
  g.wordWidth = static_cast<int16_t>(wordWidth);
  g.rank = rank;
  std::snprintf(g.text, sizeof(g.text), "%s", text);
  return g;
}

TEST(WordLensStrip, CentresAndClampsToTheLine) {
  StripGloss g[2] = {gloss(0, 16, 5000, "abcdef"), gloss(200, 40, 5000, "xy")};
  placeStripGlosses(g, 2, 240, 4, measureGlyphs, nullptr);
  EXPECT_TRUE(g[0].placed);
  EXPECT_EQ(g[0].x, 0);  // centred would start at -16: clamped to the line start
  EXPECT_TRUE(g[1].placed);
  EXPECT_EQ(g[1].x, 200 + 20 - 8);
}

TEST(WordLensStrip, TheRarerWordWinsACollision) {
  // Two adjacent words whose glosses both want x 20..100.
  StripGloss g[2] = {gloss(40, 16, 5000, "commonword"), gloss(64, 16, 9000, "rareglossy")};
  placeStripGlosses(g, 2, 240, 4, measureGlyphs, nullptr);
  ASSERT_TRUE(g[1].placed);
  EXPECT_FALSE(g[1].truncated);
  // The commoner one keeps what room is left to its left, shortened with an ellipsis, or is dropped.
  if (g[0].placed) {
    EXPECT_TRUE(g[0].truncated);
    EXPECT_LE(g[0].x + g[0].width + 4, g[1].x);
    EXPECT_NE(std::strstr(g[0].text, "\xE2\x80\xA6"), nullptr);
  }
}

TEST(WordLensStrip, TruncatesToTheGapWhenThreeGlyphsFit) {
  // Rare gloss at 100..180; the common word sits at 40..56 with room 0..96 to its left.
  StripGloss g[2] = {gloss(40, 16, 5000, "abcdefghijklmnopqrst"), gloss(120, 40, 9000, "RRRRRRRRRR")};
  placeStripGlosses(g, 2, 240, 4, measureGlyphs, nullptr);
  ASSERT_TRUE(g[1].placed);
  ASSERT_TRUE(g[0].placed);
  EXPECT_TRUE(g[0].truncated);
  EXPECT_LE(g[0].x + g[0].width, g[1].x - 4);
  EXPECT_GE(g[0].x, 0);
  EXPECT_EQ(std::string(g[0].text).substr(0, 3), "abc");
}

TEST(WordLensStrip, DropsWhenFewerThanThreeGlyphsFit) {
  StripGloss g[3] = {gloss(0, 16, 9000, "LLLLLLLLL"), gloss(80, 8, 5000, "abcdefgh"), gloss(90, 16, 9500, "RRRRRRRRR")};
  placeStripGlosses(g, 3, 240, 4, measureGlyphs, nullptr);
  EXPECT_TRUE(g[0].placed);
  EXPECT_TRUE(g[2].placed);
  EXPECT_FALSE(g[1].placed);
}

TEST(WordLensStrip, NoOverlapEver) {
  StripGloss g[6];
  for (int i = 0; i < 6; i++) g[i] = gloss(i * 30, 20, static_cast<uint16_t>(5000 + (i * 7919) % 3000), "glossgloss");
  placeStripGlosses(g, 6, 240, 4, measureGlyphs, nullptr);
  for (int a = 0; a < 6; a++)
    for (int b = a + 1; b < 6; b++)
      if (g[a].placed && g[b].placed) {
        EXPECT_TRUE(g[a].x + g[a].width + 4 <= g[b].x || g[b].x + g[b].width + 4 <= g[a].x) << a << "/" << b;
      }
}

// ── Navigation ───────────────────────────────────────────────────────────────

NavTarget target(const int16_t x) {
  NavTarget t;
  t.x = x;
  t.y = 100;
  t.width = 30;
  t.height = LINE;
  return t;
}

TEST(WordLensNavigation, NoFocusStartsAtEitherEnd) {
  const NavTarget t[4] = {target(0), target(40), target(80), target(120)};
  WordLensNavigator nav;
  nav.setTargetsForTest(t, 4);
  EXPECT_FALSE(nav.hasFocus());
  for (int i = 0; i < 3; i++) nav.next(false);
  EXPECT_EQ(nav.focus(), 2);  // Right x3 from no focus: the third word
  nav.clearFocus();
  for (int i = 0; i < 2; i++) nav.prev(false);
  EXPECT_EQ(nav.focus(), 2);  // Left x2 from no focus: also the third word
}

TEST(WordLensNavigation, EndsWrapOnlyWithLoop) {
  const NavTarget t[2] = {target(0), target(40)};
  WordLensNavigator nav;
  nav.setTargetsForTest(t, 2);
  nav.next(false);
  nav.next(false);
  EXPECT_EQ(nav.next(false), WordLensNavigator::Step::PastEnd);
  EXPECT_EQ(nav.focus(), 1);
  EXPECT_EQ(nav.next(true), WordLensNavigator::Step::Wrapped);
  EXPECT_EQ(nav.focus(), 0);
  EXPECT_EQ(nav.prev(false), WordLensNavigator::Step::PastStart);
  EXPECT_EQ(nav.prev(true), WordLensNavigator::Step::Wrapped);
  EXPECT_EQ(nav.focus(), 1);
  WordLensNavigator empty;
  EXPECT_EQ(empty.next(true), WordLensNavigator::Step::NoTargets);
}

TEST(WordLensNavigation, TapOnAWordOrItsGlossFocusesIt) {
  const NavTarget t[2] = {target(0), target(80)};
  WordLensNavigator nav;
  nav.setTargetsForTest(t, 2);
  EXPECT_TRUE(nav.focusAt(90, 105, LINE));  // the word
  EXPECT_EQ(nav.focus(), 1);
  EXPECT_TRUE(nav.focusAt(5, 90, LINE));  // its gloss, in the strip above
  EXPECT_EQ(nav.focus(), 0);
  EXPECT_FALSE(nav.focusAt(50, 105, LINE));
  EXPECT_FALSE(nav.focusAt(5, 60, LINE));
}

TEST_F(WordLensLayoutTest, TargetsIncludeWordsWhoseGlossWasDropped) {
  // Three hard words packed so tightly that the strip cannot show every gloss.
  FakeGlosses glosses;
  glosses.words["aa"] = {9000, "glossforaa"};
  glosses.words["bb"] = {5000, "glossforbb"};
  glosses.words["cc"] = {9500, "glossforcc"};
  const auto lines = layout("<p>aa bb cc</p>", LinguaLayout::WordLens, &glosses, 240);
  size_t glossCount = 0;
  for (const auto& l : lines)
    if (l.role == LineFontRole::Annotation) glossCount += shown(l).size();
  EXPECT_LT(glossCount, 3u);
  ASSERT_EQ(pages.size(), 1u);
  WordLensNavigator nav;
  nav.collect(renderer, *pages[0], 0, &FakeGlosses::entry, &glosses);
  EXPECT_EQ(nav.count(), 3u);
  for (size_t i = 0; i < nav.count(); i++) EXPECT_TRUE(nav.target(i).hasEntry);
  // Reading order.
  EXPECT_LT(nav.target(0).x, nav.target(1).x);
  EXPECT_LT(nav.target(1).x, nav.target(2).x);
}

TEST_F(WordLensLayoutTest, TargetsAreExactlyTheLayoutsHardWords) {
  // "Mare" is in the pack three times, but layout takes the mid-sentence one for a name: navigation
  // must follow the layout's decision, not re-decide it.
  FakeGlosses glosses;
  glosses.words["mare"] = {9000, "кобыла"};
  layout("<p>Mare ran. Mare stopped, and Mare sat.</p>", LinguaLayout::WordLens, &glosses, 400);
  ASSERT_EQ(pages.size(), 1u);
  WordLensNavigator nav;
  nav.collect(renderer, *pages[0], 0, &FakeGlosses::entry, &glosses);
  ASSERT_EQ(nav.count(), 2u);
  EXPECT_STREQ(nav.target(0).surface, "mare");
  EXPECT_LT(nav.target(0).x, nav.target(1).x);
}

TEST(WordLensNavigation, LookupFallsBackFromDictionaryToLemmaToGloss) {
  EXPECT_EQ(decideLookup(true, true, true, true), LookupAction::DictionarySurface);
  EXPECT_EQ(decideLookup(true, false, true, true), LookupAction::DictionaryLemma);
  EXPECT_EQ(decideLookup(true, false, false, true), LookupAction::GlossPopup);
  EXPECT_EQ(decideLookup(true, false, true, false), LookupAction::GlossPopup);
  EXPECT_EQ(decideLookup(false, true, true, true), LookupAction::GlossPopup);
}

}  // namespace
