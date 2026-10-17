#include "modules/lingua/wordlens/LensBuilder.h"

#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "modules/lingua/wordlens/GlossClean.h"
#include "modules/lingua/wordlens/LensFormat.h"

namespace lingua::wordlens {
namespace {

// Same text as lens_format.DEFAULT_ATTRIBUTION.
constexpr char DEFAULT_ATTRIBUTION[] =
    "Word Lens data derived from Readest's Word Lens packs (https://github.com/readest/readest). "
    "See ATTRIBUTION.md for sources and licences.";

constexpr uint8_t TAG_HEADWORD = 'H';
constexpr uint8_t TAG_INFLECTION = 'I';
constexpr uint8_t TAG_RESOLVED = 'R';
constexpr size_t READ_CHUNK = 512;
constexpr size_t PATH_BYTES = 128;
// Largest temporary record: tag, two length-prefixed keys.
constexpr size_t MAX_TEMP_RECORD = 1 + 1 + LENS_MAX_KEY_BYTES + 1 + LENS_MAX_KEY_BYTES;

uint32_t partitionOf(const uint32_t hash, const uint8_t log2) { return log2 == 0 ? 0 : hash >> (32 - log2); }

bool cancelled(const LensBuildOptions& o) { return o.cancel && *o.cancel; }

void report(const LensBuildOptions& o, const LensBuildPhase phase, const uint32_t done, const uint32_t total) {
  if (o.progress) o.progress(o.progressCtx, phase, done, total);
}

// 2^log2 temporary files, each fed through a small RAM buffer so the card sees a few large writes
// rather than one tiny write per record.
class PartitionSet {
 public:
  PartitionSet(LensFs& fs, const char* workDir, const char prefix, const uint8_t log2, const size_t bufferBytes)
      : fs_(fs), workDir_(workDir), prefix_(prefix), log2_(log2), bufferBytes_(bufferBytes) {}
  ~PartitionSet() { files_.reset(); }

  uint32_t count() const { return 1u << log2_; }

  void path(const uint32_t index, char* out, const size_t outSize) const {
    std::snprintf(out, outSize, "%s/wl%c%03u.tmp", workDir_, prefix_, static_cast<unsigned>(index));
  }

  bool open() {
    const uint32_t n = count();
    files_ = makeUniqueNoThrow<std::unique_ptr<LensFile>[]>(n);
    buffers_ = makeUniqueNoThrow<uint8_t[]>(bufferBytes_ * n);
    used_ = makeUniqueNoThrow<uint16_t[]>(n);
    if (!files_ || !buffers_ || !used_) {
      LOG_ERR("WLENS", "OOM: %u partition buffers", static_cast<unsigned>(n));
      return false;
    }
    for (uint32_t i = 0; i < n; i++) {
      char p[PATH_BYTES];
      path(i, p, sizeof(p));
      files_[i] = fs_.openWrite(p);
      if (!files_[i]) {
        LOG_ERR("WLENS", "Cannot create %s", p);
        return false;
      }
    }
    return true;
  }

  bool append(const uint32_t index, const uint8_t* data, const size_t length) {
    if (used_[index] + length > bufferBytes_ && !flush(index)) return false;
    if (length > bufferBytes_) {
      if (!files_[index]->write(data, length)) return false;
    } else {
      std::memcpy(buffers_.get() + index * bufferBytes_ + used_[index], data, length);
      used_[index] = static_cast<uint16_t>(used_[index] + length);
    }
    return true;
  }

  // Flush and close every writer; the files stay for reading.
  bool finish() {
    bool ok = true;
    if (files_) {
      for (uint32_t i = 0; i < count(); i++) {
        ok = flush(i) && ok;
        files_[i].reset();
      }
    }
    files_.reset();
    buffers_.reset();
    return ok;
  }

  // Close (without flushing) and delete every partition file.
  void removeAll() {
    files_.reset();
    buffers_.reset();
    for (uint32_t i = 0; i < count(); i++) {
      char p[PATH_BYTES];
      path(i, p, sizeof(p));
      fs_.remove(p);
    }
  }

 private:
  bool flush(const uint32_t index) {
    if (!files_ || !files_[index] || used_[index] == 0) return true;
    const bool ok = files_[index]->write(buffers_.get() + index * bufferBytes_, used_[index]);
    used_[index] = 0;
    return ok;
  }

  LensFs& fs_;
  const char* workDir_;
  char prefix_;
  uint8_t log2_;
  size_t bufferBytes_;
  std::unique_ptr<std::unique_ptr<LensFile>[]> files_;
  std::unique_ptr<uint8_t[]> buffers_;
  std::unique_ptr<uint16_t[]> used_;
};

// Buffered sequential writer for the pool, records and output files. The buffer is on the heap so a
// writer costs the caller's stack a few words, not the buffer.
class BufferedWriter {
 public:
  static constexpr size_t BUFFER_BYTES = 512;
  explicit BufferedWriter(LensFile* file) : file_(file), buffer_(makeUniqueNoThrow<uint8_t[]>(BUFFER_BYTES)) {}
  bool ok() const { return buffer_ != nullptr; }
  bool write(const void* data, size_t length) {
    if (!buffer_) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    written_ += length;
    while (length > 0) {
      const size_t room = BUFFER_BYTES - used_;
      const size_t n = length < room ? length : room;
      std::memcpy(buffer_.get() + used_, p, n);
      used_ += n;
      p += n;
      length -= n;
      if (used_ == BUFFER_BYTES && !flush()) return false;
    }
    return true;
  }
  bool flush() {
    if (used_ == 0) return true;
    const bool ok = buffer_ && file_->write(buffer_.get(), used_);
    used_ = 0;
    return ok;
  }
  uint32_t written() const { return static_cast<uint32_t>(written_); }

