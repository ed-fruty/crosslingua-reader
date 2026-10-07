#include "modules/lingua/parsers/TranslationPairWalker.h"

#include <Epub/parsers/ParagraphBoundary.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

#include "modules/lingua/services/TranslatedContentDetector.h"
#include "modules/lingua/utils/TextNormalize.h"

namespace lingua {

namespace {

// Caps one paragraph's original and its translation. 8 KB is roughly 4000 Cyrillic characters --
// past any real paragraph -- and bounds peak heap to a constant regardless of chapter length.
constexpr size_t MAX_RUN_BYTES = 8192;

// Content we must not read text out of at all.
bool isNonTextTag(const char* name) { return strcmp(name, "script") == 0 || strcmp(name, "style") == 0; }

struct WalkCtx {
  const char* bookPrimaryLang = "";
  void* sinkCtx = nullptr;
  TranslationPairSink sink = nullptr;
  XML_Parser parser = nullptr;
  bool stopped = false;  // the sink asked to stop; not an error

  int depth = 0;
  int skipFromDepth = INT_MAX;  // inside <script>/<style>
  bool inBlock = false;
  int blockDepth = 0;
  // Depth of the element that switched accumulation to the TRANSLATION buffer. Cleared by its
  // matching close, so original text after a Calibre </span> is original again -- exactly how the
  // parser pops its inline translated marker.
  int translatedFromDepth = INT_MAX;

  std::string original;
  std::string translation;
  // An original block whose translation has not arrived yet (the sibling shape). Replaced, not
  // emitted, when another original follows: that means the first one had no translation.
  std::string pendingOriginal;
};

// Appends `data` with every inter-token separator collapsed to ONE ASCII space, so a pair folds to
// the same bytes the page's words do when joined. The recognized set mirrors
// textnorm::whitespaceLenAt, the single source of truth for "what separates words": ASCII
// space/tab/CR/LF, U+00A0, U+2000..U+200A, U+202F, U+205F. U+00AD soft hyphen is dropped -- it is
// invisible in the page words too.
void appendNormalized(std::string& out, const char* data, const int len) {
  const auto pushSpace = [&out]() {
    if (!out.empty() && out.back() != ' ') out += ' ';
  };
  for (int i = 0; i < len; i++) {
    if (out.size() >= MAX_RUN_BYTES) return;
    const uint8_t b0 = static_cast<uint8_t>(data[i]);
    if (b0 == ' ' || b0 == '\t' || b0 == '\n' || b0 == '\r') {
      pushSpace();
      continue;
    }
    if (b0 == 0xC2 && i + 1 < len) {
      const uint8_t b1 = static_cast<uint8_t>(data[i + 1]);
      if (b1 == 0xA0) {  // U+00A0 NBSP
        pushSpace();
        i++;
        continue;
      }
      if (b1 == 0xAD) {  // U+00AD soft hyphen -- invisible, drop
        i++;
        continue;
      }
    } else if (b0 == 0xE2 && i + 2 < len) {
      const uint8_t b1 = static_cast<uint8_t>(data[i + 1]);
      const uint8_t b2 = static_cast<uint8_t>(data[i + 2]);
      if ((b1 == 0x80 && b2 >= 0x80 && b2 <= 0x8A) || (b1 == 0x80 && b2 == 0xAF) || (b1 == 0x81 && b2 == 0x9F)) {
        pushSpace();
        i += 2;  // the loop's ++ consumes the third byte
        continue;
      }
    }
    out += data[i];
  }
}

void trimTrailingSpace(std::string& s) {
  while (!s.empty() && s.back() == ' ') s.pop_back();
}

// Finishes the container block that just closed and applies the pairing rule (see the header).
void closeBlock(WalkCtx* ctx) {
  trimTrailingSpace(ctx->original);
  trimTrailingSpace(ctx->translation);

  if (!ctx->translation.empty()) {
    // INLINE shape when this block also carried the original; SIBLING shape when it did not, in
    // which case the original is the one being held from an earlier block.
    const std::string& original = ctx->original.empty() ? ctx->pendingOriginal : ctx->original;
    if (!original.empty() && ctx->sink != nullptr) {
      TranslationPair pair;
      pair.original = original.c_str();
      pair.originalLength = original.size();
      pair.translation = ctx->translation.c_str();
      pair.translationLength = ctx->translation.size();
      if (!ctx->sink(ctx->sinkCtx, pair)) {
        ctx->stopped = true;
        if (ctx->parser != nullptr) XML_StopParser(ctx->parser, XML_FALSE);
      }
    }
    ctx->pendingOriginal.clear();
  } else if (!ctx->original.empty()) {
    // An original still waiting for its translation. Overwriting a previous one is correct: that
    // paragraph simply had none, and the overlays derive untranslated text from the page.
    ctx->pendingOriginal = ctx->original;
  }

  ctx->original.clear();
  ctx->translation.clear();
  ctx->translatedFromDepth = INT_MAX;
}

void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* ctx = static_cast<WalkCtx*>(ud);
  ctx->depth++;
  if (ctx->depth > ctx->skipFromDepth) return;
  if (isNonTextTag(name)) {
    ctx->skipFromDepth = ctx->depth;
    return;
  }

