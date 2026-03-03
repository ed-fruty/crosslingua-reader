#include "TranslatingHtmlRewriter.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "ParagraphTranslator.h"

static constexpr size_t PARSE_CHUNK = 1024;

const char* TranslatingHtmlRewriter::BLOCK_TAGS[] = {"p",   "h1",        "h2",        "h3",
                                                      "h4",  "h5",        "h6",        "li",
                                                      "blockquote"};
const int TranslatingHtmlRewriter::NUM_BLOCK_TAGS =
    static_cast<int>(sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]));

bool TranslatingHtmlRewriter::isBlockTag(const char* name) {
  for (int i = 0; i < NUM_BLOCK_TAGS; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

void TranslatingHtmlRewriter::appendEscaped(const char* s, size_t len, std::string& buf) {
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '&') buf += "&amp;";
    else if (c == '<') buf += "&lt;";
    else if (c == '>') buf += "&gt;";
    else buf += c;
  }
}

void TranslatingHtmlRewriter::writeOut(const char* s, size_t len) {
  out->write(reinterpret_cast<const uint8_t*>(s), len);
}

void TranslatingHtmlRewriter::writeOut(const std::string& s) {
  if (!s.empty()) writeOut(s.c_str(), s.size());
}

std::string TranslatingHtmlRewriter::makeOpenTag(const XML_Char* name, const XML_Char** atts) {
  std::string tag = "<";
  tag += name;
  if (atts) {
    for (int i = 0; atts[i]; i += 2) {
      tag += ' ';
      tag += atts[i];  // attribute name
      tag += "=\"";
      for (const char* v = atts[i + 1]; *v; v++) {
        if (*v == '"') tag += "&quot;";
        else if (*v == '&') tag += "&amp;";
        else if (*v == '<') tag += "&lt;";
        else tag += *v;
      }
      tag += '"';
    }
  }
  tag += '>';
  return tag;
}

void TranslatingHtmlRewriter::flushBlock(const char* endTagName) {
  // Write the original block HTML (already reconstructed in blockHtml)
  writeOut(blockHtml);
  writeOut("</", 2);
  writeOut(endTagName, strlen(endTagName));
  writeOut(">\n", 2);

  // Try to translate the plain text we collected
  const std::string trimmed = [&] {
    size_t s = blockText.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return std::string{};
    size_t e = blockText.find_last_not_of(" \t\n\r");
    return blockText.substr(s, e - s + 1);
  }();

  if (!trimmed.empty() && trimmed.size() <= ParagraphTranslator::MAX_TEXT_BYTES) {
    std::string translated;
    // Retry up to 2 times
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (attempt > 0) delay(500);
      ok = ParagraphTranslator::translate(trimmed, sourceLang, targetLang, engine, apiKey, translated);
    }
    if (ok && !translated.empty() && translated != trimmed) {
      // Write translation paragraph with lang + data-translation attrs (grey, auto direction)
      static constexpr char kTagOpen[] = "<p lang=\"";
      static constexpr char kTagAttrs[] = "\" data-translation=\"true\" dir=\"auto\" style=\"color:#5A5A5A\">";
      writeOut(kTagOpen, sizeof(kTagOpen) - 1);
      writeOut(targetLang, strlen(targetLang));
      writeOut(kTagAttrs, sizeof(kTagAttrs) - 1);
      // Write escaped translation text
      std::string escaped;
      appendEscaped(translated.c_str(), translated.size(), escaped);
      writeOut(escaped);
      writeOut("</p>\n", 5);
      paragraphsTranslated++;
    } else {
      paragraphsSkipped++;
    }
    delay(100);  // rate-limit between requests
  } else {
    paragraphsSkipped++;
  }

  LOG_DBG("HtmlRW", "Block <%s> text=%u bytes, translated=%d skipped=%d", endTagName, (unsigned)blockText.size(),
          paragraphsTranslated, paragraphsSkipped);

  if (progressOut) *progressOut = paragraphsTranslated + paragraphsSkipped;

  blockHtml.clear();
  blockText.clear();
  blockDepth = -1;
}

