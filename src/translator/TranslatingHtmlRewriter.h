#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

/**
 * SAX-based HTML rewriter that inserts machine-translated paragraphs after
 * each block element in an EPUB chapter.
 *
 * Strategy: reconstruct the original markup from expat callbacks, and after
 * each closing block tag (p, h1-h6, li, blockquote) append a translated
 * paragraph in a grey colour (<p style="color:#5A5A5A">).
 */
class TranslatingHtmlRewriter {
 public:
  struct Result {
    int paragraphsTranslated = 0;
    int paragraphsSkipped = 0;
    bool cancelled = false;
  };

  // Rewrite HTML from `inputBuf` (size `inputSize`) into `out`.
  // Returns summary of what happened.
  Result rewrite(const char* inputBuf, size_t inputSize, Print& out, const char* targetLang,
                 volatile const bool* cancelled);

 private:
  Print* out = nullptr;
  const char* targetLang = nullptr;
  volatile const bool* cancelled = nullptr;

  int depth = 0;
  int blockDepth = -1;   // depth where current block element began; -1 = not in block
  bool insideHead = false;
  bool wroteXmlDecl = false;

  std::string blockHtml;  // Reconstructed markup of current block (for output)
  std::string blockText;  // Plain text of current block (for translation)

  int paragraphsTranslated = 0;
  int paragraphsSkipped = 0;
  bool wasCancelled = false;

  static const char* BLOCK_TAGS[];
  static const int NUM_BLOCK_TAGS;

  static bool isBlockTag(const char* name);

  // Append XML-escaped version of `s` to `buf`
  static void appendEscaped(const char* s, size_t len, std::string& buf);

  // Write to output Print stream
  void writeOut(const char* s, size_t len);
  void writeOut(const std::string& s);

  // Build opening tag string "<name attr1=...>"
  static std::string makeOpenTag(const XML_Char* name, const XML_Char** atts);

  // Called when a block element closes: write block HTML, then translation
  void flushBlock(const char* endTagName);

  static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL onEnd(void* ud, const XML_Char* name);
  static void XMLCALL onChars(void* ud, const XML_Char* s, int len);
  static void XMLCALL onDefault(void* ud, const XML_Char* s, int len);
};