 private:
  LensFile* file_;
  std::unique_ptr<uint8_t[]> buffer_;
  size_t used_ = 0;
  size_t written_ = 0;
};

std::string trimmed(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  const auto ws = [](const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (a < b && ws(s[a])) a++;
  while (b > a && ws(s[b - 1])) b--;
  return s.substr(a, b - a);
}

// --- Pass 1: SAX over the Readest JSON ------------------------------------------------------

enum class Section : uint8_t { None, Meta, Entries, Inflections, Other };
enum class EntryField : uint8_t { None, Rank, Gloss, Other };

struct ParseState {
  PartitionSet* partitions = nullptr;
  BufferedWriter* pool = nullptr;
  LensBuildStats* stats = nullptr;
  uint8_t partitionLog2 = 0;
  LensBuildStatus status = LensBuildStatus::Ok;

  int depth = 0;
  Section section = Section::None;
  std::string topKey;
  std::string key;  // the current depth-2 key: a headword, an inflected form or a meta field
  EntryField field = EntryField::None;
  bool haveRank = false;
  long rank = 0;
  std::string gloss;

  bool metaSeen = false;
  std::string source;
  std::string target;
  std::string license;
  std::string attribution;

  uint8_t record[MAX_TEMP_RECORD] = {};

  void fail(const LensBuildStatus s) {
    if (status == LensBuildStatus::Ok) status = s;
  }
};

void emitHeadword(ParseState& st) {
  if (st.key.empty() || st.key.size() > LENS_MAX_KEY_BYTES) return;
  if (!st.metaSeen) {
    LOG_ERR("WLENS", "Pack JSON has entries before meta");
    st.fail(LensBuildStatus::BadPack);
    return;
  }
  const std::string cleaned = cleanGloss(st.gloss, st.source == st.target);
  if (cleaned.empty()) {
    st.stats->droppedEmptyGloss++;
    return;
  }
  long r = st.haveRank ? st.rank : LENS_MAX_RANK;
  r = std::max(0L, std::min(static_cast<long>(LENS_MAX_RANK), r));
  const uint32_t entryOffset = st.pool->written();
  if (entryOffset + 2 + st.key.size() + cleaned.size() >= LENS_MAX_POOL_BYTES) {
    st.fail(LensBuildStatus::BadPack);
    return;
  }
  const auto headLen = static_cast<uint8_t>(st.key.size());
  const auto glossLen = static_cast<uint8_t>(cleaned.size());
  if (!st.pool->write(&headLen, 1) || !st.pool->write(st.key.data(), headLen) || !st.pool->write(&glossLen, 1) ||
      !st.pool->write(cleaned.data(), glossLen)) {
    st.fail(LensBuildStatus::WriteFailed);
    return;
  }
  uint8_t* rec = st.record;
  rec[0] = TAG_HEADWORD;
  rec[1] = headLen;
  std::memcpy(rec + 2, st.key.data(), headLen);
  writeU16(rec + 2 + headLen, static_cast<uint16_t>(r));
  writeU24(rec + 4 + headLen, entryOffset);
  const uint32_t hash = fnv1a32(st.key.data(), st.key.size());
  if (!st.partitions->append(partitionOf(hash, st.partitionLog2), rec, 7u + headLen)) {
    st.fail(LensBuildStatus::WriteFailed);
    return;
  }
  st.stats->headwords++;
}

void emitInflection(ParseState& st, const char* lemma, const size_t lemmaLen) {
  // Counted before any validity check: the bucket estimate counts every pair in the JSON.
  st.stats->inflectionPairs++;
  if (st.key.empty() || st.key.size() > LENS_MAX_KEY_BYTES || lemmaLen == 0 || lemmaLen > LENS_MAX_KEY_BYTES) return;
  uint8_t* rec = st.record;
  size_t n = 0;
  rec[n++] = TAG_INFLECTION;
  rec[n++] = static_cast<uint8_t>(st.key.size());
  std::memcpy(rec + n, st.key.data(), st.key.size());
  n += st.key.size();
  rec[n++] = static_cast<uint8_t>(lemmaLen);
  std::memcpy(rec + n, lemma, lemmaLen);
  n += lemmaLen;
  const uint32_t hash = fnv1a32(lemma, lemmaLen);
  if (!st.partitions->append(partitionOf(hash, st.partitionLog2), rec, n)) st.fail(LensBuildStatus::WriteFailed);
}

void onKey(void* ctx, const char* key, const size_t len) {
  auto& st = *static_cast<ParseState*>(ctx);
  if (st.depth == 1) {
    st.topKey.assign(key, len);
  } else if (st.depth == 2) {
    st.key.assign(key, len);
  } else if (st.depth == 3 && st.section == Section::Entries) {
    st.field = (len == 1 && key[0] == 'r')   ? EntryField::Rank
               : (len == 1 && key[0] == 'g') ? EntryField::Gloss
                                             : EntryField::Other;
  }
}

void onString(void* ctx, const char* value, const size_t len) {
  auto& st = *static_cast<ParseState*>(ctx);
  if (st.depth == 2 && st.section == Section::Meta) {
    if (st.key == "source") st.source.assign(value, len);
    if (st.key == "target") st.target.assign(value, len);
    if (st.key == "license") st.license.assign(value, len);
    if (st.key == "attribution") st.attribution.assign(value, len);
  } else if (st.depth == 2 && st.section == Section::Inflections) {
    emitInflection(st, value, len);
  } else if (st.depth == 3 && st.section == Section::Entries && st.field == EntryField::Gloss) {
    st.gloss.assign(value, len);
  }
}

void onNumber(void* ctx, const char* value, const size_t len) {
  auto& st = *static_cast<ParseState*>(ctx);
  if (st.depth == 3 && st.section == Section::Entries && st.field == EntryField::Rank) {
    char buf[24];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    std::memcpy(buf, value, n);
    buf[n] = '\0';
    st.rank = std::strtol(buf, nullptr, 10);
    st.haveRank = true;
  }
}

void onObjectStart(void* ctx) {
  auto& st = *static_cast<ParseState*>(ctx);
  st.depth++;
  if (st.depth == 2) {
    st.section = st.topKey == "meta"          ? Section::Meta
                 : st.topKey == "entries"     ? Section::Entries
                 : st.topKey == "inflections" ? Section::Inflections
                                              : Section::Other;
  } else if (st.depth == 3 && st.section == Section::Entries) {
    st.haveRank = false;
    st.rank = 0;
    st.gloss.clear();
    st.field = EntryField::None;
  }
}

void onObjectEnd(void* ctx) {
  auto& st = *static_cast<ParseState*>(ctx);
  if (st.depth == 3 && st.section == Section::Entries) emitHeadword(st);
  if (st.depth == 2) {
    if (st.section == Section::Meta) st.metaSeen = true;
    st.section = Section::None;
  }
  st.depth--;
}

void onArrayStart(void* ctx) { static_cast<ParseState*>(ctx)->depth++; }
void onArrayEnd(void* ctx) { static_cast<ParseState*>(ctx)->depth--; }

// --- Passes 2 and 3 ---------------------------------------------------------------------------

// A record inside a loaded partition: its key and the byte offset of the whole record.
struct KeyRef {
  uint32_t hash;
  uint32_t offset;
};

int compareKeys(const uint8_t* a, const uint8_t aLen, const uint8_t* b, const uint8_t bLen) {
  const int c = std::memcmp(a, b, aLen < bLen ? aLen : bLen);
  if (c != 0) return c;
  return static_cast<int>(aLen) - static_cast<int>(bLen);
}

// Size of the temp record starting at p (H, I or R).
size_t recordSize(const uint8_t* p) {
  switch (p[0]) {
    case TAG_HEADWORD:
    case TAG_RESOLVED:
      return 2u + p[1] + 5u;
    case TAG_INFLECTION:
      return 2u + p[1] + 1u + p[2 + p[1]];
    default:
      return 0;
  }
}

bool walkRecords(const uint8_t* data, const uint32_t length, std::vector<KeyRef>& out, const uint8_t wantTag,
                 const bool hashKey) {
  uint32_t p = 0;
  while (p < length) {
    const size_t n = recordSize(data + p);
    if (n == 0 || p + n > length) return false;
    if (data[p] == wantTag) {
      out.push_back({hashKey ? fnv1a32(data + p + 2, data[p + 1]) : 0u, p});
    }
    p += static_cast<uint32_t>(n);
  }
  return true;
}

size_t countRecords(const uint8_t* data, const uint32_t length, const uint8_t wantTag) {
  size_t count = 0;
  uint32_t p = 0;
  while (p < length) {
    const size_t n = recordSize(data + p);
    if (n == 0 || p + n > length) break;
    if (data[p] == wantTag) count++;
    p += static_cast<uint32_t>(n);
  }
  return count;
}

// Sequential reader over a partition file that hands out whole temp records. A record is at most
// MAX_TEMP_RECORD bytes, so a buffer twice that always holds the next one after a refill.
class RecordReader {
 public:
  static constexpr size_t BUFFER_BYTES = 2 * MAX_TEMP_RECORD + 2;
  bool open(LensFs& fs, const char* path) {
    buffer_ = makeUniqueNoThrow<uint8_t[]>(BUFFER_BYTES);
    file_ = fs.openRead(path);
    return buffer_ && file_;
  }
  // Next record, or false at the end. `error` is set on a truncated or unknown record.
  bool next(const uint8_t*& record, size_t& size, bool& error) {
    error = false;
    if (!fill(1)) return false;  // clean end of file
    const uint8_t tag = buffer_[pos_];
    if ((tag != TAG_HEADWORD && tag != TAG_INFLECTION && tag != TAG_RESOLVED) || !fill(2)) {
      error = true;
      return false;
    }
    size_t need = 2u + buffer_[pos_ + 1];
    if (tag == TAG_INFLECTION) {
      if (!fill(need + 1)) {
        error = true;
        return false;
      }
      need += 1u + buffer_[pos_ + need];
    } else {
      need += 5u;
    }
    if (!fill(need)) {
      error = true;
      return false;
    }
    record = buffer_.get() + pos_;
    size = need;
    pos_ += need;
    return true;
  }

 private:
  // Make `n` bytes available at pos_; false at end of file.
  bool fill(const size_t n) {
    if (len_ - pos_ >= n) return true;
    std::memmove(buffer_.get(), buffer_.get() + pos_, len_ - pos_);
    len_ -= pos_;
    pos_ = 0;
    while (len_ < n) {
      const size_t got = file_->read(buffer_.get() + len_, BUFFER_BYTES - len_);
      if (got == 0) return false;
      len_ += got;
    }
    return true;
  }

  std::unique_ptr<LensFile> file_;
  std::unique_ptr<uint8_t[]> buffer_;
  size_t pos_ = 0;
  size_t len_ = 0;
};

// The records of one tag from a partition file, packed into `out` (sized exactly: a counting pass
// first, so the vector never grows by doubling).
LensBuildStatus loadTagged(LensFs& fs, const PartitionSet& set, const uint32_t index, const uint8_t tag,
                           std::vector<uint8_t>& out) {
  out.clear();
  char path[PATH_BYTES];
  set.path(index, path, sizeof(path));
  size_t total = 0;
  for (int pass = 0; pass < 2; pass++) {
    RecordReader reader;
    if (!reader.open(fs, path)) return LensBuildStatus::OutOfMemory;
    const uint8_t* rec = nullptr;
    size_t size = 0;
    bool error = false;
    while (reader.next(rec, size, error)) {
      if (rec[0] != tag) continue;
      if (pass == 0) {
        total += size;
      } else {
        out.insert(out.end(), rec, rec + size);
      }
    }
    if (error) return LensBuildStatus::BadPack;
    if (pass == 0) {
      out.reserve(total);
      if (out.capacity() < total) return LensBuildStatus::OutOfMemory;
    }
  }
  return LensBuildStatus::Ok;
}

}  // namespace

uint8_t recommendedPartitionLog2(const uint32_t jsonBytes) {
  uint8_t log2 = 3;
  while (log2 < 7 && (jsonBytes >> log2) > 16u * 1024u) log2++;
  return log2;
}

const char* lensBuildStatusName(const LensBuildStatus status) {
  switch (status) {
    case LensBuildStatus::Ok:
      return "ok";
    case LensBuildStatus::Cancelled:
      return "cancelled";
    case LensBuildStatus::OpenFailed:
      return "open failed";
    case LensBuildStatus::ReadFailed:
      return "read failed";
    case LensBuildStatus::WriteFailed:
      return "write failed";
    case LensBuildStatus::BadJson:
      return "bad JSON";
    case LensBuildStatus::BadPack:
      return "bad pack";
    case LensBuildStatus::OutOfMemory:
      return "out of memory";
  }
  return "?";
}

namespace {

class HashingWriter {
 public:
  explicit HashingWriter(LensFile* file) : writer_(file) {}
  bool ok() const { return writer_.ok(); }
  bool write(const void* data, const size_t length) {
    hash_ = fnv1a32(static_cast<const uint8_t*>(data), length, hash_);
    return writer_.write(data, length);
  }
  bool flush() { return writer_.flush(); }
  uint32_t hash() const { return hash_; }
  uint32_t written() const { return writer_.written(); }