void XMLCALL TranslatingHtmlRewriter::onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  if (self->cancelled && *self->cancelled) {
    self->wasCancelled = true;
    return;
  }

  if (strcmp(name, "head") == 0) self->insideHead = true;

  const std::string tag = makeOpenTag(name, atts);

  if (self->blockDepth != -1) {
    // Inside a block element: accumulate
    self->blockHtml += tag;
    // Also track plain text for inner inline tags (no text here, just tag)
  } else if (!self->insideHead && isBlockTag(name)) {
    // Start of a new block element
    self->blockDepth = self->depth;
    self->blockHtml = tag;
    self->blockText.clear();
  } else {
    // Structural / head element: pass through directly
    self->writeOut(tag);
  }

  self->depth++;
}

void XMLCALL TranslatingHtmlRewriter::onEnd(void* ud, const XML_Char* name) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  self->depth--;

  if (self->blockDepth == self->depth) {
    // Closing the outermost block element we're tracking
    if (self->cancelled && *self->cancelled) {
      self->wasCancelled = true;
      self->blockHtml.clear();
      self->blockText.clear();
      self->blockDepth = -1;
    } else {
      self->flushBlock(name);
    }
  } else if (self->blockDepth != -1) {
    // Closing an inner element within a block
    self->blockHtml += "</";
    self->blockHtml += name;
    self->blockHtml += '>';
  } else {
    // Structural close: pass through
    if (strcmp(name, "head") == 0) self->insideHead = false;
    self->writeOut("</", 2);
    self->writeOut(name, strlen(name));
    self->writeOut(">", 1);
    if (strcmp(name, "html") == 0) self->writeOut("\n", 1);
  }
}

void XMLCALL TranslatingHtmlRewriter::onChars(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  if (self->blockDepth != -1) {
    // Inside a block: add to both HTML output and plain text
    appendEscaped(s, static_cast<size_t>(len), self->blockHtml);
    self->blockText.append(s, static_cast<size_t>(len));
  } else if (!self->insideHead) {
    // Outside blocks (e.g. whitespace between tags): pass through escaped
    std::string escaped;
    appendEscaped(s, static_cast<size_t>(len), escaped);
    self->writeOut(escaped);
  }
}

void XMLCALL TranslatingHtmlRewriter::onDefault(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  // Handle HTML entities and pass through XML declarations / DOCTYPE
  if (len >= 2 && s[0] == '&' && s[len - 1] == ';') {
    // Entity reference — pass through as-is (expat already tried to expand)
    if (self->blockDepth != -1) {
      self->blockHtml.append(s, static_cast<size_t>(len));
      // For plain text, strip the entity name (approximate: use space)
      self->blockText += ' ';
    } else if (!self->insideHead) {
      self->writeOut(s, static_cast<size_t>(len));
    }
    return;
  }
  // Pass through everything else (XML declaration, DOCTYPE, PIs) verbatim
  if (self->blockDepth == -1) {
    self->writeOut(s, static_cast<size_t>(len));
  }
}

// ─── Block counting (for progress bar) ──────────────────────────────────────

void XMLCALL TranslatingHtmlRewriter::onStartCount(void* ud, const XML_Char* name, const XML_Char** atts) {
  (void)atts;
  auto* count = static_cast<int*>(ud);
  if (isBlockTag(name)) {
    (*count)++;
  }
}

int TranslatingHtmlRewriter::countBlocksInFile(const std::string& inputPath) {
  FsFile inputFile;
  if (!Storage.openFileForRead("HtmlRW", inputPath, inputFile)) {
    LOG_ERR("HtmlRW", "countBlocks: failed to open %s", inputPath.c_str());
    return 0;
  }

  const size_t fileSize = inputFile.size();

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    inputFile.close();
    return 0;
  }

  int blockCount = 0;
  XML_SetUserData(parser, &blockCount);
  XML_SetStartElementHandler(parser, onStartCount);

  char buf[PARSE_CHUNK];
  size_t totalRead = 0;
  while (totalRead < fileSize) {
    const size_t toRead = ((fileSize - totalRead) < PARSE_CHUNK) ? (fileSize - totalRead) : PARSE_CHUNK;
    const int bytesRead = inputFile.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (bytesRead <= 0) break;
    totalRead += bytesRead;
    const bool done = (totalRead >= fileSize);
    if (XML_Parse(parser, buf, bytesRead, done ? 1 : 0) == XML_STATUS_ERROR) break;
  }

  XML_ParserFree(parser);
  inputFile.close();

  LOG_DBG("HtmlRW", "Pre-scan: %d translatable blocks in %s", blockCount, inputPath.c_str());
  return blockCount;
}

