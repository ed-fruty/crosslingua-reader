// CrossLingua Word Lens: the .lens pack format, its lookup, gloss cleaning, level cutoffs and the
// on-device JSON -> .lens converter (byte-identical to scripts/wordlens/lens_format.py).
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/ReaderRenderSpec.h"
#include "modules/lingua/services/LinguaCacheKey.h"
#include "modules/lingua/wordlens/GlossClean.h"
#include "modules/lingua/wordlens/LensBuilder.h"
#include "modules/lingua/wordlens/LensFormat.h"
#include "modules/lingua/wordlens/LensReader.h"
#include "modules/lingua/wordlens/LensText.h"
#include "modules/lingua/wordlens/WordLensGlosser.h"
#include "modules/lingua/wordlens/WordLensLevels.h"

using namespace lingua::wordlens;

namespace {

std::string fixture(const char* name) { return std::string(WORD_LENS_FIXTURES) + "/" + name; }

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// The Python-built .lens fixtures are committed as hex text (<name>.lens.hex).
std::vector<uint8_t> readLensFixture(const std::string& lensName) {
  std::ifstream in(fixture((lensName + ".hex").c_str()));
  std::vector<uint8_t> out;
  std::string digits;
  std::string line;
  while (std::getline(in, line)) digits += line;
  for (size_t i = 0; i + 1 < digits.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoi(digits.substr(i, 2), nullptr, 16)));
  }
  return out;
}

class MemorySource final : public LensByteSource {
 public:
  explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  bool readAt(const uint32_t offset, void* buffer, const size_t length) override {
    reads++;
    if (offset + length > bytes_.size()) return false;
    std::memcpy(buffer, bytes_.data() + offset, length);
    return true;
  }
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  int reads = 0;

 private:
  std::vector<uint8_t> bytes_;
};

class StdioFile final : public LensFile {
 public:
  explicit StdioFile(std::FILE* f) : f_(f) {}
  ~StdioFile() override { std::fclose(f_); }
  size_t read(void* buffer, const size_t length) override { return std::fread(buffer, 1, length, f_); }
  bool write(const void* buffer, const size_t length) override { return std::fwrite(buffer, 1, length, f_) == length; }
  bool seek(const uint32_t position) override { return std::fseek(f_, static_cast<long>(position), SEEK_SET) == 0; }
  uint32_t size() override {
    const long at = std::ftell(f_);
    std::fseek(f_, 0, SEEK_END);
    const long end = std::ftell(f_);
    std::fseek(f_, at, SEEK_SET);
    return static_cast<uint32_t>(end);
  }

 private:
  std::FILE* f_;
};

class StdioFs final : public LensFs {
 public:
  std::unique_ptr<LensFile> openRead(const char* path) override {
    std::FILE* f = std::fopen(path, "rb");
    return f ? std::make_unique<StdioFile>(f) : nullptr;
  }
  std::unique_ptr<LensFile> openWrite(const char* path) override {
    std::FILE* f = std::fopen(path, "w+b");
    opened++;
    return f ? std::make_unique<StdioFile>(f) : nullptr;
  }
  bool remove(const char* path) override { return std::remove(path) == 0; }
  bool rename(const char* from, const char* to) override { return std::rename(from, to) == 0; }
  int opened = 0;
};

std::string tempDir() {
  const char* base = std::getenv("TMPDIR");
  std::string dir = std::string(base ? base : "/tmp") + "/wordlens_test_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name();
  std::string cmd = "mkdir -p '" + dir + "'";
  EXPECT_EQ(std::system(cmd.c_str()), 0);
  return dir;
}

struct OpenPack {
  std::unique_ptr<MemorySource> source;
  LensReader reader;
  explicit OpenPack(const char* name) : source(std::make_unique<MemorySource>(readLensFixture(name))) {
    EXPECT_TRUE(reader.open(source.get()));
  }
};

bool lookupWord(LensReader& reader, const char* word, LensReader::Hit& hit) {
  return reader.lookup(word, std::strlen(word), hit);
}

std::string lemmaOf(LensReader& reader, const LensReader::Hit& hit) {
  char lemma[256];
  char gloss[256];
  EXPECT_TRUE(reader.readEntry(hit.entryOffset, lemma, sizeof(lemma), gloss, sizeof(gloss)));
  return lemma;
}

std::string glossOf(LensReader& reader, const LensReader::Hit& hit) {
  char gloss[256];
  EXPECT_TRUE(reader.readEntry(hit.entryOffset, nullptr, 0, gloss, sizeof(gloss)));
  return gloss;
}

