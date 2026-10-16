#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

class CssParser;

// Host stub for the Lingua Section tests: one spine item, no TOC, no CSS, files under cachePath.
class Epub {
 public:
  struct SpineEntry {
    std::string href;
  };
  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    int16_t spineIndex = -1;
  };

  explicit Epub(std::string cachePath, std::string language = "en")
      : cachePath(std::move(cachePath)), language(std::move(language)) {}

  const std::string& getCachePath() const { return cachePath; }
  const std::string& getLanguage() const { return language; }
  SpineEntry getSpineItem(int) const { return {"chapter.xhtml"}; }
  int getTocIndexForSpineIndex(int) const { return -1; }
  TocEntry getTocItem(int) const { return {}; }
  int getTocItemsCount() const { return 0; }
  CssParser* getCssParser() const { return nullptr; }
  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output&, size_t, bool = false) const {
    return false;
  }

 private:
  std::string cachePath;
  std::string language;
};
