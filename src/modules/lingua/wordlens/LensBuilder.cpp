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
    sizes_ = makeUniqueNoThrow<uint32_t[]>(n);
    if (!files_ || !buffers_ || !used_ || !sizes_) {
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
    sizes_[index] += static_cast<uint32_t>(length);
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

  uint32_t size(const uint32_t index) const { return sizes_ ? sizes_[index] : 0; }

  // Read partition `index` whole into a fresh buffer.
  std::unique_ptr<uint8_t[]> load(const uint32_t index, uint32_t& length, LensBuildStatus& status) const {
    length = size(index);
    auto buffer = makeUniqueNoThrow<uint8_t[]>(length > 0 ? length : 1);
    if (!buffer) {
      LOG_ERR("WLENS", "OOM: partition of %u bytes", static_cast<unsigned>(length));
      status = LensBuildStatus::OutOfMemory;
      return nullptr;
    }
    if (length == 0) return buffer;
    char p[PATH_BYTES];
    path(index, p, sizeof(p));
    auto file = fs_.openRead(p);
    if (!file || file->read(buffer.get(), length) != length) {
      status = LensBuildStatus::ReadFailed;
      return nullptr;
    }
    return buffer;
  }

  void removeAll() {
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
  std::unique_ptr<uint32_t[]> sizes_;
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

LensBuildStatus parsePass(LensFs& fs, const char* jsonPath, ParseState& st, const LensBuildOptions& options) {
  auto json = fs.openRead(jsonPath);
  if (!json) return LensBuildStatus::OpenFailed;
  const uint32_t total = json->size();
  JsonCallbacks callbacks{};
  callbacks.ctx = &st;
  callbacks.onKey = onKey;
  callbacks.onString = onString;
  callbacks.onNumber = onNumber;
  callbacks.onObjectStart = onObjectStart;
  callbacks.onObjectEnd = onObjectEnd;
  callbacks.onArrayStart = onArrayStart;
  callbacks.onArrayEnd = onArrayEnd;
  auto parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks);
  auto chunk = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!parser || !chunk) return LensBuildStatus::OutOfMemory;
  uint32_t done = 0;
  uint32_t nextReport = 0;
  for (;;) {
    const size_t n = json->read(chunk.get(), READ_CHUNK);
    if (n == 0) break;
    parser->feed(chunk.get(), n);
    done += static_cast<uint32_t>(n);
    if (parser->hasError()) return LensBuildStatus::BadJson;
    if (st.status != LensBuildStatus::Ok) return st.status;
    if (cancelled(options)) return LensBuildStatus::Cancelled;
    if (done >= nextReport) {
      report(options, LensBuildPhase::Parse, done, total);
      nextReport = done + 32u * 1024u;
    }
  }
  if (done != total) return LensBuildStatus::ReadFailed;
  if (st.depth != 0 || !st.metaSeen) return LensBuildStatus::BadJson;
  if (st.source.empty() || st.target.empty() || st.source.size() > 7 || st.target.size() > 7) {
    return LensBuildStatus::BadPack;
  }
  return LensBuildStatus::Ok;
}

