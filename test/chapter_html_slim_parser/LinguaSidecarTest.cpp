// CrossLingua fork tests: visible-text offsets of a translated `.translated.html` sidecar.
//
// Progress, bookmarks, clippings and KOReader sync store visible-text offsets of the chapter's
// ORIGINAL text. A v2 sidecar (TranslationHtmlRewriter) must therefore produce exactly the offsets the
// chapter's own XHTML produces for every original word and page, and a legacy (marker-less) sidecar
// must never be mistaken for original-text positions. See LinguaSourceKind.h.
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Print.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "modules/lingua/modes/common/PageTextJoiner.h"
#include "modules/lingua/parsers/TranslationHtmlRewriter.h"
#include "modules/lingua/parsers/TranslationPairWalker.h"
#include "modules/lingua/services/LinguaSourceKind.h"

namespace {

struct StringPrint final : Print {
  std::string out;
  size_t write(const uint8_t b) override {
    out.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* data, const size_t length) override {
    out.append(reinterpret_cast<const char*>(data), length);
    return length;
  }
};

struct WordRec {
  std::string text;
  bool translated;
  uint32_t start;
  uint32_t end;
  bool operator==(const WordRec& o) const {
    return text == o.text && translated == o.translated && start == o.start && end == o.end;
  }
};

void PrintTo(const WordRec& w, std::ostream* os) {
  *os << "{" << w.text << (w.translated ? " T" : "") << " [" << w.start << "," << w.end << ")}";
}

struct Parsed {
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<uint32_t> pageOffsets;
  uint16_t lastParagraphIndex = 0;
  uint16_t lastListItemIndex = 0;
  LinguaSourceKind kind = LinguaSourceKind::Original;  // as the parser settled it

  std::vector<WordRec> words() const {
    std::vector<WordRec> out;
    for (const auto& page : pages) {
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& b = *static_cast<const PageLine&>(*el).getBlock();
        for (uint16_t i = 0; i < b.wordCount(); i++) {
          out.push_back(
              {b.wordText(i), b.linguaWordTranslated(i), b.wordSourceRange(i).start, b.wordSourceRange(i).end});
        }
      }
    }
    return out;
  }
  std::vector<WordRec> originalWords() const {
    std::vector<WordRec> out;
    for (auto& w : words()) {
      if (!w.translated) out.push_back(w);
    }
    return out;
  }
};

std::string tmpPath(const char* tag) {
  std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
  for (char& c : name) {
    if (c == '/') c = '_';  // parameterized test names carry a '/'
  }
  return std::string("/tmp/crosspoint_lingua_sidecar_") + tag + "_" + name + ".xhtml";
}

void writeFile(const std::string& path, const std::string& content) {
  std::ofstream(path, std::ios::binary) << content;
}

std::string rewrite(const std::string& source) {
  TranslationHtmlRewriter rewriter;
  rewriter.setBookPrimaryLang("en");
  StringPrint out;
  volatile bool cancelled = false;
  const auto result = rewriter.rewrite(source.data(), source.size(), out, "en", "uk", 0, "", &cancelled);
  EXPECT_FALSE(result.cancelled);
  EXPECT_FALSE(result.abortedOnErrors);
  return out.out;
}

Parsed parse(const std::string& path, const LinguaSourceKind kind, const LinguaLayout layout) {
  Parsed parsed;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
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
                               [&parsed](std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                                         const uint16_t listItemIndex, const uint32_t visibleOffset) {
                                 parsed.pages.push_back(std::move(page));
                                 parsed.pageOffsets.push_back(visibleOffset);
                                 parsed.lastParagraphIndex = paragraphIndex;
                                 parsed.lastListItemIndex = listItemIndex;
                               },
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser,
                               layout,
                               "en"};
  parser.setLinguaSourceKind(kind);
  EXPECT_TRUE(parser.beginParse());
  for (int guard = 0; guard < 100000; guard++) {
    const auto status = parser.parseStep();
    EXPECT_NE(status, ChapterHtmlSlimParser::ParseStatus::Error);
    if (status != ChapterHtmlSlimParser::ParseStatus::More) break;
  }
  parser.finishParse();
  parsed.kind = parser.linguaSourceKind();
  return parsed;
}

std::string rewrite(const std::string& source, const char* bookLang) {
  TranslationHtmlRewriter rewriter;
  rewriter.setBookPrimaryLang(bookLang);
  StringPrint out;
  volatile bool cancelled = false;
  rewriter.rewrite(source.data(), source.size(), out, "en", "uk", 0, "", &cancelled);
  return out.out;
}