  const char* langAttr = nullptr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0) langAttr = atts[i + 1];
    }
  }
  // The parser's rule, via the parser's predicate: html/body are skipped so a document-level lang
  // cannot mark the whole chapter as translated.
  const bool langAllowed = strcmp(name, "html") != 0 && strcmp(name, "body") != 0;
  const bool translated = langAllowed && content::isTranslatedLangTag(langAttr, ctx->bookPrimaryLang);

  if (paraboundary::isContainerBlockTag(name)) {
    // A container opening while one is already open (mixed content, or a nested block) finishes the
    // outer one first, so its direct text is not merged into the child's.
    if (ctx->inBlock) closeBlock(ctx);
    ctx->inBlock = true;
    ctx->blockDepth = ctx->depth;
    ctx->original.clear();
    ctx->translation.clear();
    ctx->translatedFromDepth = translated ? ctx->depth : INT_MAX;
    return;
  }

  // An INLINE element can also open a translated run -- Calibre puts its translation in a
  // <span lang="..."> after a <br/> inside the original paragraph.
  if (translated && ctx->translatedFromDepth == INT_MAX) ctx->translatedFromDepth = ctx->depth;
}

void XMLCALL onEnd(void* ud, const XML_Char* name) {
  auto* ctx = static_cast<WalkCtx*>(ud);
  (void)name;
  if (ctx->depth == ctx->skipFromDepth) ctx->skipFromDepth = INT_MAX;
  if (ctx->depth <= ctx->skipFromDepth) {
    if (ctx->inBlock && ctx->depth == ctx->blockDepth) {
      closeBlock(ctx);
      ctx->inBlock = false;
    } else if (ctx->depth == ctx->translatedFromDepth) {
      // A translated inline run ended; text after it belongs to the original again.
      ctx->translatedFromDepth = INT_MAX;
    }
  }
  ctx->depth--;
}

void XMLCALL onText(void* ud, const XML_Char* s, const int len) {
  auto* ctx = static_cast<WalkCtx*>(ud);
  if (ctx->depth > ctx->skipFromDepth || ctx->skipFromDepth != INT_MAX) return;
  if (!ctx->inBlock) return;  // text outside any block cannot belong to a paragraph
  const bool intoTranslation = ctx->depth >= ctx->translatedFromDepth;
  appendNormalized(intoTranslation ? ctx->translation : ctx->original, s, len);
}

}  // namespace

