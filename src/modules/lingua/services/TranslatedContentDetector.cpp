#include "modules/lingua/services/TranslatedContentDetector.h"

#include <Epub/parsers/ParagraphBoundary.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <expat.h>

#include <cstring>

namespace lingua::content {
namespace {

// Read granularity for the scan. Heap-allocated rather than a stack array: the scan runs on the
// render task (whose stack is shared with the layout build) and CLAUDE.md caps function locals at
// 256 bytes. One allocation per scan, and Section memoizes the result so a chapter pays it at most
// once per load.
constexpr size_t SCAN_CHUNK = 512;

bool hasHiddenAttribute(const XML_Char** atts) {
  if (atts == nullptr) return false;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "hidden") == 0) return true;
  }
  return false;
}

struct ScanState {
  const char* bookPrimaryLang = nullptr;
  bool found = false;
  bool hasText = false;
  bool inBody = false;
  unsigned ignoredDepth = 0;
  bool afterBreak = false;
};

void XMLCALL onStartElement(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* state = static_cast<ScanState*>(ud);
  if (strcmp(name, "body") == 0) state->inBody = true;
  // An element with the HTML `hidden` attribute is laid out as display:none by the parser, so
  // neither its text nor a lang= inside it can reach a page.
  if (state->ignoredDepth || strcmp(name, "script") == 0 || strcmp(name, "style") == 0 || hasHiddenAttribute(atts)) {
    ++state->ignoredDepth;
    return;
  }
  if (paraboundary::isHardBreak(name)) state->afterBreak = true;
  const bool block = paraboundary::isContainerBlockTag(name);
  const bool inlineTranslation = state->afterBreak && strcmp(name, "span") == 0;
  if (block) state->afterBreak = false;
  if (state->found || atts == nullptr || (!block && !inlineTranslation)) return;

  // Take the LAST of lang / xml:lang, exactly as ChapterHtmlSlimParser::startElement does, so an
  // element carrying both resolves to the same language in the gate and in the layout engine.
  const XML_Char* langAttr = nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0) {
      langAttr = atts[i + 1];
    }
  }
  if (isTranslatedLangTag(langAttr, state->bookPrimaryLang)) {
    state->found = true;
  }
}

void XMLCALL onEndElement(void* ud, const XML_Char* name) {
  auto* state = static_cast<ScanState*>(ud);
  if (state->ignoredDepth) --state->ignoredDepth;
  if (strcmp(name, "body") == 0) state->inBody = false;
}

void XMLCALL onText(void* ud, const XML_Char* text, const int length) {
  auto* state = static_cast<ScanState*>(ud);
  if (!state->inBody || state->ignoredDepth) return;
  for (int i = 0; i < length; ++i) {
    if (!detail::isAsciiSpace(text[i])) {
      state->afterBreak = false;
      state->hasText = true;
      return;
    }
  }
}

}  // namespace

bool htmlHasTranslatedBlock(const std::string& htmlPath, const std::string& bookPrimaryLang, bool* textless) {
  if (textless) *textless = false;
  // No book language: nothing can be classified as "other than the book's language". Answer
  // without touching the SD card (see the header note on why this is the safe answer). `textless`
  // keeps its false default here rather than earning a scan of its own: its only consumer is the
  // per-chapter fallback gate, which downgrades a book with no language whatever this says.
  if (bookPrimaryLang.empty()) return false;

  HalFile htmlFile;
  if (!Storage.openFileForRead("TRDET", htmlPath, htmlFile)) {
    LOG_ERR("TRDET", "Failed to open %s", htmlPath.c_str());
    return false;
  }
  const size_t fileSize = htmlFile.size();

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    LOG_ERR("TRDET", "OOM: XML parser");
    return false;
  }

  auto buf = makeUniqueNoThrow<char[]>(SCAN_CHUNK);
  if (!buf) {
    LOG_ERR("TRDET", "OOM: %d bytes", static_cast<int>(SCAN_CHUNK));
    XML_ParserFree(parser);
    return false;
  }

  ScanState state;
  state.bookPrimaryLang = bookPrimaryLang.c_str();
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStartElement, onEndElement);
  XML_SetCharacterDataHandler(parser, onText);
  bool complete = false;

  size_t totalRead = 0;
  // Stops at the first translated block: a bilingual chapter normally answers within the first
  // couple of paragraphs, so only a genuinely monolingual chapter reads to EOF.
  while (totalRead < fileSize && !state.found) {
    const size_t toRead = ((fileSize - totalRead) < SCAN_CHUNK) ? (fileSize - totalRead) : SCAN_CHUNK;
    const int bytesRead = htmlFile.read(reinterpret_cast<uint8_t*>(buf.get()), toRead);
    if (bytesRead <= 0) break;
    totalRead += bytesRead;
    const bool done = (totalRead >= fileSize);
    // A malformed tail is not a reason to claim a translation: stop and report what was seen so
    // far. Chapter HTML that expat rejects would fail the layout parse too.
    if (XML_Parse(parser, buf.get(), bytesRead, done ? 1 : 0) == XML_STATUS_ERROR) break;
    complete = done;
  }

  if (textless) *textless = complete && !state.hasText && !state.found;
  XML_ParserFree(parser);
  LOG_DBG("TRDET", "%s: embedded translation = %s (scanned %u/%u bytes)", htmlPath.c_str(), state.found ? "yes" : "no",
          static_cast<unsigned>(totalRead), static_cast<unsigned>(fileSize));
  return state.found;
}

}  // namespace lingua::content
