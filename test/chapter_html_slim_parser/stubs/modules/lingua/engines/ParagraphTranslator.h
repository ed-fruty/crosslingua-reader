#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "modules/lingua/services/TranslationHttpSession.h"

// Host stub: a deterministic "translation" ("UK:" + the text, five times -- translations are
// often longer than their source, and the sidecar tests need ones longer than a page) so sidecar
// tests run offline.
class ParagraphTranslator {
 public:
  static bool translate(const std::string& text, const char*, const char*, uint8_t, const char*, std::string& result,
                        std::string* = nullptr, TranslationHttpSession* = nullptr) {
    result = "UK:" + text;
    for (int i = 0; i < 4; i++) result += " " + text;
    return true;
  }
  static constexpr size_t MAX_TEXT_BYTES = 1800;
  static bool primeAzureToken() { return true; }
  static bool refreshAzureTokenIfExpiring() { return true; }
};
