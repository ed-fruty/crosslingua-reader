// CrossLingua fork tests: Section's offset lookups for a chapter laid out from a translated
// `.translated.html` sidecar, whose inserted translations add no visible-text offset.
#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

void Hyphenator::setPreferredLanguage(const std::string&) {}

namespace {

class SidecarTieTest : public ::testing::Test {
 protected:
  std::string cacheDir = "/tmp/crosspoint_lingua_section_ties";
  std::string sidecarPath = cacheDir + "/sections/0.translated.html";
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    std::system(("rm -rf " + cacheDir).c_str());
    Storage.mkdir(cacheDir.c_str());
    Storage.mkdir((cacheDir + "/sections").c_str());
    // One short original, then an inserted translation long enough for several pages.
    std::string translation;
    for (int i = 0; i < 2500; i++) translation += "slovo" + std::to_string(i) + " ";
    std::ofstream(sidecarPath, std::ios::binary)
        << "<html><head><title>T</title></head><body><p>Short original.</p><p lang=\"uk\" "
           "data-translation=\"true\" dir=\"auto\">"
        << translation << "</p><p>Tail.</p></body></html>";
    epub = std::make_shared<Epub>(cacheDir);
  }
  void TearDown() override { std::system(("rm -rf " + cacheDir).c_str()); }

  ReaderRenderSpec spec() const {
    ReaderRenderSpec s;
    s.viewportWidth = static_cast<uint16_t>(renderer.getScreenWidth());
    s.viewportHeight = static_cast<uint16_t>(renderer.getScreenHeight());
    s.embeddedStyle = false;
    s.linguaLayout = LinguaLayout::TranslationOnly;
    return s;
  }
};

// Pages inside one inserted translation share their start offset. A lookup of that offset lands on
// the caller's saved page when it is in the run (a reopen stays where the reader was); otherwise it
// answers exactly as upstream.
TEST_F(SidecarTieTest, TiesResolveToTheSavedPage) {
  {
    Section build(epub, 0, renderer);
    ASSERT_TRUE(build.createSectionFile(spec()));
  }
  const Section section(epub, 0, renderer);
  const auto count = section.getCachedPageCount();
  ASSERT_TRUE(count.has_value());
  ASSERT_GE(*count, 4);
  // The run: pages 1.. that start at page 1's offset.
  const auto runOffset = section.getVisibleTextOffsetForPage(1);
  ASSERT_TRUE(runOffset.has_value());
  uint16_t last = 1;
  while (last + 1 < *count && section.getVisibleTextOffsetForPage(last + 1) == runOffset) last++;
  ASSERT_GE(last, 2) << "the translation must span several pages";

  // No hint: upstream's answer (the last page of the run, or the first with preferFirstAtOffset).
  EXPECT_EQ(section.getPageForVisibleTextOffset(*runOffset), std::optional<uint16_t>(last));
  EXPECT_EQ(section.getPageForVisibleTextOffset(*runOffset, true), std::optional<uint16_t>(1));
  // A saved page inside the run wins.
  section.linguaSetTieBreakPage(2);
  EXPECT_EQ(section.getPageForVisibleTextOffset(*runOffset), std::optional<uint16_t>(2));
  // Consumed by that lookup; and a hint outside the run changes nothing.
  EXPECT_EQ(section.getPageForVisibleTextOffset(*runOffset), std::optional<uint16_t>(last));
  section.linguaSetTieBreakPage(*count + 5);
  EXPECT_EQ(section.getPageForVisibleTextOffset(*runOffset), std::optional<uint16_t>(last));

  // Without a sidecar the hint is ignored: upstream's answer.
  std::remove(sidecarPath.c_str());
  const Section original(epub, 0, renderer);
  original.linguaSetTieBreakPage(2);
  EXPECT_EQ(original.getPageForVisibleTextOffset(*runOffset), std::optional<uint16_t>(last));
}

}  // namespace
