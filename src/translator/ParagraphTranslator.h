#pragma once
#include <string>

/**
 * Translates text paragraphs using the Google Translate free (gtx) API.
 * No API key required. Source language is auto-detected.
 */
class ParagraphTranslator {
 public:
  // Translate `text` to `targetLang` (BCP-47 code, e.g. "uk", "ru", "es").
  // Returns true and populates `result` on success.
  // Texts longer than MAX_TEXT_BYTES are rejected (returns false).
  static bool translate(const std::string& text, const char* targetLang, std::string& result);

  static constexpr size_t MAX_TEXT_BYTES = 1800;

 private:
  static std::string urlEncode(const std::string& s);
  static bool parseResponse(const std::string& json, std::string& result);
  static void codePointToUtf8(uint32_t cp, std::string& out);
};