 private:
  BufferedWriter writer_;
  uint32_t hash_ = 0x811C9DC5u;
};

// JSON bytes fed to the parser, and partitions / copy chunks handled, per step.
constexpr size_t PARSE_CHUNKS_PER_STEP = 8;
constexpr size_t COPY_CHUNKS_PER_STEP = 16;

enum class Stage : uint8_t { Parse, Resolve, Sort, CopyRecords, CopyPool, Finish, Done, Failed };

}  // namespace

struct LensConverter::State {
  LensFs* fs = nullptr;
  LensBuildOptions options;
  std::string workDir;
  std::string jsonPath;
  std::string outPath;
  std::string poolPath;
  std::string recordsPath;
  uint8_t log2 = 0;
  Stage stage = Stage::Failed;
  LensBuildStats stats;

  std::unique_ptr<PartitionSet> byLemma;
  std::unique_ptr<PartitionSet> resolved;
  std::unique_ptr<ParseState> parse;

  // Pass 1.
  std::unique_ptr<LensFile> json;
  uint32_t jsonSize = 0;
  uint32_t jsonDone = 0;
  std::unique_ptr<StreamingJsonParser> parser;
  std::unique_ptr<char[]> chunk;
  std::unique_ptr<LensFile> poolFile;
  std::unique_ptr<BufferedWriter> pool;

