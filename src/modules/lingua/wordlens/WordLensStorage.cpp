#include "modules/lingua/wordlens/WordLensStorage.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdio>

namespace lingua::wordlens {
namespace {

class HalLensFile final : public LensFile {
 public:
  HalFile file;
  size_t read(void* buffer, const size_t length) override {
    const int n = file.read(buffer, length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  bool write(const void* buffer, const size_t length) override { return file.write(buffer, length) == length; }
  bool seek(const uint32_t position) override { return file.seek(position); }
  uint32_t size() override { return static_cast<uint32_t>(file.size()); }
};

}  // namespace

void packFileName(const char* source, const char* target, char* out, const size_t outSize) {
  std::snprintf(out, outSize, "word-lens-%s-%s.lens", source, target);
}

void packPath(const char* source, const char* target, char* out, const size_t outSize) {
  std::snprintf(out, outSize, "%s/word-lens-%s-%s.lens", PACK_DIR, source, target);
}

bool HalLensSource::open(const char* path) {
  close();
  if (!Storage.exists(path) || !Storage.openFileForRead("WLENS", path, file_)) return false;
  size_ = static_cast<uint32_t>(file_.fileSize());
  return true;
}

void HalLensSource::close() {
  // Explicit close: a member handle outlives every call that uses it.
  if (file_.isOpen()) file_.close();
  size_ = 0;
}

bool HalLensSource::readAt(const uint32_t offset, void* buffer, const size_t length) {
  if (!file_.isOpen() || offset + length > size_) return false;
  if (!file_.seek(offset)) return false;
  return file_.read(buffer, length) == static_cast<int>(length);
}

std::unique_ptr<LensFile> HalLensFs::openRead(const char* path) {
  auto f = makeUniqueNoThrow<HalLensFile>();
  if (!f || !Storage.openFileForRead("WLENS", path, f->file)) return nullptr;
  return f;
}

std::unique_ptr<LensFile> HalLensFs::openWrite(const char* path) {
  // Opened read-write and truncated, so the converter can seek back and patch the header.
  auto f = makeUniqueNoThrow<HalLensFile>();
  if (!f || !Storage.openFileForWrite("WLENS", path, f->file)) return nullptr;
  return f;
}

bool HalLensFs::remove(const char* path) { return Storage.remove(path); }

bool HalLensFs::rename(const char* from, const char* to) { return Storage.rename(from, to); }

}  // namespace lingua::wordlens