std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> out;
  if (hex == "-") return out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  return out;
}

}  // namespace

// ── Reader ───────────────────────────────────────────────────────────────────

TEST(LensReader, HeaderDescribesThePack) {
  OpenPack pack("mini-en-ru.lens");
  const LensHeader& h = pack.reader.header();
  EXPECT_STREQ(h.source, "en");
  EXPECT_STREQ(h.target, "ru");
  EXPECT_FALSE(h.monolingual());
  EXPECT_EQ(h.rankScale, LENS_RANK_SCALE_FREQUENCY);
  EXPECT_GT(h.recordCount, h.headwordCount);
  EXPECT_EQ(h.contentHash,
            fnv1a32(pack.source->bytes().data() + LENS_HEADER_SIZE, pack.source->bytes().size() - LENS_HEADER_SIZE));
  char attribution[128];
  pack.reader.readAttribution(attribution, sizeof(attribution));
  EXPECT_STREQ(attribution, "Fixture glosses (test only). License: CC-BY-SA-3.0.");
}

TEST(LensReader, InflectionResolvesToItsLemma) {
  OpenPack pack("mini-en-ru.lens");
  LensReader::Hit hit;
  ASSERT_TRUE(lookupWord(pack.reader, "whispered", hit));
  EXPECT_EQ(hit.rank, 6200);
  EXPECT_EQ(lemmaOf(pack.reader, hit), "whisper");
  EXPECT_EQ(glossOf(pack.reader, hit), "шептать; шёпот");
  LensReader::Hit direct;
  ASSERT_TRUE(lookupWord(pack.reader, "whisper", direct));
  EXPECT_EQ(direct.entryOffset, hit.entryOffset);
}

TEST(LensReader, MissesAreMisses) {
  OpenPack pack("mini-en-ru.lens");
  LensReader::Hit hit;
  EXPECT_FALSE(lookupWord(pack.reader, "unicorn", hit));
  EXPECT_FALSE(lookupWord(pack.reader, "Whispered", hit));  // keys are lowercase; callers fold first
  EXPECT_FALSE(lookupWord(pack.reader, "orphaned", hit));   // its lemma is not a headword
  EXPECT_FALSE(lookupWord(pack.reader, "empty", hit));      // no readable gloss: dropped
  EXPECT_FALSE(lookupWord(pack.reader, "emptied", hit));    // ...and so are its inflections
  EXPECT_FALSE(lookupWord(pack.reader, "nogloss", hit));
  EXPECT_FALSE(lookupWord(pack.reader, "whisp", hit));  // a prefix is not a key
}

TEST(LensReader, EveryRecordOfThePackIsReachable) {
  // Walk the records section and look every key up: covers the first and last record of every
  // bucket, including buckets that share a hash prefix, without depending on hash values.
  OpenPack pack("mini-en-ru.lens");
  const auto& bytes = pack.source->bytes();
  const LensHeader& h = pack.reader.header();
  uint32_t p = h.recordsOffset;
  uint32_t seen = 0;
  while (p < h.poolOffset) {
    const uint8_t n = bytes[p];
    const std::string key(reinterpret_cast<const char*>(bytes.data() + p + 1), n);
    LensReader::Hit hit;
    EXPECT_TRUE(pack.reader.lookup(key.data(), key.size(), hit)) << key;
    EXPECT_EQ(hit.rank, readU16(bytes.data() + p + 1 + n)) << key;
    p += 1 + n + 5;
    seen++;
  }
  EXPECT_EQ(seen, h.recordCount);
}

TEST(LensReader, BucketTableIsMonotonicAndCoversTheRecords) {
  OpenPack pack("mini-en-ru.lens");
  const auto& bytes = pack.source->bytes();
  const LensHeader& h = pack.reader.header();
  uint32_t previous = 0;
  for (uint32_t b = 0; b <= h.bucketCount; b++) {
    const uint32_t off = readU32(bytes.data() + h.tableOffset + b * 4);
    EXPECT_GE(off, previous);
    previous = off;
  }
  EXPECT_EQ(previous, h.poolOffset - h.recordsOffset);
}

