#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Word Lens pack format (.lens). The reference writer is scripts/lens_format.py of
// https://github.com/ed-fruty/crosslingua-wordlens; the on-device converter (LensBuilder) writes
// the same bytes. See docs/file-formats.md.
//
//   header        64 bytes (LensHeader)
//   bucket table  (bucketCount + 1) x u32, offsets into the records section
//   records       keyLen:u8 key[keyLen] rank:u16 entryOff:u24, globally sorted by
//                 (fnv1a32(key), key bytes); bucket = fnv1a32(key) >> (32 - bucketLog2)
//   entry pool    headLen:u8 head[headLen] glossLen:u8 gloss[glossLen], one per headword; an
//                 inflection record points at its lemma's entry, which therefore names the lemma
//   attribution   UTF-8 text
//
// All integers are little-endian and read with memcpy-free byte assembly: the buffers they come
// from have no alignment guarantee, and the C3 faults on unaligned multi-byte loads.
namespace lingua::wordlens {

inline constexpr char LENS_MAGIC[4] = {'L', 'E', 'N', 'S'};
inline constexpr uint8_t LENS_FORMAT_VERSION = 1;
inline constexpr uint16_t LENS_HEADER_SIZE = 64;
inline constexpr uint16_t LENS_MAX_BUCKETS = 1024;
inline constexpr uint32_t LENS_RECORDS_PER_BUCKET = 48;
inline constexpr size_t LENS_MAX_KEY_BYTES = 255;
inline constexpr uint16_t LENS_MAX_RANK = 0xFFFF;
inline constexpr uint32_t LENS_MAX_POOL_BYTES = 1u << 24;
inline constexpr uint8_t LENS_FLAG_MONOLINGUAL = 0x01;
inline constexpr uint8_t LENS_RANK_SCALE_FREQUENCY = 0;
inline constexpr uint8_t LENS_RANK_SCALE_HSK = 1;
// Record overhead beyond its key: keyLen + rank + entry offset.
inline constexpr size_t LENS_RECORD_OVERHEAD = 1 + 2 + 3;

struct LensHeader {
  uint8_t version = 0;
  uint8_t flags = 0;
  char source[8] = {};
  char target[8] = {};
  uint32_t recordCount = 0;
  uint32_t headwordCount = 0;
  uint16_t bucketCount = 0;
  uint8_t bucketLog2 = 0;
  uint8_t rankScale = 0;
  uint32_t tableOffset = 0;
  uint32_t recordsOffset = 0;
  uint32_t poolOffset = 0;
  uint32_t poolSize = 0;
  uint32_t attributionOffset = 0;
  uint16_t attributionLength = 0;
  uint16_t maxBucketBytes = 0;
  uint32_t contentHash = 0;

  bool monolingual() const { return (flags & LENS_FLAG_MONOLINGUAL) != 0; }
};

constexpr uint32_t fnv1a32(const uint8_t* data, const size_t len, uint32_t h = 0x811C9DC5u) {
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x01000193u;
  }
  return h;
}

inline uint32_t fnv1a32(const char* text, const size_t len) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(text), len);
}

constexpr uint32_t bucketOf(const uint32_t hash, const uint8_t bucketLog2) {
  return bucketLog2 == 0 ? 0 : hash >> (32 - bucketLog2);
}

// Smallest power of two holding `recordEstimate` records at LENS_RECORDS_PER_BUCKET each, capped
// so the bucket table stays a 4 KiB in-RAM index. Shared with lens_format.bucket_count_for.
constexpr uint16_t bucketCountFor(const uint32_t recordEstimate) {
  const uint32_t want = (recordEstimate + LENS_RECORDS_PER_BUCKET - 1) / LENS_RECORDS_PER_BUCKET;
  uint32_t b = 1;
  while (b < want && b < LENS_MAX_BUCKETS) b <<= 1;
  return static_cast<uint16_t>(b);
}

constexpr uint8_t log2Of(uint16_t pow2) {
  uint8_t n = 0;
  while (pow2 > 1) {
    pow2 >>= 1;
    n++;
  }
  return n;
}

inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}
inline uint32_t readU32(const uint8_t* p) { return readU24(p) | (static_cast<uint32_t>(p[3]) << 24); }
inline void writeU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void writeU24(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
}
inline void writeU32(uint8_t* p, const uint32_t v) {
  writeU24(p, v);
  p[3] = static_cast<uint8_t>(v >> 24);
}

// Parse and sanity-check a 64-byte header. `fileSize` bounds every offset.
bool parseLensHeader(const uint8_t* bytes, uint32_t fileSize, LensHeader& out);
// Serialize `h` into 64 bytes (the inverse of parseLensHeader).
void writeLensHeader(const LensHeader& h, uint8_t* bytes);

}  // namespace lingua::wordlens
