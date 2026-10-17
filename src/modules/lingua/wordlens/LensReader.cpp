#include "modules/lingua/wordlens/LensReader.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace lingua::wordlens {

bool LensReader::open(LensByteSource* source) {
  close();
  if (!source) return false;
  uint8_t raw[LENS_HEADER_SIZE];
  if (!source->readAt(0, raw, sizeof(raw)) || !parseLensHeader(raw, source->size(), header_)) {
    LOG_ERR("WLENS", "Not a valid .lens pack");
    return false;
  }
  const size_t tableBytes = (static_cast<size_t>(header_.bucketCount) + 1) * 4;
  table_ = makeUniqueNoThrow<uint8_t[]>(tableBytes);
  const size_t slotBytes = static_cast<size_t>(header_.maxBucketBytes > 0 ? header_.maxBucketBytes : 1);
  slotBuffers_ = makeUniqueNoThrow<uint8_t[]>(slotBytes * BUCKET_SLOTS);
  cache_ = makeUniqueNoThrow<CacheEntry[]>(RESULT_CACHE_SIZE);
  if (!table_ || !slotBuffers_ || !cache_) {
    LOG_ERR("WLENS", "OOM: lens index (%u + %u bytes)", static_cast<unsigned>(tableBytes),
            static_cast<unsigned>(slotBytes * BUCKET_SLOTS));
    close();
    return false;
  }
  if (!source->readAt(header_.tableOffset, table_.get(), tableBytes)) {
    LOG_ERR("WLENS", "Failed to read bucket table");
    close();
    return false;
  }
  for (auto& slot : slots_) slot = Slot{};
  useClock_ = 0;
  bucketReads_ = 0;
  source_ = source;
  return true;
}

void LensReader::close() {
  source_ = nullptr;
  table_.reset();
  slotBuffers_.reset();
  cache_.reset();
  for (auto& slot : slots_) slot = Slot{};
}

const uint8_t* LensReader::bucketBytes(const uint32_t bucket, uint16_t& length) {
  const uint32_t start = readU32(table_.get() + bucket * 4);
  const uint32_t end = readU32(table_.get() + (bucket + 1) * 4);
  if (end < start || end - start > header_.maxBucketBytes) {
    LOG_ERR("WLENS", "Corrupt bucket %u", static_cast<unsigned>(bucket));
    return nullptr;
  }
  length = static_cast<uint16_t>(end - start);
  if (length == 0) return slotBuffers_.get();

  useClock_++;
  size_t victim = 0;
  for (size_t i = 0; i < BUCKET_SLOTS; i++) {
    if (slots_[i].bucket == static_cast<int32_t>(bucket)) {
      slots_[i].lastUse = useClock_;
      return slotBuffers_.get() + i * header_.maxBucketBytes;
    }
    if (slots_[i].lastUse < slots_[victim].lastUse) victim = i;
  }
  uint8_t* buffer = slotBuffers_.get() + victim * header_.maxBucketBytes;
  bucketReads_++;
  if (!source_->readAt(header_.recordsOffset + start, buffer, length)) {
    slots_[victim] = Slot{};
    return nullptr;
  }
  slots_[victim].bucket = static_cast<int32_t>(bucket);
  slots_[victim].length = length;
  slots_[victim].lastUse = useClock_;
  return buffer;
}

bool LensReader::lookup(const char* key, const size_t keyLength, Hit& out) {
  if (!source_ || keyLength == 0 || keyLength > LENS_MAX_KEY_BYTES) return false;
  const uint32_t hash = fnv1a32(key, keyLength);
  // Hash plus length identify a key for the cache; a 32-bit collision between two words of one
  // book is the accepted (and astronomically rare) failure mode.
  CacheEntry& cached = cache_[hash & (RESULT_CACHE_SIZE - 1)];
  if (cached.state != 0 && cached.hash == hash && cached.keyLength == keyLength) {
    if (cached.state != 1) return false;
    out.rank = cached.rank;
    out.entryOffset = cached.entryOffset;
    return true;
  }

  uint16_t length = 0;
  const uint8_t* bytes = bucketBytes(bucketOf(hash, header_.bucketLog2), length);
  if (!bytes) return false;
  bool found = false;
  size_t p = 0;
  while (p < length) {
    const uint8_t n = bytes[p];
    if (p + 1 + n + 5 > length) break;
    if (n == keyLength && std::memcmp(bytes + p + 1, key, n) == 0) {
      out.rank = readU16(bytes + p + 1 + n);
      out.entryOffset = readU24(bytes + p + 3 + n);
      found = true;
      break;
    }
    p += 1 + n + 5;
  }
  cached.hash = hash;
  cached.keyLength = static_cast<uint8_t>(keyLength);
  cached.state = found ? 1 : 2;
  cached.rank = found ? out.rank : 0;
  cached.entryOffset = found ? out.entryOffset : 0;
  return found;
}

bool LensReader::readEntry(const uint32_t entryOffset, char* lemma, const size_t lemmaSize, char* gloss,
                           const size_t glossSize) {
  if (!source_ || entryOffset >= header_.poolSize) return false;
  uint32_t at = header_.poolOffset + entryOffset;
  uint8_t len = 0;
  if (!source_->readAt(at, &len, 1)) return false;
  at += 1;
  if (lemma && lemmaSize > 0) {
    const size_t copy = len < lemmaSize - 1 ? len : lemmaSize - 1;
    if (!source_->readAt(at, lemma, copy)) return false;
    lemma[copy] = '\0';
  }
  at += len;
  if (gloss && glossSize > 0) {
    uint8_t glossLen = 0;
    if (!source_->readAt(at, &glossLen, 1)) return false;
    const size_t copy = glossLen < glossSize - 1 ? glossLen : glossSize - 1;
    if (!source_->readAt(at + 1, gloss, copy)) return false;
    gloss[copy] = '\0';
  }
  return true;
}

size_t LensReader::readAttribution(char* out, const size_t outSize) {
  if (!source_ || outSize == 0) return 0;
  const size_t copy = header_.attributionLength < outSize - 1 ? header_.attributionLength : outSize - 1;
  if (!source_->readAt(header_.attributionOffset, out, copy)) {
    out[0] = '\0';
    return 0;
  }
  out[copy] = '\0';
  return copy;
}

}  // namespace lingua::wordlens