TEST(LensReader, ResultCacheAndLruAvoidRepeatedReads) {
  OpenPack pack("mini-en-ru.lens");
  LensReader::Hit hit;
  ASSERT_TRUE(lookupWord(pack.reader, "zebra", hit));
  const uint32_t afterFirst = pack.reader.bucketReads();
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(lookupWord(pack.reader, "zebra", hit));
    EXPECT_FALSE(lookupWord(pack.reader, "unicorn", hit));
  }
  EXPECT_LE(pack.reader.bucketReads(), afterFirst + 1);
}

TEST(LensReader, RejectsCorruptHeaders) {
  auto bytes = readLensFixture("mini-en-ru.lens");
  bytes[0] = 'X';
  MemorySource bad(bytes);
  LensReader reader;
  EXPECT_FALSE(reader.open(&bad));
  auto truncated = readLensFixture("mini-en-ru.lens");
  truncated.resize(truncated.size() - 1);
  MemorySource shortSource(truncated);
  EXPECT_FALSE(reader.open(&shortSource));
}

// ── Gloss cleaning ───────────────────────────────────────────────────────────

TEST(GlossClean, MatchesThePythonReferenceOnRealGlosses) {
  std::ifstream in(fixture("gloss_cases.txt"));
  std::string line;
  int cases = 0;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    int mono = 0;
    std::string rawHex;
    std::string cleanHex;
    if (!(fields >> mono >> rawHex >> cleanHex)) continue;
    const auto raw = fromHex(rawHex);
    const auto clean = fromHex(cleanHex);
    const std::string got = cleanGloss(std::string(raw.begin(), raw.end()), mono != 0);
    EXPECT_EQ(got, std::string(clean.begin(), clean.end())) << "raw: " << std::string(raw.begin(), raw.end());
    cases++;
  }
  EXPECT_GT(cases, 500);
}

TEST(GlossClean, HandMadeCases) {
  EXPECT_EQ(cleanGloss("крова́вый；окрова́вленный", false), "кровавый; окровавленный");
  EXPECT_EQ(cleanGloss("францу́зский；] ]", false), "французский");
  EXPECT_EQ(cleanGloss("] ]；наиху́дшее", false), "наихудшее");
  EXPECT_EQ(cleanGloss("] [[пища；пи́щи]]", false), "пища; пищи");
  EXPECT_EQ(cleanGloss("v. to eat, devour", false), "to eat");
  EXPECT_EQ(cleanGloss("(of a person) bloody", false), "bloody");
  EXPECT_EQ(cleanGloss("阻止, 监禁, 拘留；隔离", false), "阻止; 隔离");
  EXPECT_EQ(cleanGloss("See Thee; definite article", true), "See Thee; definite article");
  EXPECT_EQ(cleanGloss("idle, slothful", true), "idle, slothful");
  EXPECT_EQ(cleanGloss("] ]；[[ ]]", false), "");
}

TEST(GlossClean, DisplayGlossIsTheCappedFirstSense) {
  char out[WordLensGloss::TEXT_BYTES];
  EXPECT_EQ(std::string(out, displayGloss("кровавый; окровавленный", out, sizeof(out))), "кровавый");
  displayGloss("in a manner that is not typical", out, sizeof(out));
  EXPECT_STREQ(out, "in a manner that is not\xE2\x80\xA6");
  displayGloss("abcdefghijklmnopqrstuvwx", out, sizeof(out));  // exactly 24: untouched
  EXPECT_STREQ(out, "abcdefghijklmnopqrstuvwx");
}

// ── Token normalization ──────────────────────────────────────────────────────

TEST(LensText, NormalizesBookTokensToKeys) {
  char key[LOOKUP_KEY_MAX_BYTES + 1];
  TokenInfo info;
  const auto norm = [&](const char* token) {
    const size_t n = normalizeLookupKey(token, std::strlen(token), key, sizeof(key), info);
    return std::string(key, n);
  };
  EXPECT_EQ(norm("\xE2\x80\x9CWhispered,"), "whispered");
  EXPECT_TRUE(info.capitalized);
  EXPECT_FALSE(info.allCaps);
  EXPECT_EQ(norm("don\xE2\x80\x99t"), "don't");
  EXPECT_EQ(norm("НАИВНЫЙ!"), "наивный");
  EXPECT_TRUE(info.allCaps);
  EXPECT_EQ(norm("кро\xCC\x81вь"), "кровь");  // stress mark of a learner edition
  EXPECT_EQ(norm("Naïve"), "naïve");
  EXPECT_EQ(norm("1984"), "");
  EXPECT_EQ(norm("B52s"), "");
  EXPECT_EQ(norm("\xE2\x80\x94"), "");
  EXPECT_EQ(norm("well-known."), "well-known");
}

