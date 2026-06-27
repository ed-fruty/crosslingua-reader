#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// Upper bound for a single serialized string. Any length beyond this is treated as
// corruption rather than honored — resize()-ing to a bogus length throws
// std::bad_alloc/length_error, and because the throw unwinds through C (FatFs) frames
// it aborts the whole device into a boot loop instead of failing gracefully.
constexpr uint32_t kMaxSerializedStringLen = 64u * 1024u;

static void readString(std::istream& is, std::string& s) {
  uint32_t len = 0;
  readPod(is, len);
  if (!is || len > kMaxSerializedStringLen) {
    s.clear();
    return;
  }
  s.resize(len);
  if (len > 0) {
    is.read(&s[0], len);
  }
}

static void readString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != static_cast<int>(sizeof(len))) {
    s.clear();
    return;
  }
  // A length longer than what remains in the file (or absurdly large) is garbage from a
  // truncated/interrupted write. Refuse it instead of throwing inside resize().
  const int remaining = file.available();
  if (remaining < 0 || len > static_cast<uint32_t>(remaining) || len > kMaxSerializedStringLen) {
    s.clear();
    return;
  }
  s.resize(len);
  if (len > 0) {
    file.read(reinterpret_cast<uint8_t*>(&s[0]), len);
  }
}
}  // namespace serialization
