#include "ParagraphTranslator.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "network/HttpDownloader.h"

static constexpr const char* GTRANSLATE_URL =
    "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&dt=t&tl=";

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

bool ParagraphTranslator::parseResponse(const std::string& json, std::string& result) {
  result.clear();
  // Response format: [[["translated","original",...],["seg2",...],...],...]
  // Collect first string of each inner array in the first outer array.
  size_t pos = json.find("[[[");
  if (pos == std::string::npos) return false;
  pos += 3;

  bool firstSeg = true;
  while (pos < json.size()) {
    if (!firstSeg) {
      // Find next inner array ["   within the first outer array ]]
      size_t dblClose = json.find("]]", pos);
      size_t nextOpen = json.find("[\"", pos);
      if (nextOpen == std::string::npos || (dblClose != std::string::npos && dblClose <= nextOpen)) break;
      pos = nextOpen + 1;  // point at opening "
    }
    firstSeg = false;

    if (pos >= json.size() || json[pos] != '"') break;
    pos++;  // skip opening "

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
        } else if (esc == 'u' && pos + 5 < json.size()) {
          unsigned int cp = 0;
          sscanf(json.c_str() + pos + 2, "%4x", &cp);
          // Handle surrogate pairs (emoji etc.)
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
        pos++;  // skip closing "
        break;
      } else {
        result += c;
        pos++;
      }
    }
  }
  return !result.empty();
}

bool ParagraphTranslator::translate(const std::string& text, const char* targetLang, std::string& result) {
  if (text.size() < 3) {
    result = text;
    return true;
  }
  if (text.size() > MAX_TEXT_BYTES) {
    LOG_ERR("Translator", "Text too long (%u bytes), skipping", (unsigned)text.size());
    return false;
  }

  const std::string url = std::string(GTRANSLATE_URL) + targetLang + "&q=" + urlEncode(text);

  std::string response;
  if (!HttpDownloader::fetchUrl(url, response)) {
    LOG_ERR("Translator", "HTTP fetch failed");
    return false;
  }

  if (!parseResponse(response, result)) {
    LOG_ERR("Translator", "Failed to parse response (%.80s)", response.c_str());
    return false;
  }

  return true;
}