TEST(LensText, SentenceEnds) {
  EXPECT_TRUE(endsSentence("end.", 4));
  EXPECT_TRUE(endsSentence("said:", 5));
  EXPECT_TRUE(endsSentence("\xE2\x80\xA6", 3));
  EXPECT_TRUE(endsSentence("so?\xE2\x80\x9D", 6));
  EXPECT_FALSE(endsSentence("word,", 5));
}

// ── Levels and the gloss decision ────────────────────────────────────────────

TEST(WordLensLevels, CutoffsAreFrequencyBands) {
  EXPECT_EQ(rankCutoff(static_cast<uint8_t>(Level::A1)), 1000);
  EXPECT_EQ(rankCutoff(static_cast<uint8_t>(Level::B1)), 4000);
  EXPECT_EQ(rankCutoff(static_cast<uint8_t>(Level::C2)), 24000);
  EXPECT_EQ(rankCutoff(static_cast<uint8_t>(Level::C2), LENS_RANK_SCALE_HSK), 24000);
  EXPECT_EQ(rankCutoff(static_cast<uint8_t>(Level::A1), LENS_RANK_SCALE_HSK), 6000);
  EXPECT_FALSE(isDifficult(900, rankCutoff(0)));  // the commonest 1000 are never glossed
}

TEST(WordLensGlosser, GlossesOnlyWordsAtOrAboveTheCutoff) {
  OpenPack pack("mini-en-ru.lens");
  WordLensGlosser glosser;
  WordLensGloss gloss;
  glosser.configure(&pack.reader, static_cast<uint8_t>(Level::C2));
  EXPECT_FALSE(glosser.glossFor("zebra", 5, false, gloss));  // 23999 < 24000
  EXPECT_TRUE(glosser.glossFor("aardvark", 8, false, gloss));
  EXPECT_STREQ(gloss.text, "трубкозуб");
  glosser.configure(&pack.reader, static_cast<uint8_t>(Level::B1));
  EXPECT_TRUE(glosser.glossFor("zebras,", 7, false, gloss));
  EXPECT_STREQ(gloss.text, "зебра");
  EXPECT_FALSE(glosser.glossFor("the", 3, false, gloss));
  EXPECT_FALSE(glosser.glossFor("bloody", 6, false, gloss));  // 1001 < 4000
  glosser.configure(&pack.reader, static_cast<uint8_t>(Level::A1));
  EXPECT_TRUE(glosser.glossFor("bloody", 6, false, gloss));
  EXPECT_STREQ(gloss.text, "кровавый");
}

TEST(WordLensGlosser, CapitalisedMidSentenceWordsAreTakenForNames) {
  OpenPack pack("mini-en-ru.lens");
  WordLensGlosser glosser;
  glosser.configure(&pack.reader, static_cast<uint8_t>(Level::B1));
  WordLensGloss gloss;
  EXPECT_FALSE(glosser.glossFor("Zebra", 5, false, gloss));
  EXPECT_TRUE(glosser.glossFor("Zebra", 5, true, gloss));
  EXPECT_TRUE(glosser.glossFor("ZEBRA", 5, false, gloss));
}

TEST(WordLensGlosser, EnglishDerivationsInheritTheirBaseRank) {
  OpenPack pack("mini-en-en.lens");
  WordLensGlosser glosser;
  glosser.configure(&pack.reader, static_cast<uint8_t>(Level::B1));  // cutoff 4000
  WordLensGloss gloss;
  // "thickly" (12000) is glossed by "in a thick manner", which names its base "thick" (3000).
  EXPECT_FALSE(glosser.glossFor("thickly", 7, false, gloss));
  // "lazily" shares "idle" with "lazy", but lazy (5000) is itself above the cutoff.
  EXPECT_TRUE(glosser.glossFor("lazily", 6, false, gloss));
  // "hardly" does not mean "hard": no shared gloss word, so it keeps its own rank.
  EXPECT_TRUE(glosser.glossFor("hardly", 6, false, gloss));
}