  // Passes 2 and 3.
  uint32_t part = 0;
  std::vector<uint8_t> heads;
  std::vector<uint8_t> inflections;
  std::vector<KeyRef> refs;
  std::unique_ptr<uint8_t[]> rec;
  std::unique_ptr<LensFile> recordsFile;
  std::unique_ptr<BufferedWriter> records;
  uint16_t bucketCount = 0;
  uint8_t bucketLog2 = 0;
  std::unique_ptr<uint8_t[]> table;
  uint32_t currentBucket = 0;
  uint32_t bucketStart = 0;

  // Pass 4.
  std::unique_ptr<LensFile> out;
  std::unique_ptr<HashingWriter> body;
  std::unique_ptr<LensFile> copyIn;
  uint32_t recordsBytes = 0;
  std::string attribution;

  void cleanup() {
    json.reset();
    parser.reset();
    poolFile.reset();
    pool.reset();
    recordsFile.reset();
    records.reset();
    copyIn.reset();
    body.reset();
    out.reset();
    if (byLemma) byLemma->removeAll();
    if (resolved) resolved->removeAll();
    if (fs) {
      fs->remove(poolPath.c_str());
      fs->remove(recordsPath.c_str());
    }
  }

  LensBuildStatus fail(const LensBuildStatus status) {
    LOG_ERR("WLENS", "Conversion failed: %s", lensBuildStatusName(status));
    cleanup();
    if (fs) fs->remove(outPath.c_str());
    stage = Stage::Failed;
    return status;
  }

