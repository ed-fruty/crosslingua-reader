#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "modules/lingua/wordlens/LensFs.h"
#include "modules/lingua/wordlens/LensReader.h"

// Where Word Lens keeps its packs on the SD card, and the HalStorage-backed adapters the format
// code runs on.
namespace lingua::wordlens {

inline constexpr char PACK_DIR[] = "/.crosspoint/wordlens";
// Our manifest as last fetched, so pack status can be shown (and a CDN fallback can be checked)
// without the network.
inline constexpr char MANIFEST_PATH[] = "/.crosspoint/wordlens/word-lens-manifest.json";
// Readest's manifest as last fetched (when our release was unreachable).
inline constexpr char READEST_MANIFEST_PATH[] = "/.crosspoint/wordlens/readest-manifest.json";

// "/.crosspoint/wordlens/word-lens-<source>-<target>.lens"
void packPath(const char* source, const char* target, char* out, size_t outSize);
// "word-lens-<source>-<target>.lens": the release asset name, and the file name on the card.
void packFileName(const char* source, const char* target, char* out, size_t outSize);

// A pack opened for lookups. Seeks are absolute, so the reader's LRU slots are the only buffering.
class HalLensSource final : public LensByteSource {
 public:
  bool open(const char* path);
  void close();
  bool readAt(uint32_t offset, void* buffer, size_t length) override;
  uint32_t size() const override { return size_; }

 private:
  HalFile file_;
  uint32_t size_ = 0;
};

class HalLensFs final : public LensFs {
 public:
  std::unique_ptr<LensFile> openRead(const char* path) override;
  std::unique_ptr<LensFile> openWrite(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
};

}  // namespace lingua::wordlens
