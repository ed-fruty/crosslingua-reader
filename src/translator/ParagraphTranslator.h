#pragma once
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"  // TRANSLATION_ENGINE enum, for engineNeedsApiKey()

class TranslationHttpSession;  // network/HttpDownloader.h — reusable keep-alive connection

/**
 * Translates text paragraphs using configurable translation engines.
 * Supports: Google Free, DeepL, DeepL Pro, OpenAI, DeepSeek, Gemini.
 */
class ParagraphTranslator {
 public:
  // Translate `text` from `sourceLang` to `targetLang` using the specified engine.
  // sourceLang: BCP-47 code or "auto" for auto-detect.
  // Returns true and populates `result` on success.
  // `session`: optional reusable connection. When non-null, the engine's HTTP
  // request(s) go through it (one kept-alive TLS session across many calls);
  // when null (the default), each call uses the stateless HttpDownloader
  // statics — the original per-call behavior, kept for tests and other callers.
  static bool translate(const std::string& text, const char* sourceLang, const char* targetLang, uint8_t engine,
                        const char* apiKey, std::string& result, std::string* errorOut = nullptr,
                        TranslationHttpSession* session = nullptr);

  // Convenience: reads engine/apiKey/sourceLang from SETTINGS
  static bool translate(const std::string& text, const char* targetLang, std::string& result,
                        std::string* errorOut = nullptr, TranslationHttpSession* session = nullptr);

  static constexpr size_t MAX_TEXT_BYTES = 1800;

  // Whether the given engine requires a user-supplied API key. Kept co-located
  // with the engine dispatch in translate() so the two stay in sync when an
  // engine is added or its auth model changes. Grounded in the actual endpoints:
  //   - Google engines (GOOGLE_FREE/GOOGLE_V2/GOOGLE_HTML) ship a built-in
  //     translate-pa key baked into the request — keyless, so `false`.
  //   - DeepL / DeepL Pro / OpenAI / DeepSeek send the user key as auth.
  //   - Gemini appends the user key to the request URL (no built-in key), so it
  //     needs one too.
  static constexpr bool engineNeedsApiKey(uint8_t engine) {
    switch (engine) {
      case CrossPointSettings::ENGINE_DEEPL:
      case CrossPointSettings::ENGINE_DEEPL_PRO:
      case CrossPointSettings::ENGINE_OPENAI:
      case CrossPointSettings::ENGINE_DEEPSEEK:
      case CrossPointSettings::ENGINE_GEMINI:
        return true;
      case CrossPointSettings::ENGINE_GOOGLE_FREE:
      case CrossPointSettings::ENGINE_GOOGLE_V2:
      case CrossPointSettings::ENGINE_GOOGLE_HTML:
      default:
        return false;
    }
  }

 private:
  static std::string urlEncode(const std::string& s);
  static void codePointToUtf8(uint32_t cp, std::string& out);

  // Extract a JSON string value after a key like "text": or "content":
  static bool extractJsonStringValue(const std::string& json, size_t startPos, std::string& result);

  // Per-engine implementations. `session` (may be null) routes the HTTP request
  // through a reusable keep-alive connection; see translate() above.
  static bool translateDeepL(const std::string& text, const char* sourceLang, const char* targetLang,
                             const char* apiKey, bool pro, std::string& result, TranslationHttpSession* session);
  static bool translateOpenAICompat(const std::string& text, const char* sourceLang, const char* targetLang,
                                    const char* apiKey, const char* endpoint, const char* model, std::string& result,
                                    TranslationHttpSession* session);
  static bool translateGemini(const std::string& text, const char* sourceLang, const char* targetLang,
                              const char* apiKey, std::string& result, TranslationHttpSession* session);
  static bool translateGoogleV2(const std::string& text, const char* sourceLang, const char* targetLang,
                                std::string& result, TranslationHttpSession* session);
  static bool translateGoogleHtml(const std::string& text, const char* sourceLang, const char* targetLang,
                                  std::string& result, TranslationHttpSession* session);

  // Response parsers
  static bool parseDeepLResponse(const std::string& json, std::string& result);
  static bool parseOpenAIResponse(const std::string& json, std::string& result);
  static bool parseGeminiResponse(const std::string& json, std::string& result);
  static bool parseGoogleV2Response(const std::string& json, std::string& result);
  static bool parseGoogleHtmlResponse(const std::string& json, std::string& result);

  // Build LLM translation prompt. When batch=true, adds instructions to preserve \n\n separators.
  static std::string buildLlmPrompt(const char* sourceLang, const char* targetLang, bool batch = false);
};