  bool advanceBucketsTo(const uint32_t bucket) {
    while (currentBucket < bucket) {
      const uint32_t size = records->written() - bucketStart;
      if (size > 0xFFFF) return false;
      stats.maxBucketBytes = static_cast<uint16_t>(std::max<uint32_t>(stats.maxBucketBytes, size));
      currentBucket++;
      bucketStart = records->written();
      writeU32(table.get() + currentBucket * 4, bucketStart);
    }
    return true;
  }

  LensBuildStatus stepParse() {
    for (size_t i = 0; i < PARSE_CHUNKS_PER_STEP; i++) {
      const size_t n = json->read(chunk.get(), READ_CHUNK);
      if (n == 0) return endParse();
      parser->feed(chunk.get(), n);
      jsonDone += static_cast<uint32_t>(n);
      if (parser->hasError()) return fail(LensBuildStatus::BadJson);
      if (parse->status != LensBuildStatus::Ok) return fail(parse->status);
    }
    report(options, LensBuildPhase::Parse, jsonDone, jsonSize);
    return LensBuildStatus::Ok;
  }

  LensBuildStatus endParse() {
    if (jsonDone != jsonSize) return fail(LensBuildStatus::ReadFailed);
    if (parse->depth != 0 || !parse->metaSeen) return fail(LensBuildStatus::BadJson);
    if (parse->source.empty() || parse->target.empty() || parse->source.size() > 7 || parse->target.size() > 7) {
      return fail(LensBuildStatus::BadPack);
    }
    json.reset();
    parser.reset();
    chunk.reset();
    if (!pool->flush() || !byLemma->finish()) return fail(LensBuildStatus::WriteFailed);
    pool.reset();
    poolFile.reset();
    if (!resolved->open()) return fail(LensBuildStatus::OutOfMemory);
    rec = makeUniqueNoThrow<uint8_t[]>(2 + LENS_MAX_KEY_BYTES + 5);
    if (!rec) return fail(LensBuildStatus::OutOfMemory);
    part = 0;
    stage = Stage::Resolve;
    return LensBuildStatus::Ok;
  }

  // Pass 2, one partition: resolve every inflection whose lemma hashes here.
  LensBuildStatus stepResolve() {
    if (part >= byLemma->count()) {
      if (!resolved->finish()) return fail(LensBuildStatus::WriteFailed);
      return startSort();
    }
    report(options, LensBuildPhase::Resolve, part, byLemma->count());
    LensBuildStatus status = loadTagged(*fs, *byLemma, part, TAG_HEADWORD, heads);
    if (status != LensBuildStatus::Ok) return fail(status);
    refs.clear();
    refs.reserve(countRecords(heads.data(), static_cast<uint32_t>(heads.size()), TAG_HEADWORD));
    if (!walkRecords(heads.data(), static_cast<uint32_t>(heads.size()), refs, TAG_HEADWORD, false)) {
      return fail(LensBuildStatus::BadPack);
    }
    const uint8_t* base = heads.data();
    std::sort(refs.begin(), refs.end(), [base](const KeyRef& a, const KeyRef& b) {
      return compareKeys(base + a.offset + 2, base[a.offset + 1], base + b.offset + 2, base[b.offset + 1]) < 0;
    });
    char path[PATH_BYTES];
    byLemma->path(part, path, sizeof(path));
    status = LensBuildStatus::Ok;
    {
      // Scoped so the partition is closed before fail() removes it.
      RecordReader reader;
      if (!reader.open(*fs, path)) status = LensBuildStatus::OutOfMemory;
      const uint8_t* r = nullptr;
      size_t size = 0;
      bool error = false;
      while (status == LensBuildStatus::Ok && reader.next(r, size, error)) {
        if (r[0] != TAG_INFLECTION) continue;
        const uint8_t formLen = r[1];
        const uint8_t* form = r + 2;
        const uint8_t lemmaLen = r[2 + formLen];
        const uint8_t* lemma = r + 3 + formLen;
        const auto it = std::lower_bound(refs.begin(), refs.end(), 0, [&](const KeyRef& h, int) {
          return compareKeys(base + h.offset + 2, base[h.offset + 1], lemma, lemmaLen) < 0;
        });
        if (it == refs.end() || compareKeys(base + it->offset + 2, base[it->offset + 1], lemma, lemmaLen) != 0)
          continue;
        const uint8_t* head = base + it->offset;
        rec[0] = TAG_RESOLVED;
        rec[1] = formLen;
        std::memcpy(rec.get() + 2, form, formLen);
        std::memcpy(rec.get() + 2 + formLen, head + 2 + head[1], 5);  // rank + entry offset of the lemma
        if (!resolved->append(partitionOf(fnv1a32(form, formLen), log2), rec.get(), 7u + formLen)) {
          status = LensBuildStatus::WriteFailed;
          break;
        }
        stats.inflectionsFolded++;
      }
      if (status == LensBuildStatus::Ok && error) status = LensBuildStatus::BadPack;
    }
    if (status != LensBuildStatus::Ok) return fail(status);
    part++;
    return LensBuildStatus::Ok;
  }