// A chapter long enough to span several pages, with inline markup, a list and pretty-printing
// whitespace between blocks -- every place the old rewriter added or dropped characters.
std::string chapter(const bool withEmbeddedTranslation) {
  std::string body;
  for (int i = 0; i < 90; i++) {
    const std::string n = std::to_string(i);
    body += "\n  <p>Paragraph " + n + " was one fine day, a <b>well-known</b> fact.</p>";
    if (withEmbeddedTranslation && i % 3 == 0) body += "\n  <p lang=\"de\">Absatz " + n + " alt.</p>";
    if (i % 5 == 0) body += "\n  <ul>\n    <li>Item " + n + "a</li>\n    <li>Item " + n + "b</li>\n  </ul>";
  }
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
         "<head><title>T</title></head>\n<body>" +
         body + "\n</body></html>\n";
}

class SidecarOffsetsTest : public ::testing::TestWithParam<bool> {
 protected:
  std::string originalPath = tmpPath("orig");
  std::string sidecarPath = tmpPath("v2");
  std::string sidecar;

  void SetUp() override {
    const std::string source = chapter(GetParam());
    writeFile(originalPath, source);
    sidecar = rewrite(source);
    writeFile(sidecarPath, sidecar);
  }
  void TearDown() override {
    std::remove(originalPath.c_str());
    std::remove(sidecarPath.c_str());
  }
};

// The rewriter stamps the format, keeps an existing translation hidden, and adds no whitespace.
TEST_P(SidecarOffsetsTest, RewriterWritesFormatTwo) {
  EXPECT_NE(sidecar.find("<html xmlns=\"http://www.w3.org/1999/xhtml\" data-crosslingua-sidecar=\"2\">"),
            std::string::npos);
  EXPECT_NE(sidecar.find("data-translation=\"true\""), std::string::npos);
  EXPECT_EQ(sidecar.find("</p>\n<p lang=\"uk\""), std::string::npos);
  if (GetParam()) {
    EXPECT_NE(sidecar.find("<p lang=\"de\" hidden=\"hidden\">Absatz 0 alt.</p>"), std::string::npos);
  }
}

// Every original word keeps exactly its original source range, and the XPath paragraph / list-item
// counts (KOReader sync) match the chapter's own XHTML.
TEST_P(SidecarOffsetsTest, OriginalWordsKeepTheirOffsets) {
  const Parsed original = parse(originalPath, LinguaSourceKind::Original, LinguaLayout::Both);
  const Parsed translated = parse(sidecarPath, LinguaSourceKind::LegacySidecar, LinguaLayout::Both);
  EXPECT_EQ(translated.kind, LinguaSourceKind::SidecarV2);  // upgraded at the root element
  const auto originalWords = original.originalWords();
  ASSERT_GT(originalWords.size(), 100u);
  EXPECT_EQ(translated.originalWords(), originalWords);
  EXPECT_EQ(translated.lastParagraphIndex, original.lastParagraphIndex);
  EXPECT_EQ(translated.lastListItemIndex, original.lastListItemIndex);

  // The inserted translation has no position in the original.
  size_t translatedWords = 0;
  for (const auto& w : translated.words()) {
    if (!w.translated) continue;
    translatedWords++;
    EXPECT_EQ(w.start, UINT32_MAX) << w.text;
  }
  EXPECT_GT(translatedWords, 0u);
}

// With the translation filtered out the sidecar lays out exactly the original's pages, so the page
// LUT -- what progress and sync store -- is identical.
TEST_P(SidecarOffsetsTest, OriginalOnlyPagesHaveTheOriginalLut) {
  const Parsed original = parse(originalPath, LinguaSourceKind::Original, LinguaLayout::OriginalOnly);
  const Parsed translated = parse(sidecarPath, LinguaSourceKind::LegacySidecar, LinguaLayout::OriginalOnly);
  ASSERT_GT(original.pageOffsets.size(), 2u);
  EXPECT_EQ(translated.pageOffsets, original.pageOffsets);
}

