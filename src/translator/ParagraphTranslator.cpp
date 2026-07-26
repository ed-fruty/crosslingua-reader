#include "ParagraphTranslator.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "network/HttpDownloader.h"

namespace {
// Route an engine's HTTP call through the caller-provided reusable session when
// one is given (one kept-alive TLS connection across many paragraphs), else
// through the stateless HttpDownloader statics (a fresh connection per call —
// the original behavior, preserved for tests and other callers). The static and
// session methods share identical contracts (status handling, lastHttpCode
// semantics), so these are exact drop-ins for the previous direct static calls.
bool httpFetch(TranslationHttpSession* session, const std::string& url, std::string& out) {
  return session ? session->fetchUrl(url, out) : HttpDownloader::fetchUrl(url, out);
}
bool httpPost(TranslationHttpSession* session, const std::string& url, const std::string& body, const char* contentType,
              const char* extraHeaderName, const char* extraHeaderValue, std::string& out) {
  return session ? session->post(url, body, contentType, extraHeaderName, extraHeaderValue, out)
                 : HttpDownloader::post(url, body, contentType, extraHeaderName, extraHeaderValue, out);
}
bool httpPostJson(TranslationHttpSession* session, const std::string& url, const std::string& body,
                  const std::string& authHeader, std::string& out) {
  return session ? session->postJson(url, body, authHeader, out) : HttpDownloader::postJson(url, body, authHeader, out);
}
}  // namespace

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
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 11 < json.size() && json[pos + 6] == '\\' && json[pos + 7] == 'u') {
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
                                         const char* apiKey, bool pro, std::string& result,
                                         TranslationHttpSession* session) {
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
    if (c == '"')
      body += "\\\"";
    else if (c == '\\')
      body += "\\\\";
    else if (c == '\n')
      body += "\\n";
    else if (c == '\r')
      body += "\\r";
    else if (c == '\t')
      body += "\\t";
    else
      body += c;
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
  if (!httpPostJson(session, url, body, auth, response)) {
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
      {"en", "English"},    {"es", "Spanish"},
      {"fr", "French"},     {"de", "German"},
      {"it", "Italian"},    {"pt", "Portuguese"},
      {"nl", "Dutch"},      {"pl", "Polish"},
      {"uk", "Ukrainian"},  {"ru", "Russian"},
      {"cs", "Czech"},      {"sv", "Swedish"},
      {"no", "Norwegian"},  {"fi", "Finnish"},
      {"ro", "Romanian"},   {"hu", "Hungarian"},
      {"tr", "Turkish"},    {"ar", "Arabic"},
      {"ja", "Japanese"},   {"ko", "Korean"},
      {"zh-CN", "Chinese"}, {"zh-TW", "Traditional Chinese"},
      {"hi", "Hindi"},      {"th", "Thai"},
      {"vi", "Vietnamese"}, {"id", "Indonesian"},
      {"el", "Greek"},      {"bg", "Bulgarian"},
      {"hr", "Croatian"},   {"sr", "Serbian"},
      {"sk", "Slovak"},     {"sl", "Slovenian"},
      {"et", "Estonian"},   {"lv", "Latvian"},
      {"lt", "Lithuanian"}, {"ca", "Catalan"},
      {"da", "Danish"},     {"ms", "Malay"},
      {"he", "Hebrew"},     {"fa", "Persian"},
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

bool ParagraphTranslator::translateOpenAICompat(const std::string& text, const char* sourceLang, const char* targetLang,
                                                const char* apiKey, const char* endpoint, const char* model,
                                                std::string& result, TranslationHttpSession* session) {
  LOG_DBG("Translator", "OpenAI-compat: endpoint=%s, model=%s", endpoint, model);

  const bool isBatch = text.find("\n\n") != std::string::npos;
  std::string prompt = buildLlmPrompt(sourceLang, targetLang, isBatch);

  // Build JSON body
  auto jsonEscape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
      if (c == '"')
        out += "\\\"";
      else if (c == '\\')
        out += "\\\\";
      else if (c == '\n')
        out += "\\n";
      else if (c == '\r')
        out += "\\r";
      else if (c == '\t')
        out += "\\t";
      else
        out += c;
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
  if (!httpPostJson(session, endpoint, body, auth, response)) {
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
                                          const char* apiKey, std::string& result, TranslationHttpSession* session) {
  LOG_DBG("Translator", "Gemini: src=%s, tgt=%s", sourceLang, targetLang);

  const bool isBatch = text.find("\n\n") != std::string::npos;
  std::string prompt = buildLlmPrompt(sourceLang, targetLang, isBatch);
  prompt += "\n\n";
  prompt += text;

  auto jsonEscape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
      if (c == '"')
        out += "\\\"";
      else if (c == '\\')
        out += "\\\\";
      else if (c == '\n')
        out += "\\n";
      else if (c == '\r')
        out += "\\r";
      else if (c == '\t')
        out += "\\t";
      else
        out += c;
    }
    return out;
  };

  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=";
  url += apiKey;

  std::string body = "{\"contents\":[{\"parts\":[{\"text\":\"";
  body += jsonEscape(prompt);
  body += "\"}]}]}";

  std::string response;
  if (!httpPostJson(session, url, body, "", response)) {
    LOG_ERR("Translator", "Gemini HTTP POST failed");
    return false;
  }

  if (!parseGeminiResponse(response, result)) {
    LOG_ERR("Translator", "Gemini parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── Google Translate v2 (free, GET-based) ────────────────────────────────────

bool ParagraphTranslator::parseGoogleV2Response(const std::string& json, std::string& result) {
  // Response: {"translation":"...","detectedLanguage":"...","model":"..."}
  size_t pos = json.find("\"translation\"");
  if (pos == std::string::npos) return false;
  pos += 13;  // skip past "translation"
  pos = json.find(':', pos);
  if (pos == std::string::npos) return false;
  pos++;
  return extractJsonStringValue(json, pos, result);
}

bool ParagraphTranslator::translateGoogleV2(const std::string& text, const char* sourceLang, const char* targetLang,
                                            std::string& result, TranslationHttpSession* session) {
  const char* src = (sourceLang && strcmp(sourceLang, "auto") != 0) ? sourceLang : "auto";
  LOG_DBG("Translator", "GoogleV2: src=%s, tgt=%s", src, targetLang);

  static constexpr char kApiKey[] = "AIzaSyDLEeFI5OtFBwYBIoK_jj5m32rZK5CkCXA";

  std::string url = "https://translate-pa.googleapis.com/v1/translate?params.client=gtx&query.source_language=";
  url += src;
  url += "&query.target_language=";
  url += targetLang;
  url += "&query.display_language=en-US&data_types=TRANSLATION&key=";
  url += kApiKey;
  url += "&query.text=";
  url += urlEncode(text);

  std::string response;
  if (!httpFetch(session, url, response)) {
    LOG_ERR("Translator", "GoogleV2 HTTP fetch failed");
    return false;
  }

  if (!parseGoogleV2Response(response, result)) {
    LOG_ERR("Translator", "GoogleV2 parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── Google Translate HTML (free, POST-based) ─────────────────────────────────

bool ParagraphTranslator::parseGoogleHtmlResponse(const std::string& json, std::string& result) {
  // Response: [["translated text"]] — nested JSON array, extract first string at [0][0]
  size_t pos = json.find("[[");
  if (pos == std::string::npos) return false;
  pos += 2;
  return extractJsonStringValue(json, pos - 1, result);
}

bool ParagraphTranslator::translateGoogleHtml(const std::string& text, const char* sourceLang, const char* targetLang,
                                              std::string& result, TranslationHttpSession* session) {
  const char* src = (sourceLang && strcmp(sourceLang, "auto") != 0) ? sourceLang : "auto";
  LOG_DBG("Translator", "GoogleHtml: src=%s, tgt=%s", src, targetLang);

  static constexpr char kApiKey[] = "AIzaSyATBXajvzQLTDHEQbcpq0Ihe0vWDHmO520";
  static constexpr char kEndpoint[] = "https://translate-pa.googleapis.com/v1/translateHtml";

  // Build body: [[["<text>"],"<src>","<tgt>"],"wt_lib"]
  auto jsonEscape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
      if (c == '"')
        out += "\\\"";
      else if (c == '\\')
        out += "\\\\";
      else if (c == '\n')
        out += "\\n";
      else if (c == '\r')
        out += "\\r";
      else if (c == '\t')
        out += "\\t";
      else
        out += c;
    }
    return out;
  };

  std::string body = "[[[\"";
  body += jsonEscape(text);
  body += "\"],\"";
  body += src;
  body += "\",\"";
  body += targetLang;
  body += "\"],\"wt_lib\"]";

  std::string response;
  if (!httpPost(session, kEndpoint, body, "application/json+protobuf", "X-Goog-Api-Key", kApiKey, response)) {
    LOG_ERR("Translator", "GoogleHtml HTTP POST failed");
    return false;
  }

  if (!parseGoogleHtmlResponse(response, result)) {
    LOG_ERR("Translator", "GoogleHtml parse failed (%.80s)", response.c_str());
    return false;
  }
  return true;
}

// ─── Azure (Microsoft Edge translator deployment, keyless) ───────────────────
//
// This targets api-edge.cognitive.microsofttranslator.com — the deployment that
// serves Microsoft Edge's built-in page translator — NOT the paid Azure
// Translator resource at api.cognitive.microsofttranslator.com. The difference
// matters: the paid endpoint requires a subscription key (plus a region header
// for multi-region resources), while this one authenticates with a short-lived
// bearer token handed out by an anonymous public GET. The user therefore
// configures nothing, exactly like the Google engines with their built-in keys.
// The trade-off is that neither endpoint is a documented, supported API, so
// Microsoft can change or withdraw them without notice.
//
// Azure natively accepts an ARRAY of text items per POST and returns one result
// object per item, in input order. That maps onto the rewriter's batch path
// (paragraphs joined with "\n\n") with a hard ordering guarantee, unlike the LLM
// engines already on that path which rely on the model echoing the separators.

namespace {
constexpr char kAzureAuthUrl[] = "https://edge.microsoft.com/translate/auth";
constexpr char kAzureTranslateUrl[] = "https://api-edge.cognitive.microsofttranslator.com/translate";
// The issued JWT lives ~10 minutes; refresh at 8 so a request is never sent with
// a token that expires in flight.
constexpr uint32_t kAzureTokenTtlMs = 8u * 60u * 1000u;

// Function-local statics, so the ~1 KB token string is only ever allocated if the
// Azure engine is actually selected — no permanent .bss cost for the other engines.
std::string& azureTokenCache() {
  static std::string cached;
  return cached;
}
uint32_t& azureTokenExpiresAtMs() {
  static uint32_t expiresAt = 0;
  return expiresAt;
}

void azureInvalidateToken() {
  azureTokenCache().clear();
  azureTokenExpiresAtMs() = 0;
}

// Fetch (or reuse) the anonymous bearer token.
//
// Deliberately uses the STATIC HttpDownloader::fetchUrl rather than the caller's
// TranslationHttpSession: the token host differs from the translate host, so
// routing it through the session would bounce the kept-alive socket between two
// origins (two extra handshakes per chapter) and, worse, would set the session's
// everConnected flag — after which the next request, which genuinely
// re-handshakes against the other host, would be gated by the small reuse heap
// floor instead of the full TLS floor. The static path applies its own
// insufficientHeapForTls() guard, so this handshake is still protected.
bool azureAuthToken(std::string& out) {
  std::string& cached = azureTokenCache();
  const uint32_t now = millis();
  // Signed difference so a millis() rollover (~49 days uptime) reads as expired
  // rather than as valid forever.
  if (!cached.empty() && static_cast<int32_t>(azureTokenExpiresAtMs() - now) > 0) {
    out = cached;
    return true;
  }

  std::string jwt;
  if (!HttpDownloader::fetchUrl(kAzureAuthUrl, jwt)) {
    LOG_ERR("Translator", "Azure: auth token fetch failed");
    return false;
  }
  // The endpoint returns the raw JWT as text/plain; strip any trailing newline so
  // it cannot corrupt the Authorization header.
  while (!jwt.empty() && (jwt.back() == '\n' || jwt.back() == '\r' || jwt.back() == ' ')) {
    jwt.pop_back();
  }
  if (jwt.empty()) {
    LOG_ERR("Translator", "Azure: auth token empty");
    return false;
  }

  cached = std::move(jwt);
  azureTokenExpiresAtMs() = now + kAzureTokenTtlMs;
  LOG_INF("Translator", "Azure: auth token acquired (%u bytes)", (unsigned)cached.size());
  out = cached;
  return true;
}

// Append `len` bytes of `data` to `out`, JSON-string-escaped. Takes a range rather
// than a std::string so the batch splitter can escape each "\n\n"-delimited part
// straight out of the merged text with no intermediate substr() allocation.
void appendJsonEscaped(const char* data, size_t len, std::string& out) {
  for (size_t i = 0; i < len; i++) {
    const char c = data[i];
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else
      out += c;
  }
}
}  // namespace

bool ParagraphTranslator::primeAzureToken() {
  std::string token;
  return azureAuthToken(token);
}

const char* ParagraphTranslator::azureLangCode(const char* code) {
  if (!code || !*code) return "";  // never return null: callers append this straight onto the URL
  struct LangRemap {
    const char* ours;
    const char* azure;
  };
  // Azure's language list uses script/variant subtags for these four; the plain
  // forms we store in LanguagePickerActivity::LANGUAGES are not in its list and
  // are rejected. Every other code we store (ar, bg, ca, cs, da, de, el, en, es,
  // et, fa, fi, fr, he, hi, hr, hu, id, it, ja, ko, lt, lv, ms, nl, pl, pt, ro,
  // ru, sk, sl, sv, th, tr, uk, vi) appears verbatim in Azure's list and passes
  // through unchanged.
  static constexpr LangRemap REMAP[] = {
      {"zh-CN", "zh-Hans"},  // Azure has no zh-CN
      {"zh-TW", "zh-Hant"},  // Azure has no zh-TW
      {"no", "nb"},          // Azure carries Norwegian Bokmal only
      {"sr", "sr-Cyrl"},     // Azure splits Serbian by script; Cyrillic is the default
  };
  for (const auto& r : REMAP) {
    if (strcmp(r.ours, code) == 0) return r.azure;
  }
  return code;
}

bool ParagraphTranslator::parseAzureResponse(const std::string& json, size_t expectedCount, std::string& result) {
  // [{"detectedLanguage":{...},"translations":[{"text":"...","to":"fr"}]}, ...]
  //
  // Manual scan, matching every other parser in this file (no ArduinoJson, no DOM).
  // Anchor on "translations" rather than scanning bare "text" keys: each object also
  // carries "to", and a detectedLanguage sub-object may precede the array. A translated
  // string cannot spoof the anchor, because a quote inside the body is escaped (\") so
  // the literal sequence "translations" (quote-delimited on both sides) cannot occur.
  result.clear();
  size_t pos = 0;
  std::string piece;
  for (size_t i = 0; i < expectedCount; i++) {
    pos = json.find("\"translations\"", pos);
    if (pos == std::string::npos) return false;
    pos += 14;  // past the quoted key
    size_t t = json.find("\"text\"", pos);
    if (t == std::string::npos) return false;
    t = json.find(':', t + 6);
    if (t == std::string::npos) return false;
    const size_t q = json.find('"', t + 1);
    if (q == std::string::npos) return false;
    if (q + 1 < json.size() && json[q + 1] == '"') {
      // A legitimately empty translation. extractJsonStringValue() reports empty as a
      // parse failure, which under the exact-N rule below would sink the whole batch,
      // so recognise "" here instead.
      piece.clear();
    } else if (!extractJsonStringValue(json, q, piece)) {
      return false;
    }
    if (i) result += "\n\n";
    result += piece;
    pos = t;  // the next "translations" necessarily follows this item's value
  }
  // Exactly-N or fail: the rewriter writes translations back POSITIONALLY, so a short
  // reply must not silently shift every later paragraph's translation by one.
  return true;
}

bool ParagraphTranslator::translateAzure(const std::string& text, const char* sourceLang, const char* targetLang,
                                         std::string& result, TranslationHttpSession* session) {
  LOG_DBG("Translator", "Azure: src=%s, tgt=%s", sourceLang ? sourceLang : "auto", targetLang);

  std::string token;
  if (!azureAuthToken(token)) return false;  // already logged

  std::string url = kAzureTranslateUrl;
  url += "?api-version=3.0&to=";
  url += azureLangCode(targetLang);
  // Omitting `from` entirely is what triggers Azure's auto-detect.
  if (sourceLang && *sourceLang && strcmp(sourceLang, "auto") != 0) {
    url += "&from=";
    url += azureLangCode(sourceLang);
  }

  // Body: [{"Text":"..."},...] — one element per "\n\n"-separated part, so the
  // single-paragraph and batch cases share this one path (count == 1 when unbatched).
  // Reserve once for the text plus the ~12-byte-per-item array scaffolding, so the
  // body never reallocates mid-build (each growth is a malloc + copy + free).
  std::string body;
  body.reserve(text.size() + text.size() / 4 + 64);
  body += '[';
  size_t start = 0;
  size_t count = 0;
  while (true) {
    const size_t sep = text.find("\n\n", start);
    const size_t len = (sep == std::string::npos) ? (text.size() - start) : (sep - start);
    if (count) body += ',';
    body += "{\"Text\":\"";
    appendJsonEscaped(text.data() + start, len, body);
    body += "\"}";
    count++;
    if (sep == std::string::npos) break;
    start = sep + 2;
  }
  body += ']';

  std::string auth = "Bearer ";
  auth += token;

  std::string response;
  if (!httpPostJson(session, url, body, auth, response)) {
    const int code = HttpDownloader::lastHttpCode;
    // A rejected token would otherwise stay cached until its 8-minute TTL lapses and
    // fail every subsequent chapter the same way; drop it so the next run refetches.
    // The caller still treats 401/403 as fatal to this chapter (shouldRetryAfterFailure).
    if (code == 401 || code == 403) azureInvalidateToken();
    LOG_ERR("Translator", "Azure HTTP POST failed (code %d)", code);
    return false;
  }

  if (!parseAzureResponse(response, count, result)) {
    LOG_ERR("Translator", "Azure parse failed, expected %u items (%.80s)", (unsigned)count, response.c_str());
    return false;
  }
  return true;
}

// ─── Main dispatch ───────────────────────────────────────────────────────────

bool ParagraphTranslator::translate(const std::string& text, const char* sourceLang, const char* targetLang,
                                    uint8_t engine, const char* apiKey, std::string& result, std::string* errorOut,
                                    TranslationHttpSession* session) {
  LOG_DBG("MEM", "Free heap (pre-translate): %u", (unsigned)ESP.getFreeHeap());
  if (text.size() < 3) {
    result = text;
    return true;
  }
  if (text.size() > MAX_TEXT_BYTES) {
    LOG_ERR("Translator", "Text too long (%u bytes), skipping", (unsigned)text.size());
    if (errorOut) *errorOut = "Text too long";
    return false;
  }

  LOG_DBG("Translator", "Engine=%d, text=%u bytes, src=%s, tgt=%s", engine, (unsigned)text.size(), sourceLang,
          targetLang);

  HttpDownloader::lastHttpCode = 0;
  bool ok = false;
  switch (engine) {
    case CrossPointSettings::ENGINE_GOOGLE_FREE:
      // Legacy free gtx-API engine was removed; route to the maintained Google V2 engine so old
      // saved settings (translationEngine == ENGINE_GOOGLE_FREE == 0) keep working.
      ok = translateGoogleV2(text, sourceLang, targetLang, result, session);
      break;
    case CrossPointSettings::ENGINE_DEEPL:
      ok = translateDeepL(text, sourceLang, targetLang, apiKey, false, result, session);
      break;
    case CrossPointSettings::ENGINE_DEEPL_PRO:
      ok = translateDeepL(text, sourceLang, targetLang, apiKey, true, result, session);
      break;
    case CrossPointSettings::ENGINE_OPENAI:
      ok = translateOpenAICompat(text, sourceLang, targetLang, apiKey, "https://api.openai.com/v1/chat/completions",
                                 "gpt-4o-mini", result, session);
      break;
    case CrossPointSettings::ENGINE_DEEPSEEK:
      ok = translateOpenAICompat(text, sourceLang, targetLang, apiKey, "https://api.deepseek.com/v1/chat/completions",
                                 "deepseek-chat", result, session);
      break;
    case CrossPointSettings::ENGINE_GEMINI:
      ok = translateGemini(text, sourceLang, targetLang, apiKey, result, session);
      break;
    case CrossPointSettings::ENGINE_GOOGLE_V2:
      ok = translateGoogleV2(text, sourceLang, targetLang, result, session);
      break;
    case CrossPointSettings::ENGINE_GOOGLE_HTML:
      ok = translateGoogleHtml(text, sourceLang, targetLang, result, session);
      break;
    case CrossPointSettings::ENGINE_AZURE:
      ok = translateAzure(text, sourceLang, targetLang, result, session);
      break;
    default:
      LOG_ERR("Translator", "Unknown engine: %d", engine);
      if (errorOut) *errorOut = "Unknown engine";
      return false;
  }

  if (ok) {
    LOG_DBG("Translator", "Translation OK, result=%u bytes", (unsigned)result.size());
  } else if (errorOut) {
    // Format error from HTTP code
    int code = HttpDownloader::lastHttpCode;
    char buf[64];
    if (code == 0) {
      snprintf(buf, sizeof(buf), "HTTP request failed");
    } else if (code < 0) {
      snprintf(buf, sizeof(buf), "Connection failed (code %d)", code);
    } else {
      snprintf(buf, sizeof(buf), "HTTP error %d", code);
    }
    *errorOut = buf;
  }
  return ok;
}

bool ParagraphTranslator::translate(const std::string& text, const char* targetLang, std::string& result,
                                    std::string* errorOut, TranslationHttpSession* session) {
  LOG_DBG("MEM", "Free heap (pre-translate): %u", (unsigned)ESP.getFreeHeap());
  return translate(text, "auto", targetLang, SETTINGS.translationEngine, SETTINGS.translateApiKey, result, errorOut,
                   session);
}