  LensBuildStatus startSort() {
    bucketCount = bucketCountFor(stats.headwords + stats.inflectionPairs);
    bucketLog2 = log2Of(bucketCount);
    stats.bucketCount = bucketCount;
    table = makeUniqueNoThrow<uint8_t[]>((static_cast<size_t>(bucketCount) + 1) * 4);
    recordsFile = fs->openWrite(recordsPath.c_str());
    if (!table || !recordsFile) return fail(table ? LensBuildStatus::OpenFailed : LensBuildStatus::OutOfMemory);
    records = makeUniqueNoThrow<BufferedWriter>(recordsFile.get());
    if (!records || !records->ok()) return fail(LensBuildStatus::OutOfMemory);
    writeU32(table.get(), 0);
    currentBucket = 0;
    bucketStart = 0;
    part = 0;
    stage = Stage::Sort;
    return LensBuildStatus::Ok;
  }

  // Pass 3, one partition: its headwords and resolved inflections, sorted and deduplicated.
  LensBuildStatus stepSort() {
    if (part >= byLemma->count()) return endSort();
    report(options, LensBuildPhase::Sort, part, byLemma->count());
    LensBuildStatus status = loadTagged(*fs, *byLemma, part, TAG_HEADWORD, heads);
    if (status == LensBuildStatus::Ok) status = loadTagged(*fs, *resolved, part, TAG_RESOLVED, inflections);
    if (status != LensBuildStatus::Ok) return fail(status);
    const auto headLength = static_cast<uint32_t>(heads.size());
    refs.clear();
    refs.reserve(countRecords(heads.data(), headLength, TAG_HEADWORD) +
                 countRecords(inflections.data(), static_cast<uint32_t>(inflections.size()), TAG_RESOLVED));
    if (!walkRecords(heads.data(), headLength, refs, TAG_HEADWORD, true)) return fail(LensBuildStatus::BadPack);
    const size_t headCount = refs.size();
    if (!walkRecords(inflections.data(), static_cast<uint32_t>(inflections.size()), refs, TAG_RESOLVED, true)) {
      return fail(LensBuildStatus::BadPack);
    }
    // One address space for both buffers: offsets past headLength point into `inflections`.
    for (size_t i = headCount; i < refs.size(); i++) refs[i].offset += headLength;
    const uint8_t* h = heads.data();
    const uint8_t* r = inflections.data();
    const auto at = [h, r, headLength](const uint32_t offset) {
      return offset < headLength ? h + offset : r + (offset - headLength);
    };
    std::sort(refs.begin(), refs.end(), [&at](const KeyRef& a, const KeyRef& b) {
      if (a.hash != b.hash) return a.hash < b.hash;
      const uint8_t* pa = at(a.offset);
      const uint8_t* pb = at(b.offset);
      const int c = compareKeys(pa + 2, pa[1], pb + 2, pb[1]);
      if (c != 0) return c < 0;
      return pa[0] == TAG_HEADWORD && pb[0] != TAG_HEADWORD;  // the headword sorts first and wins
    });
    const uint8_t* previous = nullptr;
    for (const KeyRef& ref : refs) {
      const uint8_t* entry = at(ref.offset);
      if (previous && compareKeys(previous + 2, previous[1], entry + 2, entry[1]) == 0) continue;
      previous = entry;
      if (!advanceBucketsTo(bucketOf(ref.hash, bucketLog2))) return fail(LensBuildStatus::BadPack);
      // Final record: keyLen key rank:u16 entryOff:u24 -- the temp record minus its tag.
      if (!records->write(entry + 1, 1u + entry[1] + 5u)) return fail(LensBuildStatus::WriteFailed);
      stats.records++;
    }
    part++;
    return LensBuildStatus::Ok;
  }