// A clipping anchored on the untranslated chapter highlights the same word after translation.
TEST_P(SidecarOffsetsTest, ClippingSurvivesTranslation) {
  const Parsed original = parse(originalPath, LinguaSourceKind::Original, LinguaLayout::Both);
  const Parsed translated = parse(sidecarPath, LinguaSourceKind::LegacySidecar, LinguaLayout::Both);
  WordRec clip{};
  bool found = false;
  for (const auto& w : original.originalWords()) {
    if (w.text == "fine" && found) {  // the second paragraph's "fine"
      clip = w;
      break;
    }
    if (w.text == "fine") found = true;
  }
  ASSERT_NE(clip.start, 0u);
  // ClippingStore's range rule: overlapping, known source range.
  std::vector<WordRec> hits;
  for (const auto& w : translated.words()) {
    if (w.start != UINT32_MAX && w.end > clip.start && w.start < clip.end) hits.push_back(w);
  }
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0], clip);
}

// Tooltip / Page Translation match original page text against the sidecar's pairs. Joined from the
// v2 sidecar's own pages, every paragraph finds its NEW translation -- never the hidden old one.
TEST_P(SidecarOffsetsTest, PairWalkerMatchesV2Pages) {
  const Parsed translated = parse(sidecarPath, LinguaSourceKind::LegacySidecar, LinguaLayout::OriginalOnly);
  std::vector<std::string> visible;
  int16_t lastIdx = -1;
  lingua::PageTextJoiner joiner;
  for (const auto& el : translated.pages.front()->elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (visible.empty() || line.paragraphIdx != lastIdx) {
      visible.emplace_back();
      joiner.reset();
      lastIdx = line.paragraphIdx;
    }
    for (uint16_t i = 0; i < line.getBlock()->wordCount(); i++) joiner.append(visible.back(), *line.getBlock(), i);
  }
  ASSERT_GE(visible.size(), 2u);
  std::vector<lingua::PageMatch> matches(visible.size());
  ASSERT_TRUE(
      lingua::matchPageParagraphs(sidecarPath, "en", visible.data(), static_cast<int>(visible.size()), matches.data()));
  EXPECT_TRUE(matches[0].matched);
  EXPECT_EQ(matches[0].translation.rfind("UK:Paragraph 0 was one fine day", 0), 0u) << matches[0].translation;
  for (const auto& m : matches) EXPECT_EQ(m.translation.find("Absatz"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(EmbeddedTranslation, SidecarOffsetsTest, ::testing::Bool());

// A sidecar written before format 2 appends a newline after every block and drops existing
// translations, so its offsets cannot be mapped. They live in their own space: no original-text
// position resolves against it, and none of its positions resolves against an original layout.
TEST(LegacySidecarTest, OffsetsLiveInTheirOwnSpace) {
  const std::string source = chapter(false);
  std::string legacy = rewrite(source);
  // Format 1: no format attribute, and a newline after every closing block tag.
  const std::string attr = " data-crosslingua-sidecar=\"2\"";
  const size_t marker = legacy.find(attr);
  ASSERT_NE(marker, std::string::npos);
  legacy.erase(marker, attr.size());
  for (const char* close : {"</p>", "</li>"}) {
    for (size_t at = legacy.find(close); at != std::string::npos; at = legacy.find(close, at + 1)) {
      legacy.insert(at + strlen(close), "\n");
    }
  }

  const std::string originalPath = tmpPath("orig");
  const std::string legacyPath = tmpPath("legacy");
  writeFile(originalPath, source);
  writeFile(legacyPath, legacy);
  const Parsed original = parse(originalPath, LinguaSourceKind::Original, LinguaLayout::Both);
  const Parsed parsed = parse(legacyPath, LinguaSourceKind::LegacySidecar, LinguaLayout::Both);
  EXPECT_EQ(parsed.kind, LinguaSourceKind::LegacySidecar);
  std::remove(originalPath.c_str());
  std::remove(legacyPath.c_str());

  for (const auto& w : parsed.words()) {
    if (w.start == UINT32_MAX) continue;
    EXPECT_TRUE(lingua::isLegacySidecarOffset(w.start)) << w.text;
  }
  // An original clipping range matches no legacy word.
  const WordRec clip = original.originalWords()[5];
  for (const auto& w : parsed.words()) {
    EXPECT_FALSE(w.start != UINT32_MAX && w.end > clip.start && w.start < clip.end) << w.text;
  }
  // Page offsets: the first page is 0 in every space, the rest are legacy-space.
  ASSERT_GT(parsed.pageOffsets.size(), 1u);
  EXPECT_EQ(parsed.pageOffsets[0], 0u);
  for (size_t i = 1; i < parsed.pageOffsets.size(); i++)
    EXPECT_TRUE(lingua::isLegacySidecarOffset(parsed.pageOffsets[i]));

  using lingua::sourceAcceptsVisibleTextOffset;
  EXPECT_FALSE(sourceAcceptsVisibleTextOffset(LinguaSourceKind::LegacySidecar, original.pageOffsets.back()));
  EXPECT_TRUE(sourceAcceptsVisibleTextOffset(LinguaSourceKind::LegacySidecar, parsed.pageOffsets.back()));
  EXPECT_FALSE(sourceAcceptsVisibleTextOffset(LinguaSourceKind::SidecarV2, parsed.pageOffsets.back()));
  EXPECT_TRUE(sourceAcceptsVisibleTextOffset(LinguaSourceKind::Original, original.pageOffsets.back()));
  EXPECT_TRUE(sourceAcceptsVisibleTextOffset(LinguaSourceKind::LegacySidecar, 0));
}

}  // namespace

namespace {

// A long inserted translation spans pages that all start at the offset it was inserted at; the
// lookup tie rule picks the saved page within such a run, else its first page.
TEST(SidecarTiesTest, LongTranslationPagesShareAStartOffset) {
  std::string body = "<p>Short original.</p>";
  std::string longText;
  for (int i = 0; i < 200; i++) longText += "word" + std::to_string(i) + " ";
  body += "<p>" + longText + "</p><p>Tail original.</p>";
  const std::string source = "<html><head><title>T</title></head><body>" + body + "</body></html>";
  const std::string path = tmpPath("ties");
  writeFile(path, rewrite(source));
  const Parsed parsed = parse(path, LinguaSourceKind::LegacySidecar, LinguaLayout::TranslationOnly);
  std::remove(path.c_str());
  ASSERT_EQ(parsed.kind, LinguaSourceKind::SidecarV2);
  ASSERT_GE(parsed.pageOffsets.size(), 3u);
  // Translation-only pages of the two inserted translations: the long one spans several pages,
  // all starting at the same original-text offset.
  size_t tied = 0;
  for (size_t i = 1; i < parsed.pageOffsets.size(); i++) {
    if (parsed.pageOffsets[i] == parsed.pageOffsets[i - 1]) tied++;
  }
  EXPECT_GE(tied, 1u);

  EXPECT_EQ(lingua::pickTiePage(3, 6, 5), 5);   // saved page inside the run
  EXPECT_EQ(lingua::pickTiePage(3, 6, 9), 3);   // saved page elsewhere: the first
  EXPECT_EQ(lingua::pickTiePage(3, 6, -1), 3);  // no saved page
}

// A CDATA section outside any block stays CDATA: its text is not escaped, so the parser counts the
// same characters as in the original.
TEST(SidecarRewriterTest, CdataOutsideBlocksStaysLiteral) {
  const std::string source =
      "<html><head><title>T</title></head><body><div><![CDATA[a < b & c]]></div><p>One.</p></body></html>";
  const std::string out = rewrite(source);
  EXPECT_NE(out.find("<![CDATA[a < b & c]]>"), std::string::npos) << out;
  const std::string origPath = tmpPath("cdata_orig");
  const std::string sidecarPath = tmpPath("cdata_v2");
  writeFile(origPath, source);
  writeFile(sidecarPath, out);
  const Parsed original = parse(origPath, LinguaSourceKind::Original, LinguaLayout::Both);
  const Parsed translated = parse(sidecarPath, LinguaSourceKind::LegacySidecar, LinguaLayout::Both);
  std::remove(origPath.c_str());
  std::remove(sidecarPath.c_str());
  EXPECT_EQ(translated.originalWords(), original.originalWords());
}

// A block tagged with the book's OWN language is original text: translated, not hidden. Only a
// differing language marks an existing translation.
TEST(SidecarRewriterTest, OwnLanguageBlocksAreNotExistingTranslations) {
  const std::string source =
      "<html><head><title>T</title></head><body><p xml:lang=\"en\">Own.</p><p lang=\"de\">Fremd.</p></body></html>";
  const std::string out = rewrite(source, "en");
  EXPECT_NE(out.find("<p xml:lang=\"en\">Own.</p><p lang=\"uk\""), std::string::npos) << out;
  EXPECT_NE(out.find("<p lang=\"de\" hidden=\"hidden\">Fremd.</p>"), std::string::npos) << out;
  // Unknown book language keeps the conservative rule: any lang= block is an existing translation.
  const std::string unknown = rewrite(source, "");
  EXPECT_NE(unknown.find("<p xml:lang=\"en\" hidden=\"hidden\">Own.</p>"), std::string::npos) << unknown;
}

}  // namespace