// Pass 2: per partition, resolve every inflection's lemma against the partition's headwords and
// re-file the result under the inflected form's own hash.
LensBuildStatus resolvePass(PartitionSet& byLemma, PartitionSet& resolved, const uint8_t log2, LensBuildStats& stats,
                            const LensBuildOptions& options) {
  std::vector<KeyRef> heads;
  auto rec = makeUniqueNoThrow<uint8_t[]>(2 + LENS_MAX_KEY_BYTES + 5);
  if (!rec) return LensBuildStatus::OutOfMemory;
  for (uint32_t part = 0; part < byLemma.count(); part++) {
    if (cancelled(options)) return LensBuildStatus::Cancelled;
    report(options, LensBuildPhase::Resolve, part, byLemma.count());
    LensBuildStatus status = LensBuildStatus::Ok;
    uint32_t length = 0;
    auto data = byLemma.load(part, length, status);
    if (!data) return status;
    heads.clear();
    heads.reserve(countRecords(data.get(), length, TAG_HEADWORD));
    if (!walkRecords(data.get(), length, heads, TAG_HEADWORD, false)) return LensBuildStatus::BadPack;
    const uint8_t* base = data.get();
    std::sort(heads.begin(), heads.end(), [base](const KeyRef& a, const KeyRef& b) {
      return compareKeys(base + a.offset + 2, base[a.offset + 1], base + b.offset + 2, base[b.offset + 1]) < 0;
    });

    uint32_t p = 0;
    while (p < length) {
      const size_t n = recordSize(base + p);
      if (base[p] == TAG_INFLECTION) {
        const uint8_t formLen = base[p + 1];
        const uint8_t* form = base + p + 2;
        const uint8_t lemmaLen = base[p + 2 + formLen];
        const uint8_t* lemma = base + p + 3 + formLen;
        const auto it = std::lower_bound(heads.begin(), heads.end(), 0, [&](const KeyRef& h, int) {
          return compareKeys(base + h.offset + 2, base[h.offset + 1], lemma, lemmaLen) < 0;
        });
        if (it != heads.end() && compareKeys(base + it->offset + 2, base[it->offset + 1], lemma, lemmaLen) == 0) {
          const uint8_t* head = base + it->offset;
          rec[0] = TAG_RESOLVED;
          rec[1] = formLen;
          std::memcpy(rec.get() + 2, form, formLen);
          std::memcpy(rec.get() + 2 + formLen, head + 2 + head[1], 5);  // rank + entry offset of the lemma
          const uint32_t hash = fnv1a32(form, formLen);
          if (!resolved.append(partitionOf(hash, log2), rec.get(), 7u + formLen)) return LensBuildStatus::WriteFailed;
          stats.inflectionsFolded++;
        }
      }
      p += static_cast<uint32_t>(n);
    }
  }
  return LensBuildStatus::Ok;
}

// Pass 3: per partition, sort headwords and resolved inflections by (hash, key), let a headword
// shadow an inflection with the same key, and append the final records.
LensBuildStatus sortPass(PartitionSet& byKey, PartitionSet& resolved, BufferedWriter& records, uint8_t* table,
                         const uint16_t bucketCount, const uint8_t bucketLog2, LensBuildStats& stats,
                         const LensBuildOptions& options) {
  std::vector<KeyRef> refs;
  uint32_t currentBucket = 0;
  uint32_t bucketStart = 0;
  writeU32(table, 0);
  for (uint32_t part = 0; part < byKey.count(); part++) {
    if (cancelled(options)) return LensBuildStatus::Cancelled;
    report(options, LensBuildPhase::Sort, part, byKey.count());
    LensBuildStatus status = LensBuildStatus::Ok;
    uint32_t headLength = 0;
    auto heads = byKey.load(part, headLength, status);
    if (!heads) return status;
    uint32_t resolvedLength = 0;
    auto inflections = resolved.load(part, resolvedLength, status);
    if (!inflections) return status;

    // One address space for both buffers: offsets below headLength point into `heads`, the rest
    // (minus headLength) into `inflections`.
    refs.clear();
    refs.reserve(countRecords(heads.get(), headLength, TAG_HEADWORD) +
                 countRecords(inflections.get(), resolvedLength, TAG_RESOLVED));
    if (!walkRecords(heads.get(), headLength, refs, TAG_HEADWORD, true)) return LensBuildStatus::BadPack;
    const size_t headCount = refs.size();
    if (!walkRecords(inflections.get(), resolvedLength, refs, TAG_RESOLVED, true)) return LensBuildStatus::BadPack;
    for (size_t i = headCount; i < refs.size(); i++) refs[i].offset += headLength;

    const uint8_t* h = heads.get();
    const uint8_t* r = inflections.get();
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
      const uint8_t* rec = at(ref.offset);
      if (previous && compareKeys(previous + 2, previous[1], rec + 2, rec[1]) == 0) continue;
      previous = rec;
      const uint32_t bucket = bucketOf(ref.hash, bucketLog2);
      while (currentBucket < bucket) {
        const uint32_t size = records.written() - bucketStart;
        stats.maxBucketBytes = static_cast<uint16_t>(std::max<uint32_t>(stats.maxBucketBytes, size));
        if (size > 0xFFFF) return LensBuildStatus::BadPack;
        currentBucket++;
        bucketStart = records.written();
        writeU32(table + currentBucket * 4, bucketStart);
      }
      // Final record: keyLen key rank:u16 entryOff:u24 -- the temp record minus its tag.
      if (!records.write(rec + 1, 1u + rec[1] + 5u)) return LensBuildStatus::WriteFailed;
      stats.records++;
    }
  }
  while (currentBucket < bucketCount) {
    const uint32_t size = records.written() - bucketStart;
    stats.maxBucketBytes = static_cast<uint16_t>(std::max<uint32_t>(stats.maxBucketBytes, size));
    if (size > 0xFFFF) return LensBuildStatus::BadPack;
    currentBucket++;
    bucketStart = records.written();
    writeU32(table + currentBucket * 4, bucketStart);
  }
  return LensBuildStatus::Ok;
}