  LensBuildStatus endSort() {
    if (!advanceBucketsTo(bucketCount)) return fail(LensBuildStatus::BadPack);
    if (!records->flush()) return fail(LensBuildStatus::WriteFailed);
    records.reset();
    recordsFile.reset();
    std::vector<uint8_t>().swap(heads);
    std::vector<uint8_t>().swap(inflections);
    std::vector<KeyRef>().swap(refs);
    rec.reset();
    byLemma->removeAll();
    resolved->removeAll();

    // Attribution, exactly as lens_format.attribution_for() builds it.
    attribution = trimmed(parse->attribution);
    const std::string license = trimmed(parse->license);
    if (!license.empty()) attribution = (attribution.empty() ? "" : attribution + " ") + "License: " + license + ".";
    if (attribution.empty()) attribution = DEFAULT_ATTRIBUTION;
    if (attribution.size() > 0xFFFF) attribution.resize(0xFFFF);

    out = fs->openWrite(outPath.c_str());
    chunk = makeUniqueNoThrow<char[]>(READ_CHUNK);
    if (!out || !chunk) return fail(chunk ? LensBuildStatus::OpenFailed : LensBuildStatus::OutOfMemory);
    body = makeUniqueNoThrow<HashingWriter>(out.get());
    if (!body || !body->ok()) return fail(LensBuildStatus::OutOfMemory);
    const uint8_t placeholder[LENS_HEADER_SIZE] = {};
    // The header is patched once the content hash is known.
    if (!out->write(placeholder, sizeof(placeholder)) ||
        !body->write(table.get(), (static_cast<size_t>(bucketCount) + 1) * 4)) {
      return fail(LensBuildStatus::WriteFailed);
    }
    copyIn = fs->openRead(recordsPath.c_str());
    if (!copyIn) return fail(LensBuildStatus::OpenFailed);
    stage = Stage::CopyRecords;
    return LensBuildStatus::Ok;
  }

  // Pass 4, a few chunks of the records file, then of the pool file.
  LensBuildStatus stepCopy() {
    report(options, LensBuildPhase::Assemble, stage == Stage::CopyRecords ? 0 : 1, 2);
    for (size_t i = 0; i < COPY_CHUNKS_PER_STEP; i++) {
      const size_t n = copyIn->read(chunk.get(), READ_CHUNK);
      if (n == 0) {
        if (stage == Stage::CopyRecords) {
          recordsBytes = copyIn->size();
          copyIn = fs->openRead(poolPath.c_str());
          if (!copyIn) return fail(LensBuildStatus::OpenFailed);
          stage = Stage::CopyPool;
        } else {
          stage = Stage::Finish;
        }
        return LensBuildStatus::Ok;
      }
      if (!body->write(chunk.get(), n)) return fail(LensBuildStatus::WriteFailed);
    }
    return LensBuildStatus::Ok;
  }

  LensBuildStatus finish() {
    const uint32_t recordsSize = recordsBytes;
    const uint32_t poolSize = copyIn->size();
    copyIn.reset();
    if (!body->write(attribution.data(), attribution.size()) || !body->flush()) {
      return fail(LensBuildStatus::WriteFailed);
    }
    const size_t tableBytes = (static_cast<size_t>(bucketCount) + 1) * 4;
    LensHeader header;
    header.version = LENS_FORMAT_VERSION;
    header.flags = parse->source == parse->target ? LENS_FLAG_MONOLINGUAL : 0;
    std::memcpy(header.source, parse->source.data(), parse->source.size());
    std::memcpy(header.target, parse->target.data(), parse->target.size());
    header.recordCount = stats.records;
    header.headwordCount = stats.headwords;
    header.bucketCount = bucketCount;
    header.bucketLog2 = bucketLog2;
    header.rankScale = parse->source == "zh" ? LENS_RANK_SCALE_HSK : LENS_RANK_SCALE_FREQUENCY;
    header.tableOffset = LENS_HEADER_SIZE;
    header.recordsOffset = static_cast<uint32_t>(LENS_HEADER_SIZE + tableBytes);
    header.poolOffset = header.recordsOffset + recordsSize;
    header.poolSize = poolSize;
    header.attributionOffset = header.poolOffset + poolSize;
    header.attributionLength = static_cast<uint16_t>(attribution.size());
    header.maxBucketBytes = stats.maxBucketBytes;
    header.contentHash = body->hash();
    uint8_t headerBytes[LENS_HEADER_SIZE];
    writeLensHeader(header, headerBytes);
    if (!out->seek(0) || !out->write(headerBytes, sizeof(headerBytes))) return fail(LensBuildStatus::WriteFailed);
    stats.contentHash = header.contentHash;
    stats.outputBytes = LENS_HEADER_SIZE + body->written();
    body.reset();
    out.reset();
    table.reset();
    cleanup();
    LOG_INF("WLENS", "Built %s: %u records (%u headwords), %u buckets", outPath.c_str(),
            static_cast<unsigned>(stats.records), static_cast<unsigned>(stats.headwords),
            static_cast<unsigned>(bucketCount));
    stage = Stage::Done;
    return LensBuildStatus::Ok;
  }
};

LensConverter::LensConverter() = default;

LensConverter::~LensConverter() {
  if (st_ && st_->stage != Stage::Done && st_->stage != Stage::Failed) abort();
}

LensBuildStatus LensConverter::begin(LensFs& fs, const char* jsonPath, const char* outPath,
                                     const LensBuildOptions& options) {
  st_ = makeUniqueNoThrow<State>();
  if (!st_) return LensBuildStatus::OutOfMemory;
  State& s = *st_;
  s.fs = &fs;
  s.options = options;
  s.workDir = options.workDir;
  s.options.workDir = s.workDir.c_str();
  s.jsonPath = jsonPath;
  s.outPath = outPath;
  s.poolPath = s.workDir + "/wlpool.tmp";
  s.recordsPath = s.workDir + "/wlrecs.tmp";
  s.log2 = options.partitionLog2 > 10 ? 10 : options.partitionLog2;
  s.byLemma = makeUniqueNoThrow<PartitionSet>(fs, s.options.workDir, 'p', s.log2, options.writeBufferBytes);
  s.resolved = makeUniqueNoThrow<PartitionSet>(fs, s.options.workDir, 'r', s.log2, options.writeBufferBytes);
  s.parse = makeUniqueNoThrow<ParseState>();
  if (!s.byLemma || !s.resolved || !s.parse) return s.fail(LensBuildStatus::OutOfMemory);

  s.json = fs.openRead(jsonPath);
  if (!s.json) return s.fail(LensBuildStatus::OpenFailed);
  s.jsonSize = s.json->size();
  s.poolFile = fs.openWrite(s.poolPath.c_str());
  if (!s.poolFile) return s.fail(LensBuildStatus::OpenFailed);
  s.pool = makeUniqueNoThrow<BufferedWriter>(s.poolFile.get());
  JsonCallbacks callbacks{};
  callbacks.ctx = s.parse.get();
  callbacks.onKey = onKey;
  callbacks.onString = onString;
  callbacks.onNumber = onNumber;
  callbacks.onObjectStart = onObjectStart;
  callbacks.onObjectEnd = onObjectEnd;
  callbacks.onArrayStart = onArrayStart;
  callbacks.onArrayEnd = onArrayEnd;
  s.parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks);
  s.chunk = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!s.pool || !s.pool->ok() || !s.parser || !s.chunk || !s.byLemma->open()) {
    return s.fail(LensBuildStatus::OutOfMemory);
  }
  s.parse->partitions = s.byLemma.get();
  s.parse->pool = s.pool.get();
  s.parse->stats = &s.stats;
  s.parse->partitionLog2 = s.log2;
  s.stage = Stage::Parse;
  return LensBuildStatus::Ok;
}

