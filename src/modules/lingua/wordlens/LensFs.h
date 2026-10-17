#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// The file operations the on-device pack converter needs, behind an interface so the converter is
// host-testable. The device implementation (WordLensStorage.cpp) goes through HalStorage.
namespace lingua::wordlens {

class LensFile {
 public:
  virtual ~LensFile() = default;
  // Bytes read; 0 at end of file or on error.
  virtual size_t read(void* buffer, size_t length) = 0;
  virtual bool write(const void* buffer, size_t length) = 0;
  virtual bool seek(uint32_t position) = 0;
  virtual uint32_t size() = 0;
};

class LensFs {
 public:
  virtual ~LensFs() = default;
  virtual std::unique_ptr<LensFile> openRead(const char* path) = 0;
  // Creates or truncates.
  virtual std::unique_ptr<LensFile> openWrite(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
};

}  // namespace lingua::wordlens