bool walkTranslationPairs(const std::string& htmlPath, const std::string& bookPrimaryLang, void* sinkCtx,
                          const TranslationPairSink sink) {
  if (htmlPath.empty() || sink == nullptr) return false;
  // Nothing can be recognized as translated without the book's language, which is the same answer
  // ChapterHtmlSlimParser gives -- so there is no point reading the file at all.
  if (bookPrimaryLang.empty()) return true;

  HalFile file;
  if (!Storage.openFileForRead("TPW", htmlPath, file)) {
    LOG_ERR("TPW", "Cannot open %s", htmlPath.c_str());
    return false;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (parser == nullptr) {
    LOG_ERR("TPW", "Failed to create expat parser");
    return false;
  }

  WalkCtx ctx;
  ctx.bookPrimaryLang = bookPrimaryLang.c_str();
  ctx.sinkCtx = sinkCtx;
  ctx.sink = sink;
  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);

  char buf[1024];
  bool done = false;
  bool failed = false;
  while (!done && !ctx.stopped) {
    int len = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (len < 0) len = 0;  // read error -- treat as end of stream rather than feed expat a bad length
    done = (len < static_cast<int>(sizeof(buf)));
    if (XML_Parse(parser, buf, len, done) == XML_STATUS_ERROR) {
      // XML_StopParser (our early stop) surfaces as ERROR with code ABORTED -- that is a success.
      if (XML_GetErrorCode(parser) != XML_ERROR_ABORTED) {
        LOG_ERR("TPW", "XML parse error at line %lu", XML_GetCurrentLineNumber(parser));
        failed = true;
      }
      break;
    }
  }
  XML_ParserFree(parser);
  return !failed;
}

// ── Page matching ────────────────────────────────────────────────────────────

namespace {

// Needle and haystack both come from textnorm's shared page-to-source matching rule
// (foldForNeedle / foldForMatchNoSpaces): spaces ignored on both sides so an inline-tag word split
// cannot defeat the match, and the needle cut before a line-break hyphenation artifact. See
// TextNormalize.h for why each of those is required.
std::string buildNeedle(const std::string& visibleText) {
  size_t begin = 0;
  if (visibleText.compare(0, 3, "\xe2\x80\xa2") == 0) {  // U+2022 <li> bullet, a real page word
    begin = 3;
    while (begin < visibleText.size() && visibleText[begin] == ' ') begin++;
  }
  return textnorm::foldForNeedle(visibleText.substr(begin), 48);
}

// Every place a paragraph's visible text was found. Position only, plus the texts while they fit in
// the budget below.
struct Candidate {
  int pairIndex = 0;
  size_t offset = 0;
  std::string original;
  std::string translation;
  bool hasTexts = false;
};

// A paragraph keeps at most this many places. Beyond it the extra occurrences cannot change the
// answer in any way a reader could see.
constexpr size_t MAX_CANDIDATES = 4;
// Total bytes of candidate text held for the whole page. The FIRST place a paragraph is found always
// keeps its texts; further places keep them only while the budget lasts, so a long paragraph that
// recurs cannot inflate the working set.
constexpr size_t CANDIDATE_TEXT_BUDGET = 8192;

struct MatchCtx {
  const std::string* visibleTexts = nullptr;
  std::vector<std::string> needles;
  std::vector<std::vector<Candidate>> candidates;
  int count = 0;
  int pairIndex = 0;
  size_t textBytes = 0;
};

// Shared by both match paths (HTML walk and pair cache), so neither can behave differently.
void initMatchCtx(MatchCtx& ctx, const std::string* visibleTexts, const int count) {
  ctx.visibleTexts = visibleTexts;
  ctx.count = count;
  ctx.needles.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) ctx.needles.push_back(buildNeedle(visibleTexts[i]));
  ctx.candidates.resize(static_cast<size_t>(count));
}

// Walk the paragraphs in order, each taking the earliest place still ahead of the previous choice.
// `apply` fills `out`; otherwise this only counts how many could be placed. Taking the earliest
// usable place maximizes that count, and a paragraph with nothing left simply goes unplaced without
// consuming anyone else's place -- the property the old decide-as-you-go matching lacked.
int assignInOrder(const MatchCtx& ctx, PageMatch* out) {
  int lastPair = -1;
  int placed = 0;
  for (int i = 0; i < ctx.count; i++) {
    const auto& mine = ctx.candidates[static_cast<size_t>(i)];
    const Candidate* chosen = nullptr;
    for (const auto& candidate : mine) {
      // Candidates are recorded in ascending order, so the first usable one is the earliest.
      if (candidate.pairIndex > lastPair) {
        chosen = &candidate;
        break;
      }
    }
    if (chosen == nullptr) continue;
    placed++;
    lastPair = chosen->pairIndex;
    if (out == nullptr) continue;
    // Past the text budget this place kept no text. Fall back to the paragraph's first place, which
    // always did: every occurrence of a recurring paragraph carries the same translation, so the
    // reader cannot tell them apart anyway.
    const Candidate& texts = chosen->hasTexts ? *chosen : mine.front();
    if (!texts.hasTexts) continue;
    out[i].matched = true;
    out[i].matchOffset = chosen->offset;
    out[i].original = texts.original;
    out[i].translation = texts.translation;
  }
  return placed;
}

