// CrossLingua fork tests: Section's view of a translated sidecar's offset space (LinguaSourceKind.h)
// -- settled before startBuild returns, and read from the .bin header by a Section that never
// loaded it (KOReader sync's temporary Section).
#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

void Hyphenator::setPreferredLanguage(const std::string&) {}

namespace {

std::string chapter(const bool v2) {
  std::string body;
  for (int i = 0; i < 120; i++) {
    const std::string n = std::to_string(i);
    body += "\n<p>Paragraph " + n + " was one fine day.</p>";
    body += "<p lang=\"uk\" data-translation=\"true\" dir=\"auto\">UK paragraph " + n + " translated.</p>";
  }
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"") +
         (v2 ? " data-crosslingua-sidecar=\"2\"" : "") + "><head><title>T</title></head>\n<body>" + body +
         "\n</body></html>\n";
}

class LinguaSectionTest : public ::testing::TestWithParam<bool> {
 protected:
  std::string cacheDir;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : name) {
      if (c == '/') c = '_';
    }
    cacheDir = "/tmp/crosspoint_lingua_section_" + name;
    std::system(("rm -rf " + cacheDir).c_str());
    Storage.mkdir(cacheDir.c_str());
    Storage.mkdir((cacheDir + "/sections").c_str());
    std::ofstream(cacheDir + "/sections/0.translated.html", std::ios::binary) << chapter(GetParam());
    epub = std::make_shared<Epub>(cacheDir);
  }
  void TearDown() override { std::system(("rm -rf " + cacheDir).c_str()); }

  ReaderRenderSpec spec() const {
    ReaderRenderSpec s;
    s.viewportWidth = static_cast<uint16_t>(renderer.getScreenWidth());
    s.viewportHeight = static_cast<uint16_t>(renderer.getScreenHeight());
    s.embeddedStyle = false;
    return s;
  }
};

// An original-text offset (the start of the second original paragraph) as the untranslated chapter
// counts it: "\n" + "Paragraph 0 was one fine day." + "\n".
constexpr uint32_t ORIGINAL_OFFSET = 31;

// Right after startBuild -- before any explicit buildSomeMore -- the kind is final, so a reopen's
// offset lookup already behaves per its source.
TEST_P(LinguaSectionTest, KindIsSettledWhenStartBuildReturns) {
  const bool v2 = GetParam();
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.startBuild(spec()));
  EXPECT_EQ(section.linguaSourceKind(), v2 ? LinguaSourceKind::SidecarV2 : LinguaSourceKind::LegacySidecar);
  if (v2) {
    // The reopen loop keeps building until the original offset is reached -- it is not rejected
    // as another offset space (the provisional-legacy bug answered "reached" and then nothing).
    int guard = 0;
    while (!section.buildReachedVisibleTextOffset(ORIGINAL_OFFSET) && guard++ < 50) {
      ASSERT_TRUE(section.buildSomeMore(1));
    }
    EXPECT_TRUE(section.buildReachedVisibleTextOffset(ORIGINAL_OFFSET));
    EXPECT_EQ(section.getPageForVisibleTextOffset(ORIGINAL_OFFSET), std::optional<uint16_t>(0));
  } else {
    // An original-text offset never resolves in a legacy sidecar's space, and never makes a build wait.
    EXPECT_TRUE(section.buildReachedVisibleTextOffset(ORIGINAL_OFFSET));
    EXPECT_EQ(section.getPageForVisibleTextOffset(ORIGINAL_OFFSET), std::nullopt);
  }
  if (!section.isBuildComplete()) ASSERT_TRUE(section.buildSomeMore(0));
  EXPECT_TRUE(section.isBuildComplete());
}

// A file small enough to parse completely while startBuild settles the kind still finalizes.
TEST(LinguaSectionTinyTest, SettlingToTheEndStillFinalizes) {
  const std::string cacheDir = "/tmp/crosspoint_lingua_section_tiny";
  std::system(("rm -rf " + cacheDir).c_str());
  Storage.mkdir(cacheDir.c_str());
  Storage.mkdir((cacheDir + "/sections").c_str());
  std::ofstream(cacheDir + "/sections/0.translated.html", std::ios::binary)
      << "<html data-crosslingua-sidecar=\"2\"><body><p>Hi.</p></body></html>";
  auto epub = std::make_shared<Epub>(cacheDir);
  GfxRenderer renderer;
  ReaderRenderSpec spec;
  spec.viewportWidth = 480;
  spec.viewportHeight = 800;
  spec.embeddedStyle = false;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.startBuild(spec));
  EXPECT_EQ(section.linguaSourceKind(), LinguaSourceKind::SidecarV2);
  ASSERT_TRUE(section.buildSomeMore(1));
  EXPECT_TRUE(section.isBuildComplete());
  EXPECT_EQ(section.pageCount, 1);
  std::system(("rm -rf " + cacheDir).c_str());
}

// A Section that never loads the file (ProgressMapper's temporary one) takes the kind from the
// header: a legacy chapter rejects an original-space offset instead of answering page 0.
TEST_P(LinguaSectionTest, UnloadedSectionReadsTheKindFromTheHeader) {
  const bool v2 = GetParam();
  {
    Section build(epub, 0, renderer);
    ASSERT_TRUE(build.startBuild(spec()));
    ASSERT_TRUE(build.buildSomeMore(0));
    ASSERT_GT(build.pageCount, 1);
  }
  const Section temp(epub, 0, renderer);
  EXPECT_EQ(temp.linguaSourceKind(), v2 ? LinguaSourceKind::SidecarV2 : LinguaSourceKind::LegacySidecar);
  const uint32_t lateOriginalOffset = 2000;
  if (v2) {
    EXPECT_TRUE(temp.getPageForVisibleTextOffset(lateOriginalOffset).has_value());
  } else {
    EXPECT_EQ(temp.getPageForVisibleTextOffset(lateOriginalOffset), std::nullopt);
    EXPECT_EQ(temp.getParagraphIndexForPage(1), std::nullopt);  // legacy paragraph LUT is absent
  }
}

INSTANTIATE_TEST_SUITE_P(SidecarFormat, LinguaSectionTest, ::testing::Bool());

}  // namespace