LensBuildStatus LensConverter::step() {
  if (!st_) return LensBuildStatus::BadPack;
  State& s = *st_;
  if (cancelled(s.options) && s.stage != Stage::Done && s.stage != Stage::Failed) {
    return s.fail(LensBuildStatus::Cancelled);
  }
  switch (s.stage) {
    case Stage::Parse:
      return s.stepParse();
    case Stage::Resolve:
      return s.stepResolve();
    case Stage::Sort:
      return s.stepSort();
    case Stage::CopyRecords:
    case Stage::CopyPool:
      return s.stepCopy();
    case Stage::Finish:
      return s.finish();
    case Stage::Done:
      return LensBuildStatus::Ok;
    case Stage::Failed:
      return LensBuildStatus::BadPack;
  }
  return LensBuildStatus::BadPack;
}

bool LensConverter::done() const { return st_ && st_->stage == Stage::Done; }

void LensConverter::abort() {
  if (st_ && st_->stage != Stage::Done && st_->stage != Stage::Failed) st_->fail(LensBuildStatus::Cancelled);
}

LensBuildPhase LensConverter::phase() const {
  if (!st_) return LensBuildPhase::Parse;
  switch (st_->stage) {
    case Stage::Parse:
      return LensBuildPhase::Parse;
    case Stage::Resolve:
      return LensBuildPhase::Resolve;
    case Stage::Sort:
      return LensBuildPhase::Sort;
    case Stage::CopyRecords:
    case Stage::CopyPool:
    case Stage::Finish:
    case Stage::Done:
    case Stage::Failed:
      break;
  }
  return LensBuildPhase::Assemble;
}

uint8_t LensConverter::percent() const {
  if (!st_) return 0;
  const State& s = *st_;
  const uint32_t parts = s.byLemma ? s.byLemma->count() : 1;
  switch (s.stage) {
    case Stage::Parse:
      return static_cast<uint8_t>(s.jsonSize > 0 ? 60ull * s.jsonDone / s.jsonSize : 0);
    case Stage::Resolve:
      return static_cast<uint8_t>(60 + 20 * s.part / parts);
    case Stage::Sort:
      return static_cast<uint8_t>(80 + 15 * s.part / parts);
    case Stage::CopyRecords:
      return 96;
    case Stage::CopyPool:
    case Stage::Finish:
      return 98;
    case Stage::Done:
    case Stage::Failed:
      break;
  }
  return 100;
}

const LensBuildStats& LensConverter::stats() const {
  static const LensBuildStats EMPTY;
  return st_ ? st_->stats : EMPTY;
}

LensBuildStatus buildLensFromJson(LensFs& fs, const char* jsonPath, const char* outPath,
                                  const LensBuildOptions& options, LensBuildStats* statsOut) {
  auto converter = makeUniqueNoThrow<LensConverter>();
  if (!converter) return LensBuildStatus::OutOfMemory;
  LensBuildStatus status = converter->begin(fs, jsonPath, outPath, options);
  while (status == LensBuildStatus::Ok && !converter->done()) status = converter->step();
  if (statsOut) *statsOut = converter->stats();
  return status;
}

}  // namespace lingua::wordlens
