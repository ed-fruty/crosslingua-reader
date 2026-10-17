#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "modules/lingua/wordlens/LensFormat.h"

namespace lingua::wordlens {

// Random-access bytes of an open pack. The device implementation wraps a HalFile; host tests use
// an in-memory buffer.
class LensByteSource {
 public:
  virtual ~LensByteSource() = default;
  virtual bool readAt(uint32_t offset, void* buffer, size_t length) = 0;
  virtual uint32_t size() const = 0;
};

// Lookup over a .lens pack that stays on the SD card.
//
// RAM while open: the bucket table ((bucketCount + 1) * 4 bytes, at most 4100), BUCKET_SLOTS
// buffers of maxBucketBytes (the largest bucket in the pack, typically 0.7 - 2 KB) and a
// direct-mapped result cache of RESULT_CACHE_SIZE entries (12 bytes each). All of it is
// allocated once in open() and freed by close(), so a lookup never allocates.
//
// A lookup hashes the key, reads its bucket (one contiguous SD read, skipped when the bucket is in
// one of the LRU slots) and scans it. The result cache remembers hits and misses by key hash, so
// the commonest words of a chapter cost one bucket read for the whole session.
class LensReader {
 public:
  static constexpr size_t BUCKET_SLOTS = 2;
  static constexpr size_t RESULT_CACHE_SIZE = 256;

  struct Hit {
    uint16_t rank = 0;
    // Offset of the headword's entry in the pool: the lemma (headword) and its gloss.
    uint32_t entryOffset = 0;
  };

  LensReader() = default;
  ~LensReader() { close(); }
  LensReader(const LensReader&) = delete;
  LensReader& operator=(const LensReader&) = delete;

  // Takes no ownership of `source`, which must outlive the reader (or the next close()).
  bool open(LensByteSource* source);
  void close();
  bool isOpen() const { return source_ != nullptr; }
  const LensHeader& header() const { return header_; }

  // `key` is a normalized pack key (see normalizeLookupKey); not necessarily NUL-terminated.
  bool lookup(const char* key, size_t keyLength, Hit& out);

  // Copy the entry's headword and gloss (NUL-terminated, truncated to the buffers). Either buffer
  // may be null to skip it.
  bool readEntry(uint32_t entryOffset, char* lemma, size_t lemmaSize, char* gloss, size_t glossSize);

  // Attribution text of the pack, NUL-terminated into `out`.
  size_t readAttribution(char* out, size_t outSize);

  // Bucket reads that went to the byte source (not served from an LRU slot). For tests and logs.
  uint32_t bucketReads() const { return bucketReads_; }

 private:
  struct Slot {
    int32_t bucket = -1;
    uint16_t length = 0;
    uint32_t lastUse = 0;
  };
  struct CacheEntry {
    uint32_t hash = 0;
    uint32_t entryOffset = 0;
    uint16_t rank = 0;
    uint8_t keyLength = 0;
    uint8_t state = 0;  // 0 empty, 1 hit, 2 miss
  };

  const uint8_t* bucketBytes(uint32_t bucket, uint16_t& length);

  LensByteSource* source_ = nullptr;
  LensHeader header_;
  std::unique_ptr<uint8_t[]> table_;
  std::unique_ptr<uint8_t[]> slotBuffers_;
  Slot slots_[BUCKET_SLOTS];
  std::unique_ptr<CacheEntry[]> cache_;
  uint32_t useClock_ = 0;
  uint32_t bucketReads_ = 0;
};

}  // namespace lingua::wordlens