TEST(WordLensGlosser, BaseCandidates) {
  char out[MAX_CANDIDATES][CANDIDATE_BYTES];
  const auto has = [&](size_t n, const char* want) {
    for (size_t i = 0; i < n; i++)
      if (std::strcmp(out[i], want) == 0) return true;
    return false;
  };
  size_t n = englishBaseCandidates("lazily", out, MAX_CANDIDATES);
  EXPECT_TRUE(has(n, "lazy"));
  n = englishBaseCandidates("insufferable", out, MAX_CANDIDATES);
  EXPECT_TRUE(has(n, "suffer"));
  EXPECT_TRUE(has(n, "sufferable"));
  n = englishBaseCandidates("downwards", out, MAX_CANDIDATES);
  EXPECT_TRUE(has(n, "downward"));
  EXPECT_TRUE(has(n, "down"));
  EXPECT_EQ(englishBaseCandidates("naïve", out, MAX_CANDIDATES), 0u);
  EXPECT_TRUE(glossesShareMeaning("in an idle manner", "idle, slothful"));
  EXPECT_FALSE(glossesShareMeaning("barely, scarcely", "difficult"));
  EXPECT_TRUE(glossesShareMeaning("懒惰地", "懒惰的"));
  EXPECT_TRUE(glossMentionsWord("in a thick manner", "thick"));
  EXPECT_FALSE(glossMentionsWord("in a thickset way", "thick"));
}

// ── On-device converter ──────────────────────────────────────────────────────

class LensBuilderTest : public ::testing::TestWithParam<std::tuple<const char*, uint8_t, size_t>> {};

TEST_P(LensBuilderTest, ProducesThePythonBytes) {
  const auto [name, partitionLog2, bufferBytes] = GetParam();
  const std::string dir = tempDir();
  const std::string out = dir + "/out.lens";
  StdioFs fs;
  LensBuildOptions options;
  options.workDir = dir.c_str();
  options.partitionLog2 = partitionLog2;
  options.writeBufferBytes = bufferBytes;
  LensBuildStats stats;
  const std::string json = fixture((std::string(name) + ".json").c_str());
  ASSERT_EQ(buildLensFromJson(fs, json.c_str(), out.c_str(), options, &stats), LensBuildStatus::Ok);
  const auto expected = readLensFixture(std::string(name) + ".lens");
  const auto got = readFile(out);
  ASSERT_EQ(got.size(), expected.size());
  EXPECT_TRUE(got == expected);
  EXPECT_EQ(stats.outputBytes, got.size());
  // Nothing but the output is left behind.
  std::remove(out.c_str());
  const std::string listing = "test -z \"$(ls -A '" + dir + "')\"";
  EXPECT_EQ(std::system(listing.c_str()), 0);
}

INSTANTIATE_TEST_SUITE_P(Partitions, LensBuilderTest,
                         ::testing::Values(std::make_tuple("mini-en-ru", 0, 256), std::make_tuple("mini-en-ru", 3, 16),
                                           std::make_tuple("mini-en-ru", 7, 8), std::make_tuple("mini-en-en", 2, 32)));

TEST(LensBuilder, RejectsMalformedJson) {
  const std::string dir = tempDir();
  const std::string json = dir + "/bad.json";
  {
    std::ofstream(json) << R"({"meta":{"source":"en","target":"ru"},"entries":{"a":{"r":5000,"g":"x")";
  }
  StdioFs fs;
  LensBuildOptions options;
  options.workDir = dir.c_str();
  options.partitionLog2 = 2;
  EXPECT_EQ(buildLensFromJson(fs, json.c_str(), (dir + "/o.lens").c_str(), options), LensBuildStatus::BadJson);
  std::remove(json.c_str());
}

TEST(LensBuilder, Cancels) {
  const std::string dir = tempDir();
  StdioFs fs;
  bool cancel = true;
  LensBuildOptions options;
  options.workDir = dir.c_str();
  options.cancel = &cancel;
  const std::string json = fixture("mini-en-ru.json");
  EXPECT_EQ(buildLensFromJson(fs, json.c_str(), (dir + "/o.lens").c_str(), options), LensBuildStatus::Cancelled);
}

TEST(LensBuilder, StepsReportProgressAndMatchTheWholeBuild) {
  const std::string dir = tempDir();
  const std::string out = dir + "/out.lens";
  StdioFs fs;
  LensBuildOptions options;
  options.workDir = dir.c_str();
  options.partitionLog2 = 3;
  options.writeBufferBytes = 16;
  const std::string json = fixture("mini-en-ru.json");
  LensConverter converter;
  ASSERT_EQ(converter.begin(fs, json.c_str(), out.c_str(), options), LensBuildStatus::Ok);
  int steps = 0;
  uint8_t last = 0;
  while (!converter.done()) {
    ASSERT_EQ(converter.step(), LensBuildStatus::Ok);
    EXPECT_GE(converter.percent(), last);
    last = converter.percent();
    ASSERT_LT(++steps, 10000);
  }
  EXPECT_EQ(converter.percent(), 100);
  EXPECT_GT(steps, 8);  // at least one step per partition and pass
  EXPECT_TRUE(readFile(out) == readLensFixture("mini-en-ru.lens"));
  std::remove(out.c_str());
}

