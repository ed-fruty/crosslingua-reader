#include "ParagraphTranslator.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "network/HttpDownloader.h"

// ─── URL encoding ────────────────────────────────────────────────────────────

void ParagraphTranslator::codePointToUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string ParagraphTranslator::urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// ─── JSON string extraction helper ──────────────────────────────────────────

bool ParagraphTranslator::extractJsonStringValue(const std::string& json, size_t startPos, std::string& result) {
  // Find opening quote after startPos
  size_t pos = json.find('"', startPos);
  if (pos == std::string::npos) return false;
  pos++;  // skip opening "

  result.clear();
  while (pos < json.size()) {
    char c = json[pos];
    if (c == '\\' && pos + 1 < json.size()) {
      char esc = json[pos + 1];
      if (esc == 'n') {
        result += '\n';
      } else if (esc == 't') {
        result += '\t';
      } else if (esc == '"') {
        result += '"';
      } else if (esc == '\\') {
        result += '\\';
      } else if (esc == '/') {
        result += '/';
      } else if (esc == 'u' && pos + 5 < json.size()) {
        unsigned int cp = 0;
        sscanf(json.c_str() + pos + 2, "%4x", &cp);
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 11 < json.size() && json[pos + 6] == '\\' &&
            json[pos + 7] == 'u') {
          unsigned int low = 0;
          sscanf(json.c_str() + pos + 8, "%4x", &low);
          if (low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            codePointToUtf8(cp, result);
            pos += 10;
          } else {
            codePointToUtf8(cp, result);
            pos += 4;
          }
        } else {
          codePointToUtf8(cp, result);
          pos += 4;
        }
      } else {
        result += esc;
      }
      pos += 2;
    } else if (c == '"') {
      return !result.empty();
    } else {
      result += c;
      pos++;
    }
  }
  return false;
}

// ─── Google Translate (free gtx API) ─────────────────────────────────────────

bool ParagraphTranslator::parseGoogleResponse(const std::string& json, std::string& result) {
  result.clear();
  size_t pos = json.find("[[[");
  if (pos == std::string::npos) return false;
  pos += 3;

  bool firstSeg = true;
  while (pos < json.size()) {
    if (!firstSeg) {
      size_t dblClose = json.find("]]", pos);
      size_t nextOpen = json.find("[\"", pos);
      if (nextOpen == std::string::npos || (dblClose != std::string::npos && dblClose <= nextOpen)) break;
      pos = nextOpen + 1;
    }
    firstSeg = false;

    if (pos >= json.size() || json[pos] != '"') break;

    // Use the shared extraction starting from pos (which points at the opening ")
    std::string segment;
    if (extractJsonStringValue(json, pos - 1, segment)) {
      result += segment;
    }
    // Skip past the closing " of this segment and find next
    size_t closeQuote = pos + 1;
    int depth = 0;
    while (closeQuote < json.size()) {
      if (json[closeQuote] == '\\') {
        closeQuote += 2;
        continue;
      }
      if (json[closeQuote] == '"') break;
      closeQuote++;
    }
    pos = closeQuote + 1;
  }
  return !result.empty();
}

