#pragma once
#include <cstdint>
#include <string>

/**
 * Translates text paragraphs using configurable translation engines.
 * Supports: Google Free, DeepL, DeepL Pro, OpenAI, DeepSeek, Gemini.
 */
class ParagraphTranslator {
 public:
  // Translate `text` from `sourceLang` to `targetLang` using the specified engine.
  // sourceLang: BCP-47 code or "auto" for auto-detect.
  // Returns true and populates `result` on success.
  static bool translate(const std::string& text, const char* sourceLang, const char* targetLang, uint8_t engine,
                        const char* apiKey, std::string& result);

  // Convenience: reads engine/apiKey/sourceLang from SETTINGS
  static bool translate(const std::string& text, const char* targetLang, std::string& result);

  static constexpr size_t MAX_TEXT_BYTES = 1800;

 private:
  static std::string urlEncode(const std::string& s);
  static void codePointToUtf8(uint32_t cp, std::string& out);

  // Extract a JSON string value after a key like "text": or "content":
  static bool extractJsonStringValue(const std::string& json, size_t startPos, std::string& result);

  // Per-engine implementations
  static bool translateGoogle(const std::string& text, const char* sourceLang, const char* targetLang,
                              std::string& result);
  static bool translateDeepL(const std::string& text, const char* sourceLang, const char* targetLang,
                             const char* apiKey, bool pro, std::string& result);
  static bool translateOpenAICompat(const std::string& text, const char* sourceLang, const char* targetLang,
                                    const char* apiKey, const char* endpoint, const char* model,
                                    std::string& result);
  static bool translateGemini(const std::string& text, const char* sourceLang, const char* targetLang,
                              const char* apiKey, std::string& result);

  // Response parsers
  static bool parseGoogleResponse(const std::string& json, std::string& result);
  static bool parseDeepLResponse(const std::string& json, std::string& result);
  static bool parseOpenAIResponse(const std::string& json, std::string& result);
  static bool parseGeminiResponse(const std::string& json, std::string& result);

  // Build LLM translation prompt. When batch=true, adds instructions to preserve \n\n separators.
  static std::string buildLlmPrompt(const char* sourceLang, const char* targetLang, bool batch = false);
};