class HashingWriter {
 public:
  explicit HashingWriter(LensFile* file) : writer_(file) {}
  bool write(const void* data, const size_t length) {
    hash_ = fnv1a32(static_cast<const uint8_t*>(data), length, hash_);
    return writer_.write(data, length);
  }
  bool copyFrom(LensFile& source, uint8_t* buffer, const size_t bufferSize) {
    if (!source.seek(0)) return false;
    for (;;) {
      const size_t n = source.read(buffer, bufferSize);
      if (n == 0) return true;
      if (!write(buffer, n)) return false;
    }
  }
  bool flush() { return writer_.flush(); }
  bool ok() const { return writer_.ok(); }
  uint32_t hash() const { return hash_; }
  uint32_t written() const { return writer_.written(); }

 private:
  BufferedWriter writer_;
  uint32_t hash_ = 0x811C9DC5u;
};

}  // namespace

LensBuildStatus buildLensFromJson(LensFs& fs, const char* jsonPath, const char* outPath,
                                  const LensBuildOptions& options, LensBuildStats* statsOut) {
  LensBuildStats localStats;
  LensBuildStats& stats = statsOut ? *statsOut : localStats;
  stats = LensBuildStats{};
  const uint8_t log2 = options.partitionLog2 > 10 ? 10 : options.partitionLog2;
  const std::string poolPathText = std::string(options.workDir) + "/wlpool.tmp";
  const std::string recordsPathText = std::string(options.workDir) + "/wlrecs.tmp";
  const char* poolPath = poolPathText.c_str();
  const char* recordsPath = recordsPathText.c_str();

  PartitionSet byLemma(fs, options.workDir, 'p', log2, options.writeBufferBytes);
  PartitionSet resolved(fs, options.workDir, 'r', log2, options.writeBufferBytes);
  const auto cleanup = [&]() {
    byLemma.removeAll();
    resolved.removeAll();
    fs.remove(poolPath);
    fs.remove(recordsPath);
  };

  auto st = makeUniqueNoThrow<ParseState>();
  if (!st) return LensBuildStatus::OutOfMemory;
  LensBuildStatus status = LensBuildStatus::Ok;
  {
    auto poolFile = fs.openWrite(poolPath);
    if (!poolFile || !byLemma.open()) {
      cleanup();
      return poolFile ? LensBuildStatus::OutOfMemory : LensBuildStatus::OpenFailed;
    }
    BufferedWriter pool(poolFile.get());
    if (!pool.ok()) {
      cleanup();
      return LensBuildStatus::OutOfMemory;
    }
    st->partitions = &byLemma;
    st->pool = &pool;
    st->stats = &stats;
    st->partitionLog2 = log2;
    status = parsePass(fs, jsonPath, *st, options);
    const bool flushed = pool.flush() && byLemma.finish();
    if (status == LensBuildStatus::Ok && !flushed) status = LensBuildStatus::WriteFailed;
  }
  if (status != LensBuildStatus::Ok) {
    LOG_ERR("WLENS", "Parse pass failed: %s", lensBuildStatusName(status));
    cleanup();
    return status;
  }

  if (!resolved.open()) {
    cleanup();
    return LensBuildStatus::OutOfMemory;
  }
  status = resolvePass(byLemma, resolved, log2, stats, options);
  if (status == LensBuildStatus::Ok && !resolved.finish()) status = LensBuildStatus::WriteFailed;
  if (status != LensBuildStatus::Ok) {
    LOG_ERR("WLENS", "Resolve pass failed: %s", lensBuildStatusName(status));
    cleanup();
    return status;
  }

  const uint16_t bucketCount = bucketCountFor(stats.headwords + stats.inflectionPairs);
  const uint8_t bucketLog2 = log2Of(bucketCount);
  stats.bucketCount = bucketCount;
  const size_t tableBytes = (static_cast<size_t>(bucketCount) + 1) * 4;
  auto table = makeUniqueNoThrow<uint8_t[]>(tableBytes);
  if (!table) {
    cleanup();
    return LensBuildStatus::OutOfMemory;
  }
  {
    auto recordsFile = fs.openWrite(recordsPath);
    if (!recordsFile) {
      cleanup();
      return LensBuildStatus::OpenFailed;
    }
    BufferedWriter records(recordsFile.get());
    if (!records.ok()) {
      cleanup();
      return LensBuildStatus::OutOfMemory;
    }
    status = sortPass(byLemma, resolved, records, table.get(), bucketCount, bucketLog2, stats, options);
    if (status == LensBuildStatus::Ok && !records.flush()) status = LensBuildStatus::WriteFailed;
  }
  byLemma.removeAll();
  resolved.removeAll();
  if (status != LensBuildStatus::Ok) {
    LOG_ERR("WLENS", "Sort pass failed: %s", lensBuildStatusName(status));
    cleanup();
    return status;
  }

  // Attribution, exactly as lens_format.attribution_for() builds it.
  std::string attribution = trimmed(st->attribution);
  const std::string license = trimmed(st->license);
  if (!license.empty()) attribution = (attribution.empty() ? "" : attribution + " ") + "License: " + license + ".";
  if (attribution.empty()) attribution = DEFAULT_ATTRIBUTION;
  if (attribution.size() > 0xFFFF) attribution.resize(0xFFFF);

  report(options, LensBuildPhase::Assemble, 0, 1);
  auto out = fs.openWrite(outPath);
  auto recordsIn = fs.openRead(recordsPath);
  auto poolIn = fs.openRead(poolPath);
  auto copyBuffer = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK);
  if (!out || !recordsIn || !poolIn || !copyBuffer) {
    cleanup();
    return copyBuffer ? LensBuildStatus::OpenFailed : LensBuildStatus::OutOfMemory;
  }
  const uint32_t recordsSize = recordsIn->size();
  const uint32_t poolSize = poolIn->size();

  LensHeader header;
  header.version = LENS_FORMAT_VERSION;
  header.flags = st->source == st->target ? LENS_FLAG_MONOLINGUAL : 0;
  std::memcpy(header.source, st->source.data(), st->source.size());
  std::memcpy(header.target, st->target.data(), st->target.size());
  header.recordCount = stats.records;
  header.headwordCount = stats.headwords;
  header.bucketCount = bucketCount;
  header.bucketLog2 = bucketLog2;
  header.rankScale = st->source == "zh" ? LENS_RANK_SCALE_HSK : LENS_RANK_SCALE_FREQUENCY;
  header.tableOffset = LENS_HEADER_SIZE;
  header.recordsOffset = static_cast<uint32_t>(LENS_HEADER_SIZE + tableBytes);
  header.poolOffset = header.recordsOffset + recordsSize;
  header.poolSize = poolSize;
  header.attributionOffset = header.poolOffset + poolSize;
  header.attributionLength = static_cast<uint16_t>(attribution.size());
  header.maxBucketBytes = stats.maxBucketBytes;

  uint8_t headerBytes[LENS_HEADER_SIZE] = {};
  bool ok = out->write(headerBytes, sizeof(headerBytes));  // patched once the content hash is known
  HashingWriter body(out.get());
  ok = ok && body.ok() && body.write(table.get(), tableBytes) &&
       body.copyFrom(*recordsIn, copyBuffer.get(), READ_CHUNK) &&
       body.copyFrom(*poolIn, copyBuffer.get(), READ_CHUNK) && body.write(attribution.data(), attribution.size()) &&
       body.flush();
  header.contentHash = body.hash();
  writeLensHeader(header, headerBytes);
  ok = ok && out->seek(0) && out->write(headerBytes, sizeof(headerBytes));
  stats.contentHash = header.contentHash;
  stats.outputBytes = LENS_HEADER_SIZE + body.written();
  recordsIn.reset();
  poolIn.reset();
  out.reset();
  cleanup();
  if (!ok) {
    fs.remove(outPath);
    return LensBuildStatus::WriteFailed;
  }
  LOG_INF("WLENS", "Built %s: %u records (%u headwords), %u buckets", outPath, static_cast<unsigned>(stats.records),
          static_cast<unsigned>(stats.headwords), static_cast<unsigned>(bucketCount));
  return LensBuildStatus::Ok;
}

}  // namespace lingua::wordlens