TEST(LensBuilder, AbortMidwayLeavesNothingBehind) {
  for (int stopAfter = 0; stopAfter < 40; stopAfter += 3) {
    const std::string dir = tempDir();
    const std::string out = dir + "/out.lens";
    StdioFs fs;
    LensBuildOptions options;
    options.workDir = dir.c_str();
    options.partitionLog2 = 2;
    const std::string json = fixture("mini-en-ru.json");
    {
      LensConverter converter;
      ASSERT_EQ(converter.begin(fs, json.c_str(), out.c_str(), options), LensBuildStatus::Ok);
      for (int i = 0; i < stopAfter && !converter.done(); i++) ASSERT_EQ(converter.step(), LensBuildStatus::Ok);
      if (converter.done()) break;
      converter.abort();
      EXPECT_EQ(converter.step(), LensBuildStatus::BadPack);  // over; no more work
    }
    const std::string listing = "test -z \"$(ls -A '" + dir + "')\"";
    EXPECT_EQ(std::system(listing.c_str()), 0) << "stopped after " << stopAfter;
  }
}

TEST(LensBuilder, PartitionRecommendation) {
  EXPECT_EQ(recommendedPartitionLog2(100 * 1024), 3);
  EXPECT_EQ(recommendedPartitionLog2(1300 * 1024), 7);
  EXPECT_EQ(recommendedPartitionLog2(800 * 1024), 6);
}

// ── Section cache key ────────────────────────────────────────────────────────

TEST(WordLensCacheKey, ChangesWithThePackAndTheLevel) {
  const uint32_t base = wordLensCacheKey(0x12345678u, 2);
  EXPECT_NE(base, 0u);
  EXPECT_NE(wordLensCacheKey(0x12345679u, 2), base);                             // another pack (or a rebuilt one)
  EXPECT_NE(wordLensCacheKey(0x12345678u, 3), base);                             // another level
  EXPECT_NE(wordLensCacheKey(0x12345678u, 2, /*keepEmptyStrips=*/false), base);  // hidden empty strips
  EXPECT_EQ(wordLensCacheKey(0x12345678u, 2), base);                             // and nothing else moves it
  for (uint8_t a = 0; a < LEVEL_COUNT; a++)
    for (uint8_t b = a + 1; b < LEVEL_COUNT; b++) EXPECT_NE(wordLensCacheKey(7, a), wordLensCacheKey(7, b));
}

TEST(WordLensCacheKey, KeyedOnlyUnderTheWordLensLayout) {
  EXPECT_EQ(lingua::keyedWordLensKey(42, LinguaLayout::WordLens), 42u);
  for (const auto layout : {LinguaLayout::Both, LinguaLayout::OriginalOnly, LinguaLayout::TranslationOnly,
                            LinguaLayout::SideBySide, LinguaLayout::Interlinear}) {
    EXPECT_EQ(lingua::keyedWordLensKey(42, layout), 0u);
  }
  // The strip face is an annotation font: keyed under Word Lens as under Interlinear.
  EXPECT_EQ(lingua::keyedAnnotationFontId(5, LinguaLayout::WordLens), 5);
  EXPECT_EQ(lingua::keyedAnnotationFontId(5, LinguaLayout::OriginalOnly), 0);
}

TEST(WordLensCacheKey, RenderSpecsWithAnotherKeyDoNotShareASection) {
  ReaderRenderSpec a;
  a.linguaLayout = LinguaLayout::WordLens;
  a.wordLensKey = wordLensCacheKey(1, 2);
  ReaderRenderSpec b = a;
  EXPECT_TRUE(a.layoutEquals(b));
  b.wordLensKey = wordLensCacheKey(1, 3);
  EXPECT_FALSE(a.layoutEquals(b));
  // The gloss source is not a key: the key already says which pack and level it answers for.
  b = a;
  b.wordLensSource = reinterpret_cast<WordLensGlossSource*>(0x1);
  EXPECT_TRUE(a.layoutEquals(b));
}
