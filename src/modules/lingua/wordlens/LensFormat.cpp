#include "modules/lingua/wordlens/LensFormat.h"

namespace lingua::wordlens {

bool parseLensHeader(const uint8_t* bytes, const uint32_t fileSize, LensHeader& out) {
  if (fileSize < LENS_HEADER_SIZE || std::memcmp(bytes, LENS_MAGIC, sizeof(LENS_MAGIC)) != 0) return false;
  out.version = bytes[4];
  out.flags = bytes[5];
  if (out.version != LENS_FORMAT_VERSION || readU16(bytes + 6) != LENS_HEADER_SIZE) return false;
  std::memcpy(out.source, bytes + 8, 8);
  std::memcpy(out.target, bytes + 16, 8);
  out.source[7] = '\0';
  out.target[7] = '\0';
  out.recordCount = readU32(bytes + 24);
  out.headwordCount = readU32(bytes + 28);
  out.bucketCount = readU16(bytes + 32);
  out.bucketLog2 = bytes[34];
  out.rankScale = bytes[35];
  out.tableOffset = readU32(bytes + 36);
  out.recordsOffset = readU32(bytes + 40);
  out.poolOffset = readU32(bytes + 44);
  out.poolSize = readU32(bytes + 48);
  out.attributionOffset = readU32(bytes + 52);
  out.attributionLength = readU16(bytes + 56);
  out.maxBucketBytes = readU16(bytes + 58);
  out.contentHash = readU32(bytes + 60);

  if (out.bucketCount == 0 || out.bucketCount > LENS_MAX_BUCKETS || (out.bucketCount & (out.bucketCount - 1)) != 0 ||
      log2Of(out.bucketCount) != out.bucketLog2) {
    return false;
  }
  const uint32_t tableBytes = (static_cast<uint32_t>(out.bucketCount) + 1) * 4;
  return out.tableOffset == LENS_HEADER_SIZE && out.recordsOffset == out.tableOffset + tableBytes &&
         out.poolOffset >= out.recordsOffset && out.attributionOffset == out.poolOffset + out.poolSize &&
         out.attributionOffset + out.attributionLength == fileSize;
}

void writeLensHeader(const LensHeader& h, uint8_t* bytes) {
  std::memset(bytes, 0, LENS_HEADER_SIZE);
  std::memcpy(bytes, LENS_MAGIC, sizeof(LENS_MAGIC));
  bytes[4] = h.version;
  bytes[5] = h.flags;
  writeU16(bytes + 6, LENS_HEADER_SIZE);
  std::memcpy(bytes + 8, h.source, 8);
  std::memcpy(bytes + 16, h.target, 8);
  writeU32(bytes + 24, h.recordCount);
  writeU32(bytes + 28, h.headwordCount);
  writeU16(bytes + 32, h.bucketCount);
  bytes[34] = h.bucketLog2;
  bytes[35] = h.rankScale;
  writeU32(bytes + 36, h.tableOffset);
  writeU32(bytes + 40, h.recordsOffset);
  writeU32(bytes + 44, h.poolOffset);
  writeU32(bytes + 48, h.poolSize);
  writeU32(bytes + 52, h.attributionOffset);
  writeU16(bytes + 56, h.attributionLength);
  writeU16(bytes + 58, h.maxBucketBytes);
  writeU32(bytes + 60, h.contentHash);
}

}  // namespace lingua::wordlens