// ─── rewrite (buffer) ───────────────────────────────────────────────────────

TranslatingHtmlRewriter::Result TranslatingHtmlRewriter::rewrite(const char* inputBuf, size_t inputSize,
                                                                   Print& outPrint, const char* srcLang,
                                                                   const char* tgtLang, uint8_t eng,
                                                                   const char* key,
                                                                   volatile const bool* cancelFlag,
                                                                   volatile int* progress) {
  out = &outPrint;
  sourceLang = srcLang;
  targetLang = tgtLang;
  engine = eng;
  apiKey = key;
  cancelled = cancelFlag;
  progressOut = progress;
  depth = 0;
  blockDepth = -1;
  insideHead = false;
  blockHtml.clear();
  blockText.clear();
  paragraphsTranslated = 0;
  paragraphsSkipped = 0;
  wasCancelled = false;

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    LOG_ERR("HtmlRW", "Failed to create expat parser");
    return {0, 0, false};
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onChars);
  XML_SetDefaultHandlerExpand(parser, onDefault);

  size_t offset = 0;
  bool ok = true;
  while (offset < inputSize && !wasCancelled) {
    size_t chunk = (inputSize - offset < PARSE_CHUNK) ? (inputSize - offset) : PARSE_CHUNK;
    bool done = (offset + chunk >= inputSize);

    if (XML_Parse(parser, inputBuf + offset, static_cast<int>(chunk), done ? 1 : 0) == XML_STATUS_ERROR) {
      LOG_ERR("HtmlRW", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      ok = false;
      break;
    }
    offset += chunk;
  }

  XML_ParserFree(parser);

  Result res;
  res.paragraphsTranslated = paragraphsTranslated;
  res.paragraphsSkipped = paragraphsSkipped;
  res.cancelled = wasCancelled || (cancelled && *cancelled);
  return res;
}

// ─── rewriteFromFile ────────────────────────────────────────────────────────

TranslatingHtmlRewriter::Result TranslatingHtmlRewriter::rewriteFromFile(const std::string& inputPath,
                                                                          Print& outPrint, const char* srcLang,
                                                                          const char* tgtLang, uint8_t eng,
                                                                          const char* key,
                                                                          volatile const bool* cancelFlag,
                                                                          volatile int* progress) {
  out = &outPrint;
  sourceLang = srcLang;
  targetLang = tgtLang;
  engine = eng;
  apiKey = key;
  cancelled = cancelFlag;
  progressOut = progress;
  depth = 0;
  blockDepth = -1;
  insideHead = false;
  blockHtml.clear();
  blockText.clear();
  paragraphsTranslated = 0;
  paragraphsSkipped = 0;
  wasCancelled = false;

  FsFile inputFile;
  if (!Storage.openFileForRead("HtmlRW", inputPath, inputFile)) {
    LOG_ERR("HtmlRW", "Failed to open input file: %s", inputPath.c_str());
    return {0, 0, false};
  }

  const size_t fileSize = inputFile.size();

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    LOG_ERR("HtmlRW", "Failed to create expat parser");
    inputFile.close();
    return {0, 0, false};
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onChars);
  XML_SetDefaultHandlerExpand(parser, onDefault);

  char buf[PARSE_CHUNK];
  size_t totalRead = 0;
  bool ok = true;
  while (totalRead < fileSize && !wasCancelled) {
    const size_t toRead = ((fileSize - totalRead) < PARSE_CHUNK) ? (fileSize - totalRead) : PARSE_CHUNK;
    const int bytesRead = inputFile.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (bytesRead <= 0) {
      LOG_ERR("HtmlRW", "Read error at offset %u", totalRead);
      ok = false;
      break;
    }
    totalRead += bytesRead;
    const bool done = (totalRead >= fileSize);

    if (XML_Parse(parser, buf, bytesRead, done ? 1 : 0) == XML_STATUS_ERROR) {
      LOG_ERR("HtmlRW", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      ok = false;
      break;
    }
  }

  XML_ParserFree(parser);
  inputFile.close();

  Result res;
  res.paragraphsTranslated = paragraphsTranslated;
  res.paragraphsSkipped = paragraphsSkipped;
  res.cancelled = wasCancelled || (cancelled && *cancelled);
  return res;
}