bool ParagraphTranslator::translateGoogle(const std::string& text, const char* sourceLang, const char* targetLang,
                                          std::string& result) {
  const char* src = (sourceLang && strcmp(sourceLang, "auto") != 0) ? sourceLang : "auto";
  std::string url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=";
  url += src;
  url += "&dt=t&tl=";
  url += targetLang;
  url += "&q=";
  url += urlEncode(text);

  LOG_DBG("Translator", "Google: src=%s, tgt=%s", src, targetLang);

  std::string response;
  if (!HttpDownloader::fetchUrl(url, response)) {
    LOG_ERR("Translator", "Google HTTP fetch failed");
    return false;
  }

  if (!parseGoogleResponse(response, result)) {
    LOG_ERR("Translator", "Google parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── DeepL ───────────────────────────────────────────────────────────────────

bool ParagraphTranslator::parseDeepLResponse(const std::string& json, std::string& result) {
  // {"translations":[{"text":"..."}]}
  size_t pos = json.find("\"text\"");
  if (pos == std::string::npos) return false;
  pos += 6;  // skip past "text"
  // Find the colon
  pos = json.find(':', pos);
  if (pos == std::string::npos) return false;
  pos++;
  return extractJsonStringValue(json, pos, result);
}

bool ParagraphTranslator::translateDeepL(const std::string& text, const char* sourceLang, const char* targetLang,
                                          const char* apiKey, bool pro, std::string& result) {
  const char* host = pro ? "https://api.deepl.com" : "https://api-free.deepl.com";
  std::string url = std::string(host) + "/v2/translate";

  LOG_DBG("Translator", "DeepL%s: src=%s, tgt=%s", pro ? " Pro" : "", sourceLang, targetLang);

  // Build JSON body — need uppercase target lang for DeepL
  std::string tgtUpper;
  for (const char* p = targetLang; *p; p++) {
    tgtUpper += static_cast<char>(toupper(*p));
  }

  std::string body = "{\"text\":[\"";
  // Escape the text for JSON
  for (char c : text) {
    if (c == '"') body += "\\\"";
    else if (c == '\\') body += "\\\\";
    else if (c == '\n') body += "\\n";
    else if (c == '\r') body += "\\r";
    else if (c == '\t') body += "\\t";
    else body += c;
  }
  body += "\"],\"target_lang\":\"";
  body += tgtUpper;
  body += "\"";

  if (sourceLang && strcmp(sourceLang, "auto") != 0) {
    std::string srcUpper;
    for (const char* p = sourceLang; *p; p++) {
      srcUpper += static_cast<char>(toupper(*p));
    }
    body += ",\"source_lang\":\"";
    body += srcUpper;
    body += "\"";
  }
  body += "}";

  std::string auth = "DeepL-Auth-Key ";
  auth += apiKey;

  std::string response;
  if (!HttpDownloader::postJson(url, body, auth, response)) {
    LOG_ERR("Translator", "DeepL HTTP POST failed");
    return false;
  }

  if (!parseDeepLResponse(response, result)) {
    LOG_ERR("Translator", "DeepL parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── OpenAI-compatible (OpenAI, DeepSeek) ────────────────────────────────────

bool ParagraphTranslator::parseOpenAIResponse(const std::string& json, std::string& result) {
  // {"choices":[{"message":{"content":"..."}}]}
  size_t pos = json.find("\"content\"");
  if (pos == std::string::npos) return false;
  pos += 9;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return false;
  pos++;
  return extractJsonStringValue(json, pos, result);
}

std::string ParagraphTranslator::buildLlmPrompt(const char* sourceLang, const char* targetLang, bool batch) {
  // Map common language codes to display names for the prompt
  struct LangName {
    const char* code;
    const char* name;
  };
  static const LangName LANG_NAMES[] = {
      {"en", "English"},     {"es", "Spanish"},     {"fr", "French"},       {"de", "German"},
      {"it", "Italian"},     {"pt", "Portuguese"},   {"nl", "Dutch"},       {"pl", "Polish"},
      {"uk", "Ukrainian"},   {"ru", "Russian"},      {"cs", "Czech"},       {"sv", "Swedish"},
      {"no", "Norwegian"},   {"fi", "Finnish"},      {"ro", "Romanian"},    {"hu", "Hungarian"},
      {"tr", "Turkish"},     {"ar", "Arabic"},       {"ja", "Japanese"},    {"ko", "Korean"},
      {"zh-CN", "Chinese"},  {"zh-TW", "Traditional Chinese"}, {"hi", "Hindi"},
      {"th", "Thai"},        {"vi", "Vietnamese"},   {"id", "Indonesian"},  {"el", "Greek"},
      {"bg", "Bulgarian"},   {"hr", "Croatian"},     {"sr", "Serbian"},     {"sk", "Slovak"},
      {"sl", "Slovenian"},   {"et", "Estonian"},     {"lv", "Latvian"},     {"lt", "Lithuanian"},
      {"ca", "Catalan"},     {"da", "Danish"},       {"ms", "Malay"},       {"he", "Hebrew"},
      {"fa", "Persian"},
  };

  const char* tgtName = targetLang;
  const char* srcName = nullptr;
  for (const auto& ln : LANG_NAMES) {
    if (strcmp(ln.code, targetLang) == 0) tgtName = ln.name;
    if (sourceLang && strcmp(ln.code, sourceLang) == 0) srcName = ln.name;
  }

  std::string prompt;
  if (sourceLang && strcmp(sourceLang, "auto") != 0 && srcName) {
    prompt = "Translate the following text from ";
    prompt += srcName;
    prompt += " to ";
    prompt += tgtName;
  } else {
    prompt = "Translate the following text to ";
    prompt += tgtName;
  }
  if (batch) {
    prompt +=
        ". The text contains multiple paragraphs separated by blank lines. "
        "Translate each paragraph and keep the blank line separators in your output. "
        "Output only the translations, nothing else.";
  } else {
    prompt += ". Output only the translation, nothing else.";
  }
  return prompt;
}

bool ParagraphTranslator::translateOpenAICompat(const std::string& text, const char* sourceLang,
                                                 const char* targetLang, const char* apiKey, const char* endpoint,
                                                 const char* model, std::string& result) {
  LOG_DBG("Translator", "OpenAI-compat: endpoint=%s, model=%s", endpoint, model);

  const bool isBatch = text.find("\n\n") != std::string::npos;
  std::string prompt = buildLlmPrompt(sourceLang, targetLang, isBatch);

  // Build JSON body
  auto jsonEscape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
      if (c == '"') out += "\\\"";
      else if (c == '\\') out += "\\\\";
      else if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else if (c == '\t') out += "\\t";
      else out += c;
    }
    return out;
  };

  std::string body = "{\"model\":\"";
  body += model;
  body += "\",\"messages\":[{\"role\":\"system\",\"content\":\"";
  body += jsonEscape(prompt);
  body += "\"},{\"role\":\"user\",\"content\":\"";
  body += jsonEscape(text);
  body += "\"}]}";

  std::string auth = "Bearer ";
  auth += apiKey;

  std::string response;
  if (!HttpDownloader::postJson(endpoint, body, auth, response)) {
    LOG_ERR("Translator", "OpenAI-compat HTTP POST failed");
    return false;
  }

  if (!parseOpenAIResponse(response, result)) {
    LOG_ERR("Translator", "OpenAI-compat parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── Gemini ──────────────────────────────────────────────────────────────────

bool ParagraphTranslator::parseGeminiResponse(const std::string& json, std::string& result) {
  // {"candidates":[{"content":{"parts":[{"text":"..."}]}}]}
  size_t pos = json.find("\"text\"");
  if (pos == std::string::npos) return false;
  pos += 6;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return false;
  pos++;
  return extractJsonStringValue(json, pos, result);
}

bool ParagraphTranslator::translateGemini(const std::string& text, const char* sourceLang, const char* targetLang,
                                           const char* apiKey, std::string& result) {
  LOG_DBG("Translator", "Gemini: src=%s, tgt=%s", sourceLang, targetLang);

  const bool isBatch = text.find("\n\n") != std::string::npos;
  std::string prompt = buildLlmPrompt(sourceLang, targetLang, isBatch);
  prompt += "\n\n";
  prompt += text;

  auto jsonEscape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
      if (c == '"') out += "\\\"";
      else if (c == '\\') out += "\\\\";
      else if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else if (c == '\t') out += "\\t";
      else out += c;
    }
    return out;
  };

  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=";
  url += apiKey;

  std::string body = "{\"contents\":[{\"parts\":[{\"text\":\"";
  body += jsonEscape(prompt);
  body += "\"}]}]}";

  std::string response;
  if (!HttpDownloader::postJson(url, body, "", response)) {
    LOG_ERR("Translator", "Gemini HTTP POST failed");
    return false;
  }

  if (!parseGeminiResponse(response, result)) {
    LOG_ERR("Translator", "Gemini parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── Main dispatch ───────────────────────────────────────────────────────────

bool ParagraphTranslator::translate(const std::string& text, const char* sourceLang, const char* targetLang,
                                     uint8_t engine, const char* apiKey, std::string& result) {
  if (text.size() < 3) {
    result = text;
    return true;
  }
  if (text.size() > MAX_TEXT_BYTES) {
    LOG_ERR("Translator", "Text too long (%u bytes), skipping", (unsigned)text.size());
    return false;
  }

  LOG_DBG("Translator", "Engine=%d, text=%u bytes, src=%s, tgt=%s", engine, (unsigned)text.size(), sourceLang,
          targetLang);

  bool ok = false;
  switch (engine) {
    case CrossPointSettings::ENGINE_GOOGLE_FREE:
      ok = translateGoogle(text, sourceLang, targetLang, result);
      break;
    case CrossPointSettings::ENGINE_DEEPL:
      ok = translateDeepL(text, sourceLang, targetLang, apiKey, false, result);
      break;
    case CrossPointSettings::ENGINE_DEEPL_PRO:
      ok = translateDeepL(text, sourceLang, targetLang, apiKey, true, result);
      break;
    case CrossPointSettings::ENGINE_OPENAI:
      ok = translateOpenAICompat(text, sourceLang, targetLang, apiKey,
                                  "https://api.openai.com/v1/chat/completions", "gpt-4o-mini", result);
      break;
    case CrossPointSettings::ENGINE_DEEPSEEK:
      ok = translateOpenAICompat(text, sourceLang, targetLang, apiKey,
                                  "https://api.deepseek.com/v1/chat/completions", "deepseek-chat", result);
      break;
    case CrossPointSettings::ENGINE_GEMINI:
      ok = translateGemini(text, sourceLang, targetLang, apiKey, result);
      break;
    default:
      LOG_ERR("Translator", "Unknown engine: %d", engine);
      return false;
  }

  if (ok) {
    LOG_DBG("Translator", "Translation OK, result=%u bytes", (unsigned)result.size());
  }
  return ok;
}

bool ParagraphTranslator::translate(const std::string& text, const char* targetLang, std::string& result) {
  return translate(text, "auto", targetLang, SETTINGS.translationEngine, SETTINGS.translateApiKey, result);
}