int assignableCount(const MatchCtx& ctx) { return assignInOrder(ctx, nullptr); }

bool onPair(void* ud, const TranslationPair& pair) {
  auto* ctx = static_cast<MatchCtx*>(ud);
  const int pairIndex = ctx->pairIndex++;
  const std::string foldedOriginal = textnorm::foldForMatchNoSpaces(std::string(pair.original, pair.originalLength));

  // Offer this pair to EVERY paragraph, independently. Nothing is decided here: a paragraph simply
  // records that its text occurs at this position. Deciding while walking is what let one paragraph
  // deny another a match -- a recurring heading claimed the first of its many occurrences and pushed
  // the search past the paragraph before it, which then found nothing at all.
  for (int i = 0; i < ctx->count; i++) {
    const std::string& needle = ctx->needles[i];
    if (needle.size() < 3) continue;  // too short to discriminate at all
    auto& mine = ctx->candidates[static_cast<size_t>(i)];
    if (mine.size() >= MAX_CANDIDATES) continue;
    const size_t at = foldedOriginal.find(needle);
    if (at == std::string::npos) continue;

    Candidate candidate;
    candidate.pairIndex = pairIndex;
    candidate.offset = at;
    const size_t bytes = pair.originalLength + pair.translationLength;
    if (mine.empty() || ctx->textBytes + bytes <= CANDIDATE_TEXT_BUDGET) {
      candidate.original.assign(pair.original, pair.originalLength);
      candidate.translation.assign(pair.translation, pair.translationLength);
      candidate.hasTexts = true;
      ctx->textBytes += bytes;
    }
    mine.push_back(std::move(candidate));
  }

  // Stop once the places found so far already give every paragraph one, IN ORDER. "Every paragraph
  // has at least one place" is not enough: identical paragraphs all find their first place in the
  // same pair, and their own occurrences come later.
  return assignableCount(*ctx) < ctx->count;
}

void chooseInOrder(MatchCtx& ctx, PageMatch* out) { (void)assignInOrder(ctx, out); }

}  // namespace

bool matchPageParagraphs(const std::string& htmlPath, const std::string& bookPrimaryLang,
                         const std::string* visibleTexts, const int count, PageMatch* out) {
  if (visibleTexts == nullptr || out == nullptr || count <= 0) return false;

  MatchCtx ctx;
  initMatchCtx(ctx, visibleTexts, count);
  const bool ok = walkTranslationPairs(htmlPath, bookPrimaryLang, &ctx, onPair);
  chooseInOrder(ctx, out);
  return ok;
}

// ── The pair cache ───────────────────────────────────────────────────────────

namespace {

constexpr uint32_t CACHE_MAGIC = 0x52505043;  // 'CPPR'
constexpr uint8_t CACHE_VERSION = 1;
constexpr uint32_t CACHE_HEADER_SIZE = 8;  // magic u32 + version u8 + reserved u8 + reserved u16

// Records are read strictly in order, which is all the matcher ever does, so the cache needs no
// offset table -- just a length-prefixed pair per record.
struct CacheWriteCtx {
  HalFile* file = nullptr;
  bool ok = true;
};

void writeU16(HalFile& file, const uint16_t value) {
  uint8_t bytes[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>(value >> 8)};
  file.write(bytes, sizeof(bytes));
}

bool readU16(HalFile& file, uint16_t& out) {
  uint8_t bytes[2];
  if (file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes))) return false;
  out = static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
  return true;
}

bool onPairToCache(void* ud, const TranslationPair& pair) {
  auto* ctx = static_cast<CacheWriteCtx*>(ud);
  // Both lengths are capped by the walker at MAX_RUN_BYTES, well inside a uint16.
  writeU16(*ctx->file, static_cast<uint16_t>(pair.originalLength));
  writeU16(*ctx->file, static_cast<uint16_t>(pair.translationLength));
  if (ctx->file->write(reinterpret_cast<const uint8_t*>(pair.original), pair.originalLength) != pair.originalLength) {
    ctx->ok = false;
    return false;
  }
  if (ctx->file->write(reinterpret_cast<const uint8_t*>(pair.translation), pair.translationLength) !=
      pair.translationLength) {
    ctx->ok = false;
    return false;
  }
  return true;  // never stop early: a partial cache would read as a complete, shorter chapter
}

}  // namespace

