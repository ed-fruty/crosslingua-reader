#pragma once

// CrossLingua fork: which words of ONE laid-out line are translated text (a word from a translated
// block, or from an inline <span lang> inside an original paragraph). Kept beside the TextBlock
// instead of in its per-word style byte, because upstream uses all eight style bits.
//
// One pointer of state per line:
//   nullptr          no translated word (every non-Lingua line)
//   &ALL_SENTINEL    every word translated (a translated paragraph)
//   owned heap mask  mixed line, bit i = word i (visual order, like the TextBlock arrays)
// Only a line that mixes original and translated words allocates, ceil(wordCount / 8) bytes.
//
// On disk (PageLine's Lingua fields): u8 encoding (0 none, 1 all, 2 mask), then for a mask
// ceil(wordCount / 8) bytes.

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

class LinguaWordFlags {
 public:
  LinguaWordFlags() = default;
  ~LinguaWordFlags() { clear(); }
  LinguaWordFlags(const LinguaWordFlags&) = delete;
  LinguaWordFlags& operator=(const LinguaWordFlags&) = delete;

  bool any() const { return bits != nullptr; }
  bool all() const { return bits == &ALL_SENTINEL; }
  bool test(const uint16_t i) const {
    if (bits == nullptr) return false;
    if (bits == &ALL_SENTINEL) return true;
    return ((bits[i >> 3] >> (i & 7)) & 1U) != 0;
  }

  void clear() {
    if (bits != nullptr && bits != &ALL_SENTINEL) delete[] bits;
    bits = nullptr;
  }
  void setAll() {
    clear();
    bits = &ALL_SENTINEL;
  }
  // Mixed line: replace the flags with an all-clear mask for `wordCount` words, filled through
  // setBit(). On OOM the flags stay clear (the words render as original text) and false returns.
  bool beginMask(const uint16_t wordCount) {
    clear();
    const size_t bytes = maskBytes(wordCount);
    if (bytes == 0) return false;
    bits = new (std::nothrow) uint8_t[bytes];
    if (bits == nullptr) {
      LOG_ERR("LWF", "OOM: translated-word mask (%u bytes)", static_cast<unsigned>(bytes));
      return false;
    }
    memset(bits, 0, bytes);
    return true;
  }
  void setBit(const uint16_t i) {
    if (bits != nullptr && bits != &ALL_SENTINEL) bits[i >> 3] |= static_cast<uint8_t>(1U << (i & 7));
  }

  bool serialize(HalFile& file, const uint16_t wordCount) const {
    const uint8_t encoding = bits == nullptr ? ENCODING_NONE : (bits == &ALL_SENTINEL ? ENCODING_ALL : ENCODING_MASK);
    serialization::writePod(file, encoding);
    if (encoding != ENCODING_MASK) return true;
    const size_t bytes = maskBytes(wordCount);
    return file.write(bits, bytes) == bytes;
  }

  // False only for a corrupt encoding byte. A mask that cannot be allocated is read past and
  // dropped, so the page still loads with those words in the original-text ink.
  bool deserialize(HalFile& file, const uint16_t wordCount) {
    clear();
    uint8_t encoding = ENCODING_NONE;
    serialization::readPod(file, encoding);
    if (encoding == ENCODING_NONE) return true;
    if (encoding == ENCODING_ALL) {
      setAll();
      return true;
    }
    if (encoding != ENCODING_MASK) {
      LOG_ERR("LWF", "Deserialization failed: bad translated-word encoding %u", encoding);
      return false;
    }
    const size_t bytes = maskBytes(wordCount);
    if (beginMask(wordCount)) {
      return static_cast<size_t>(file.read(bits, bytes)) == bytes;
    }
    uint8_t discard[16];
    for (size_t left = bytes; left > 0;) {
      const size_t chunk = left < sizeof(discard) ? left : sizeof(discard);
      if (static_cast<size_t>(file.read(discard, chunk)) != chunk) return false;
      left -= chunk;
    }
    return true;
  }

 private:
  static constexpr uint8_t ENCODING_NONE = 0;
  static constexpr uint8_t ENCODING_ALL = 1;
  static constexpr uint8_t ENCODING_MASK = 2;
  static size_t maskBytes(const uint16_t wordCount) { return (static_cast<size_t>(wordCount) + 7) / 8; }

  // Address-only marker for "every word"; never read or written through.
  inline static uint8_t ALL_SENTINEL = 0;
  uint8_t* bits = nullptr;
};