bool writeTranslationPairCache(const std::string& htmlPath, const std::string& bookPrimaryLang,
                               const std::string& cachePath) {
  if (cachePath.empty()) return false;
  const std::string partPath = cachePath + ".part";

  HalFile file;
  if (!Storage.openFileForWrite("TPW", partPath, file)) {
    LOG_ERR("TPW", "Cannot open %s", partPath.c_str());
    return false;
  }

  const uint8_t header[CACHE_HEADER_SIZE] = {static_cast<uint8_t>(CACHE_MAGIC & 0xFF),
                                             static_cast<uint8_t>((CACHE_MAGIC >> 8) & 0xFF),
                                             static_cast<uint8_t>((CACHE_MAGIC >> 16) & 0xFF),
                                             static_cast<uint8_t>((CACHE_MAGIC >> 24) & 0xFF),
                                             CACHE_VERSION,
                                             0,
                                             0,
                                             0};
  file.write(header, sizeof(header));

  CacheWriteCtx ctx;
  ctx.file = &file;
  const bool walked = walkTranslationPairs(htmlPath, bookPrimaryLang, &ctx, onPairToCache);
  file.flush();
  file.close();

  if (!walked || !ctx.ok) {
    Storage.remove(partPath.c_str());
    return false;
  }
  // Atomic commit: until the rename there is no cache, and after it there is a whole one.
  Storage.remove(cachePath.c_str());
  if (!Storage.rename(partPath.c_str(), cachePath.c_str())) {
    LOG_ERR("TPW", "Cannot commit %s", cachePath.c_str());
    Storage.remove(partPath.c_str());
    return false;
  }
  LOG_DBG("TPW", "Wrote pair cache %s", cachePath.c_str());
  return true;
}

namespace {

// Opens a pair cache and validates its header.
bool openCache(const std::string& cachePath, HalFile& file) {
  if (cachePath.empty()) return false;
  if (!Storage.openFileForRead("TPW", cachePath, file)) return false;
  uint8_t header[CACHE_HEADER_SIZE];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  const uint32_t magic = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                         (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
  return magic == CACHE_MAGIC && header[4] == CACHE_VERSION;
}

}  // namespace

bool hasTranslationPairCache(const std::string& cachePath) {
  HalFile file;
  return openCache(cachePath, file);
}

bool matchPageParagraphsCached(const std::string& cachePath, const std::string& htmlPath,
                               const std::string& bookPrimaryLang, const std::string* visibleTexts, const int count,
                               PageMatch* out) {
  if (visibleTexts == nullptr || out == nullptr || count <= 0) return false;

  HalFile cache;
  if (!openCache(cachePath, cache)) {
    // No cache yet (or a foreign version): match straight from the HTML, exactly as if the cache did
    // not exist as a concept. Nothing is ever unavailable for want of it.
    return matchPageParagraphs(htmlPath, bookPrimaryLang, visibleTexts, count, out);
  }

  MatchCtx ctx;
  initMatchCtx(ctx, visibleTexts, count);

  std::string original;
  std::string translation;
  // onPair stops us once every paragraph has been located; the loop also ends at the stream's end.
  for (;;) {
    uint16_t origLen = 0;
    uint16_t transLen = 0;
    if (!readU16(cache, origLen) || !readU16(cache, transLen)) break;  // end of the stream
    original.resize(origLen);
    translation.resize(transLen);
    if (origLen > 0 && cache.read(&original[0], origLen) != static_cast<int>(origLen)) break;
    if (transLen > 0 && cache.read(&translation[0], transLen) != static_cast<int>(transLen)) break;

    TranslationPair pair;
    pair.original = original.c_str();
    pair.originalLength = original.size();
    pair.translation = translation.c_str();
    pair.translationLength = translation.size();
    if (!onPair(&ctx, pair)) break;
  }
  chooseInOrder(ctx, out);
  return true;
}

}  // namespace lingua
